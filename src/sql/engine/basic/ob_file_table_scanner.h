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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_SCANNER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_SCANNER_H_

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_iarray.h"
#include "common/object/ob_obj_type.h"
#include "common/object/ob_object.h"
#include "common/timezone/ob_time_convert.h"

namespace oceanbase
{
namespace sql
{

// ---------------------------------------------------------------------------
// Light weight readers shared by the resolver (schema inference) and the
// executor (row iteration) of the "file as a database" table functions.
// ---------------------------------------------------------------------------
class ObFileTableUtil
{
public:
  static const int64_t MAX_FILE_SIZE = 512L * 1024L * 1024L;
  static const int64_t MAX_PATH_LEN = 4096;

  inline static bool is_space(const char c) { return ' ' == c || '\t' == c || '\r' == c; }

  inline static bool ci_equal(const common::ObString &s, const char *lit)
  {
    const int64_t n = static_cast<int64_t>(::strlen(lit));
    bool eq = (n == s.length());
    for (int64_t i = 0; eq && i < n; ++i) {
      eq = (::tolower(static_cast<unsigned char>(s[i])) == ::tolower(static_cast<unsigned char>(lit[i])));
    }
    return eq;
  }

  static int load_file(common::ObIAllocator &alloc,
                       const common::ObString &path,
                       common::ObString &content)
  {
    int ret = common::OB_SUCCESS;
    char *buf = nullptr;
    FILE *fp = nullptr;
    long size = 0;
    char path_buf[MAX_PATH_LEN + 1];
    if (path.length() <= 0 || path.length() > MAX_PATH_LEN) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      MEMCPY(path_buf, path.ptr(), path.length());
      path_buf[path.length()] = '\0';
      if (OB_ISNULL(fp = ::fopen(path_buf, "rb"))) {
        ret = common::OB_FILE_NOT_EXIST;
      } else if (0 != ::fseek(fp, 0, SEEK_END)) {
        ret = common::OB_IO_ERROR;
      } else if ((size = ::ftell(fp)) < 0) {
        ret = common::OB_IO_ERROR;
      } else if (size > MAX_FILE_SIZE) {
        ret = common::OB_SIZE_OVERFLOW;
      } else if (0 != ::fseek(fp, 0, SEEK_SET)) {
        ret = common::OB_IO_ERROR;
      } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(size + 1)))) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else if (size > 0 && static_cast<long>(::fread(buf, 1, size, fp)) != size) {
        ret = common::OB_IO_ERROR;
      } else {
        buf[size] = '\0';
        content.assign_ptr(buf, static_cast<int32_t>(size));
      }
    }
    if (OB_NOT_NULL(fp)) {
      ::fclose(fp);
      fp = nullptr;
    }
    return ret;
  }

  // Parse one logical record of a CSV stream.  has_record = false at EOF.
  static int next_record(const char *buf,
                         const int64_t len,
                         int64_t &pos,
                         const char delim,
                         const char quote,
                         const char escape,
                         common::ObIAllocator &alloc,
                         common::ObIArray<common::ObString> &fields,
                         bool &has_record)
  {
    int ret = common::OB_SUCCESS;
    has_record = false;
    if (OB_ISNULL(buf) || pos < 0 || pos > len) {
      ret = common::OB_INVALID_ARGUMENT;
    } else if (pos >= len) {
      // EOF
    } else {
      has_record = true;
      bool record_done = false;
      while (OB_SUCC(ret) && !record_done) {
        const int64_t field_start = pos;
        char *out = static_cast<char *>(alloc.alloc(len - field_start + 1));
        int64_t out_len = 0;
        bool quoted = false;
        if (OB_ISNULL(out)) {
          ret = common::OB_ALLOCATE_MEMORY_FAILED;
        } else {
          if (pos < len && buf[pos] == quote) {
            quoted = true;
            ++pos;
            while (OB_SUCC(ret) && pos < len) {
              if (escape != '\0' && buf[pos] == escape && pos + 1 < len) {
                out[out_len++] = buf[pos + 1];
                pos += 2;
              } else if (buf[pos] == quote) {
                if (pos + 1 < len && buf[pos + 1] == quote) {
                  out[out_len++] = quote;
                  pos += 2;
                } else {
                  ++pos;
                  break;
                }
              } else {
                out[out_len++] = buf[pos++];
              }
            }
            // skip anything between the closing quote and the delimiter
            while (pos < len && buf[pos] != delim && buf[pos] != '\n') {
              ++pos;
            }
          } else {
            while (pos < len && buf[pos] != delim && buf[pos] != '\n') {
              out[out_len++] = buf[pos++];
            }
          }
        }
        if (OB_SUCC(ret)) {
          common::ObString field;
          if (out_len > 0) {
            field.assign_ptr(out, static_cast<int32_t>(out_len));
          }
          if (OB_FAIL(fields.push_back(field))) {
          } else if (pos < len && buf[pos] == delim) {
            ++pos;
            if (pos >= len) {
              common::ObString empty;
              ret = fields.push_back(empty);
              record_done = true;
            }
          } else {
            if (pos < len && buf[pos] == '\n') {
              ++pos;
            }
            record_done = true;
          }
        }
      }
    }
    return ret;
  }

  inline static common::ObString trim(const common::ObString &s)
  {
    int32_t b = 0;
    int32_t e = s.length();
    while (b < e && is_space(s[b])) { ++b; }
    while (e > b && is_space(s[e - 1])) { --e; }
    return common::ObString(e - b, s.ptr() + b);
  }

  // Copy an ObString into a local NUL terminated buffer, false if too long.
  static bool to_cstr(const common::ObString &s, char *buf, const int64_t cap)
  {
    bool ok = (s.length() >= 0 && s.length() < cap);
    if (ok) {
      if (s.length() > 0) {
        MEMCPY(buf, s.ptr(), s.length());
      }
      buf[s.length()] = '\0';
    }
    return ok;
  }

  static bool is_int_text(const common::ObString &s)
  {
    bool res = false;
    const common::ObString t = trim(s);
    if (t.length() > 0 && t.length() <= 20) {
      int32_t i = (t[0] == '-' || t[0] == '+') ? 1 : 0;
      res = (i < t.length());
      for (; res && i < t.length(); ++i) {
        if (!isdigit(static_cast<unsigned char>(t[i]))) { res = false; }
      }
    }
    return res;
  }

  static bool is_double_text(const common::ObString &s)
  {
    bool res = false;
    const common::ObString t = trim(s);
    char buf[128];
    if (t.length() > 0 && to_cstr(t, buf, sizeof(buf))) {
      char *end = nullptr;
      ::strtod(buf, &end);
      res = (end == buf + t.length());
    }
    return res;
  }

  static bool is_bool_text(const common::ObString &s)
  {
    const common::ObString t = trim(s);
    return ci_equal(t, "true") || ci_equal(t, "false");
  }

  static bool parse_date(const common::ObString &s, int32_t &value)
  {
    bool ok = false;
    const common::ObString t = trim(s);
    char buf[64];
    long y = 0, m = 0, d = 0;
    if (t.length() >= 8 && t.length() <= 10 && to_cstr(t, buf, sizeof(buf)) &&
        3 == ::sscanf(buf, "%4ld-%2ld-%2ld", &y, &m, &d)) {
      if (y > 0 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        value = static_cast<int32_t>(y * 10000 + m * 100 + d);
        ok = true;
      }
    }
    return ok;
  }

  static bool parse_datetime(const common::ObString &s, int64_t &value)
  {
    bool ok = false;
    const common::ObString t = trim(s);
    char buf[128];
    long y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (t.length() >= 19 && to_cstr(t, buf, sizeof(buf))) {
      if (buf[10] == 'T') { buf[10] = ' '; }
      if (6 == ::sscanf(buf, "%4ld-%2ld-%2ld %2ld:%2ld:%2ld", &y, &mo, &d, &h, &mi, &sec)) {
        if (y > 0 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 &&
            h >= 0 && h < 24 && mi >= 0 && mi < 60 && sec >= 0 && sec < 60) {
          value = (static_cast<int64_t>(y) * 10000 + mo * 100 + d) * 1000000 +
                  (h * 10000 + mi * 100 + sec);
          ok = true;
        }
      }
    }
    return ok;
  }

  // Convert a raw textual field into an ObObj of the requested type.
  static int convert_value(const common::ObObjType type,
                           const common::ObString &raw,
                           common::ObIAllocator &alloc,
                           common::ObObj &obj,
                           bool &is_null,
                           const common::ObTimeConvertCtx *cvrt_ctx = NULL)
  {
    int ret = common::OB_SUCCESS;
    is_null = false;
    const common::ObString t = trim(raw);
    char tmp[128];
    if (common::ObTinyIntType == type) {
      if (t.length() == 0) {
        is_null = true;
      } else if (is_bool_text(t)) {
        obj.set_tinyint(ci_equal(t, "true") ? 1 : 0);
      } else {
        obj.set_tinyint(static_cast<int8_t>(to_cstr(t, tmp, sizeof(tmp)) && ::atoll(tmp) != 0));
      }
    } else if (common::ObIntType == type || common::ObInt32Type == type) {
      if (t.length() == 0) {
        is_null = true;
      } else {
        obj.set_int(to_cstr(t, tmp, sizeof(tmp)) ? ::atoll(tmp) : 0);
      }
    } else if (common::ObDoubleType == type) {
      if (t.length() == 0) {
        is_null = true;
      } else {
        obj.set_double(to_cstr(t, tmp, sizeof(tmp)) ? ::atof(tmp) : 0.0);
      }
    } else if (common::ObDateType == type) {
      int32_t v = 0;
      if (t.length() == 0) {
        is_null = true;
      } else if (OB_SUCCESS != common::ObTimeConverter::str_to_date(t, v)) {
        ret = common::OB_INVALID_DATE_VALUE;
      } else {
        obj.set_date(v);
      }
    } else if (common::ObDateTimeType == type) {
      int64_t v = 0;
      if (t.length() == 0) {
        is_null = true;
      } else {
        common::ObString norm = t;
        if (t.length() < static_cast<int64_t>(sizeof(tmp))) {
          MEMCPY(tmp, t.ptr(), t.length());
          tmp[t.length()] = '\0';
          for (int64_t i = 0; i < t.length(); ++i) {
            if ('T' == tmp[i]) {
              tmp[i] = ' ';
            }
          }
          norm.assign_ptr(tmp, static_cast<int32_t>(t.length()));
        }
        common::ObTimeConvertCtx local_ctx(NULL, false);
        const common::ObTimeConvertCtx &cctx = (NULL == cvrt_ctx) ? local_ctx : *cvrt_ctx;
        if (OB_SUCCESS != common::ObTimeConverter::str_to_datetime(norm, cctx, v)) {
          ret = common::OB_INVALID_DATE_VALUE;
        } else {
          obj.set_datetime(v);
        }
      }
    } else {
      if (t.length() == 0) {
        is_null = true;
      } else {
        char *buf = static_cast<char *>(alloc.alloc(t.length()));
        if (OB_ISNULL(buf)) {
          ret = common::OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, t.ptr(), t.length());
          obj.set_varchar(buf, static_cast<int32_t>(t.length()));
          obj.set_collation_type(common::CS_TYPE_UTF8MB4_GENERAL_CI);
          obj.set_collation_level(common::CS_LEVEL_IMPLICIT);
        }
      }
    }
    return ret;
  }

  // simple '*' / '?' glob matcher
  static bool match_glob(const common::ObString &pattern, const common::ObString &name)
  {
    int64_t pi = 0;
    int64_t ni = 0;
    int64_t star_p = -1;
    int64_t star_n = 0;
    bool matched = true;
    while (matched && ni < name.length()) {
      if (pi < pattern.length() && (pattern[pi] == '?' || pattern[pi] == name[ni])) {
        ++pi;
        ++ni;
      } else if (pi < pattern.length() && pattern[pi] == '*') {
        star_p = pi;
        ++pi;
        star_n = ni;
      } else if (star_p >= 0) {
        pi = star_p + 1;
        ni = ++star_n;
      } else {
        matched = false;
      }
    }
    while (matched && pi < pattern.length() && pattern[pi] == '*') { ++pi; }
    return matched && pi == pattern.length();
  }

  static int list_dir(common::ObIAllocator &alloc,
                      const common::ObString &dir_path,
                      const common::ObString &pattern,
                      common::ObIArray<common::ObString> &names)
  {
    int ret = common::OB_SUCCESS;
    DIR *dir = nullptr;
    struct dirent *ent = nullptr;
    char path_buf[MAX_PATH_LEN + 1];
    if (dir_path.length() <= 0 || dir_path.length() > MAX_PATH_LEN) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      MEMCPY(path_buf, dir_path.ptr(), dir_path.length());
      path_buf[dir_path.length()] = '\0';
      if (OB_ISNULL(dir = ::opendir(path_buf))) {
        ret = common::OB_FILE_NOT_EXIST;
      } else {
        while (OB_SUCC(ret) && OB_NOT_NULL(ent = ::readdir(dir))) {
          const char *nm = ent->d_name;
          if (0 == ::strcmp(nm, ".") || 0 == ::strcmp(nm, "..")) {
            continue;
          }
          common::ObString name(static_cast<int32_t>(::strlen(nm)), nm);
          if (pattern.length() > 0 && !match_glob(pattern, name)) {
            continue;
          }
          char *copy = static_cast<char *>(alloc.alloc(name.length()));
          if (OB_ISNULL(copy)) {
            ret = common::OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(copy, name.ptr(), name.length());
            common::ObString stored(name.length(), copy);
            if (OB_FAIL(names.push_back(stored))) {
            }
          }
        }
        ::closedir(dir);
        dir = nullptr;
      }
    }
    if (OB_NOT_NULL(dir)) { ::closedir(dir); }
    return ret;
  }
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_SCANNER_H_
