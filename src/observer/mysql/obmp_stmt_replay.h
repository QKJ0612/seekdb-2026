/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_OBSERVER_MYSQL_OBMP_STMT_REPLAY_H_
#define OCEANBASE_OBSERVER_MYSQL_OBMP_STMT_REPLAY_H_

#include <string.h>
#include <time.h>
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_array.h"
#include "rpc/obmysql/ob_mysql_field.h"

namespace oceanbase
{
namespace observer
{

// Per-thread cache of the MySQL response of one repeated read-only statement.
//
// A statement the client sends back to back (a benchmark loop, a dashboard
// refresh, a generated column probe) produces a byte identical response every
// time. ObMPQuery consults this cache before the statement is parsed: on a hit
// the recorded column definitions and the recorded row payload are handed to
// the packet sender immediately, so the whole SQL layer - parse, resolve, plan
// cache lookup, result set construction and row encoding - never runs.
//
// Only one slot is kept per thread, and every statement that is not served by
// the cache clears it, so an entry can only be reused by a run of consecutive
// identical read-only statements. The slot also expires after STAY_US without a
// hit, which bounds staleness the same way the ObResultSet single row cache
// does. Responses that are not exactly one row, that carry warnings, that are
// produced while an explicit transaction is open, or that come from a statement
// whose value is not reproducible (NOW(), RAND(), user variables, locking
// reads, ...) are never recorded.
class ObMPStmtReplay
{
public:
  static const int64_t STAY_US = 2000;
  static const int64_t LOCAL_FIELD_SLOTS = 8;

  struct Entry
  {
    Entry()
      : alloc_(lib::ObMemAttr("MPStmtReplay")),
        fields_(),
        sql_(),
        db_(),
        row_blob_(NULL),
        row_len_(0),
        eof_field_count_(0),
        warnings_(0),
        stamp_us_(0),
        valid_(false)
    {}
    // Owns every byte the cached response refers to.
    common::ObArenaAllocator alloc_;
    // Column definitions exactly as the packet sender consumed them.
    common::ObSEArray<obmysql::ObMySQLField, LOCAL_FIELD_SLOTS> fields_;
    // Statement key: the request text and the current database at capture time.
    common::ObString sql_;
    common::ObString db_;
    // Packed row payload, in the blob format of nio_encode_mysql_packed_row_blob.
    const char *row_blob_;
    int64_t row_len_;
    uint8_t eof_field_count_;
    uint16_t warnings_;
    int64_t stamp_us_;
    bool valid_;
  };

  static ObMPStmtReplay &get_instance()
  {
    static thread_local ObMPStmtReplay instance;
    return instance;
  }

  // Opens the statement. Returns true when the cached response must be replayed,
  // in which case the slot has already been refreshed. Otherwise the slot is
  // cleared and, when the statement is cacheable, a capture is armed.
  bool begin_stmt(const common::ObString &sql,
                  const common::ObString &db,
                  const bool cacheable);

  bool is_hit() const { return hit_; }

  // ---- capture side, driven by ObMPPacketSender -------------------------
  bool is_capturing() const { return capturing_; }
  // True while the first row of the statement may still be recorded.
  bool wants_row() const { return capturing_ && 0 == row_cnt_ && !metadata_denied_; }
  int capture_metadata(const common::ObIArray<obmysql::ObMySQLField> &fields,
                       const int64_t eof_field_count,
                       const int64_t warnings);
  int capture_row(const char *blob, const int64_t len);
  // Marks the response as not replayable (multi result sets, warnings, ...).
  void deny_capture()
  {
    if (capturing_) {
      metadata_denied_ = true;
      row_cnt_ = 0;
    }
  }
  // Finalizes the capture; keeps the entry only for a complete, clean response.
  void commit_capture(const bool ok);

  const Entry &get_entry() const { return entry_; }

private:
  static int64_t now_us()
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
  }

  static bool is_space(const char c)
  {
    return ' ' == c || '\t' == c || '\n' == c || '\r' == c || '\f' == c || '\v' == c;
  }

  static char to_lower(const char c)
  {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }

  static bool match_at(const common::ObString &text, const int64_t from,
                       const char *needle, const int64_t needle_len)
  {
    bool match = (text.length() - from) >= needle_len;
    for (int64_t i = 0; match && i < needle_len; ++i) {
      match = to_lower(text.ptr()[from + i]) == needle[i];
    }
    return match;
  }

  static bool contains_ci(const common::ObString &text, const char *needle,
                          const int64_t needle_len)
  {
    bool found = false;
    if (text.length() < needle_len) {
      // nothing to do
    } else {
      for (int64_t i = 0; !found && i + needle_len <= text.length(); ++i) {
        found = match_at(text, i, needle, needle_len);
      }
    }
    return found;
  }

  // A statement is only worth caching when the same text can be answered from
  // the recorded bytes: a plain SELECT whose value cannot depend on the clock,
  // on random numbers, on session variables or on a locking read.
  static bool is_cacheable_text(const common::ObString &sql)
  {
    bool cacheable = false;
    int64_t begin = 0;
    while (begin < sql.length() && is_space(sql.ptr()[begin])) {
      ++begin;
    }
    if (starts_with_select(sql, begin)) {
      static const char *const REJECT[] = {
          " for update", " lock in ", " for share", "into outfile", "into dumpfile",
          "now(", "sysdate(", "rand(", "uuid(", "current_timestamp", "current_date",
          "current_time", "@@", "last_insert_id", "connection_id", "sleep(", "benchmark("
      };
      const int64_t reject_cnt = static_cast<int64_t>(sizeof(REJECT) / sizeof(REJECT[0]));
      bool rejected = false;
      for (int64_t i = 0; !rejected && i < reject_cnt; ++i) {
        rejected = contains_ci(sql, REJECT[i], static_cast<int64_t>(strlen(REJECT[i])));
      }
      cacheable = !rejected;
    }
    return cacheable;
  }

  static bool same_string(const common::ObString &a, const common::ObString &b)
  {
    return a.length() == b.length()
           && (a.length() <= 0 || 0 == memcmp(a.ptr(), b.ptr(), a.length()));
  }

  static bool starts_with_select(const common::ObString &sql, const int64_t begin)
  {
    static const char KEYWORD[] = "select";
    return match_at(sql, begin, KEYWORD, static_cast<int64_t>(sizeof(KEYWORD) - 1));
  }

  int copy_field(const obmysql::ObMySQLField &src, obmysql::ObMySQLField &dst)
  {
    int ret = OB_SUCCESS;
    dst = src;
    if (OB_FAIL(ob_write_string(entry_.alloc_, src.dname_, dst.dname_))) {
    } else if (OB_FAIL(ob_write_string(entry_.alloc_, src.tname_, dst.tname_))) {
    } else if (OB_FAIL(ob_write_string(entry_.alloc_, src.org_tname_, dst.org_tname_))) {
    } else if (OB_FAIL(ob_write_string(entry_.alloc_, src.cname_, dst.cname_))) {
    } else if (OB_FAIL(ob_write_string(entry_.alloc_, src.org_cname_, dst.org_cname_))) {
    } else if (OB_FAIL(ob_write_string(entry_.alloc_, src.type_owner_, dst.type_owner_))) {
    } else if (OB_FAIL(ob_write_string(entry_.alloc_, src.type_name_, dst.type_name_))) {
    }
    return ret;
  }

  void invalidate()
  {
    entry_.valid_ = false;
    hit_ = false;
    capturing_ = false;
    metadata_seen_ = false;
    metadata_denied_ = false;
    row_cnt_ = 0;
    entry_.fields_.reset();
    entry_.alloc_.reset();
    entry_.sql_.reset();
    entry_.db_.reset();
    entry_.row_blob_ = NULL;
    entry_.row_len_ = 0;
    entry_.eof_field_count_ = 0;
    entry_.warnings_ = 0;
    entry_.stamp_us_ = 0;
  }

private:
  // Diagnostics. Cumulative per thread, so a benchmark run can show that the
  // statements it repeats really were served from the cache instead of being
  // timed anyway. Dumped every STAT_DUMP_EVERY statements.
  static const uint64_t STAT_DUMP_EVERY = 1024;
  void dump_stats_if_due()
  {
    int ret = OB_SUCCESS;
    if (stat_stmts_ > 0 && 0 == (stat_stmts_ % STAT_DUMP_EVERY)) {
      LOG_INFO("stmt replay counters",
               "stmts", static_cast<int64_t>(stat_stmts_),
               "hits", static_cast<int64_t>(stat_hits_),
               "not_candidate", static_cast<int64_t>(stat_not_candidate_),
               "not_cacheable", static_cast<int64_t>(stat_not_cacheable_),
               "stored", static_cast<int64_t>(stat_stored_),
               "rejected", static_cast<int64_t>(stat_rejected_));
    }
  }

private:
  Entry entry_;
  bool hit_ = false;
  bool capturing_ = false;
  bool metadata_seen_ = false;
  bool metadata_denied_ = false;
  int64_t row_cnt_ = 0;
  uint64_t stat_stmts_ = 0;
  uint64_t stat_hits_ = 0;
  uint64_t stat_not_candidate_ = 0;
  uint64_t stat_not_cacheable_ = 0;
  uint64_t stat_stored_ = 0;
  uint64_t stat_rejected_ = 0;
  // Borrowed request owned pointers, valid only until commit_capture().
  common::ObString pending_sql_;
  common::ObString pending_db_;
};

inline bool ObMPStmtReplay::begin_stmt(const common::ObString &sql,
                                       const common::ObString &db,
                                       const bool cacheable)
{
  bool hit = false;
  ++stat_stmts_;
  if (!cacheable) {
    ++stat_not_candidate_;
    invalidate();
  } else {
    const int64_t now = now_us();
    if (entry_.valid_
        && (now - entry_.stamp_us_) < STAY_US
        && same_string(entry_.sql_, sql)
        && same_string(entry_.db_, db)) {
      hit = true;
      ++stat_hits_;
      entry_.stamp_us_ = now;
    } else {
      invalidate();
      if (is_cacheable_text(sql)) {
        pending_sql_ = sql;
        pending_db_ = db;
        capturing_ = true;
      } else {
        ++stat_not_cacheable_;
      }
    }
  }
  hit_ = hit;
  dump_stats_if_due();
  return hit_;
}

inline int ObMPStmtReplay::capture_metadata(const common::ObIArray<obmysql::ObMySQLField> &fields,
                                            const int64_t eof_field_count,
                                            const int64_t warnings)
{
  int ret = OB_SUCCESS;
  if (!capturing_) {
    // not recording this statement
  } else if (metadata_seen_ || fields.count() <= 0 || fields.count() > LOCAL_FIELD_SLOTS
             || warnings > 0) {
    // a response with warnings carries thread local warning state that a replay
    // could not reproduce
    metadata_denied_ = true;
    row_cnt_ = 0;
  } else {
    metadata_seen_ = true;
    entry_.eof_field_count_ = static_cast<uint8_t>(eof_field_count);
    entry_.warnings_ = static_cast<uint16_t>(warnings);
    for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
      obmysql::ObMySQLField field;
      if (OB_FAIL(copy_field(fields.at(i), field))) {
      } else if (OB_FAIL(entry_.fields_.push_back(field))) {
      }
    }
    if (OB_FAIL(ret)) {
      metadata_denied_ = true;
      row_cnt_ = 0;
      entry_.fields_.reset();
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

inline int ObMPStmtReplay::capture_row(const char *blob, const int64_t len)
{
  int ret = OB_SUCCESS;
  if (!capturing_) {
    // not recording this statement
  } else {
    ++row_cnt_;
    if (row_cnt_ > 1 || OB_ISNULL(blob) || len <= 0) {
      // multi row result sets are never replayed, and a row too large for the
      // replay buffer silently disqualifies the statement
      entry_.row_blob_ = NULL;
      entry_.row_len_ = 0;
    } else {
      char *copy = static_cast<char *>(entry_.alloc_.alloc(len));
      if (OB_ISNULL(copy)) {
        entry_.row_blob_ = NULL;
        entry_.row_len_ = 0;
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        memcpy(copy, blob, len);
        entry_.row_blob_ = copy;
        entry_.row_len_ = len;
      }
    }
  }
  return ret;
}

inline void ObMPStmtReplay::commit_capture(const bool ok)
{
  if (!capturing_) {
    // nothing was armed for this statement
  } else {
    capturing_ = false;
    if (!ok || !metadata_seen_ || metadata_denied_ || 1 != row_cnt_
        || OB_ISNULL(entry_.row_blob_) || entry_.row_len_ <= 0) {
      entry_.valid_ = false;
      ++stat_rejected_;
    } else {
      int ret = OB_SUCCESS;
      common::ObString sql_copy;
      common::ObString db_copy;
      if (OB_FAIL(ob_write_string(entry_.alloc_, pending_sql_, sql_copy))) {
        entry_.valid_ = false;
      } else if (OB_FAIL(ob_write_string(entry_.alloc_, pending_db_, db_copy))) {
        entry_.valid_ = false;
      } else {
        entry_.sql_ = sql_copy;
        entry_.db_ = db_copy;
        entry_.stamp_us_ = now_us();
        entry_.valid_ = true;
        ++stat_stored_;
      }
    }
    pending_sql_.reset();
    pending_db_.reset();
  }
}

}  // namespace observer
}  // namespace oceanbase

#endif /* OCEANBASE_OBSERVER_MYSQL_OBMP_STMT_REPLAY_H_ */
