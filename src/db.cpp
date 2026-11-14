// db.cpp - storage engine, transactions and query executor.
#include "db.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include "ann.hpp"
#include "util.hpp"

namespace mvdb {
namespace {

// ---------------------------------------------------------------------------
// Little endian primitives for the frame payloads.
// ---------------------------------------------------------------------------
void putU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void putU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void putI64(std::vector<uint8_t>& out, int64_t v) { putU64(out, static_cast<uint64_t>(v)); }

void putStr(std::vector<uint8_t>& out, const std::string& s) {
  putU32(out, static_cast<uint32_t>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

int64_t getI64(const uint8_t* p) { return static_cast<int64_t>(getU64(p)); }

std::string getStr(const uint8_t* p, size_t& pos, size_t end) {
  if (pos + 4 > end) throw SqlError{"corrupt_data", "truncated string in log frame"};
  const uint32_t len = getU32(p + pos);
  pos += 4;
  if (pos + len > end) throw SqlError{"corrupt_data", "truncated string in log frame"};
  std::string s(reinterpret_cast<const char*>(p + pos), len);
  pos += len;
  return s;
}

// ---------------------------------------------------------------------------
// Row encoding
// ---------------------------------------------------------------------------
void encodeValue(const Value& v, std::vector<uint8_t>& out) {
  switch (v.type) {
    case Value::Type::Null:
      out.push_back(0);
      break;
    case Value::Type::Int:
      out.push_back(1);
      putI64(out, v.i);
      break;
    case Value::Type::Str:
      out.push_back(2);
      putStr(out, v.s);
      break;
    case Value::Type::Vec: {
      out.push_back(3);
      putU32(out, static_cast<uint32_t>(v.v.size()));
      for (float f : v.v) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        putU32(out, bits);
      }
      break;
    }
    case Value::Type::Double: {
      out.push_back(4);
      uint64_t bits = 0;
      std::memcpy(&bits, &v.d, sizeof(bits));
      putU64(out, bits);
      break;
    }
  }
}

bool decodeValue(const uint8_t* p, size_t& pos, size_t end, Value& out) {
  if (pos >= end) return false;
  const uint8_t tag = p[pos++];
  switch (tag) {
    case 0:
      out = Value::null();
      return true;
    case 1:
      if (pos + 8 > end) return false;
      out = Value::ofInt(getI64(p + pos));
      pos += 8;
      return true;
    case 2: {
      if (pos + 4 > end) return false;
      const uint32_t len = getU32(p + pos);
      pos += 4;
      if (pos + len > end) return false;
      out = Value::ofStr(std::string(reinterpret_cast<const char*>(p + pos), len));
      pos += len;
      return true;
    }
    case 3: {
      if (pos + 4 > end) return false;
      const uint32_t count = getU32(p + pos);
      pos += 4;
      if (pos + static_cast<size_t>(count) * 4 > end) return false;
      std::vector<float> vec(count);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t bits = getU32(p + pos);
        std::memcpy(&vec[i], &bits, sizeof(float));
        pos += 4;
      }
      out = Value::ofVec(std::move(vec));
      return true;
    }
    case 4: {
      if (pos + 8 > end) return false;
      const uint64_t bits = getU64(p + pos);
      double d = 0.0;
      std::memcpy(&d, &bits, sizeof(d));
      pos += 8;
      out = Value::ofDouble(d);
      return true;
    }
    default:
      return false;
  }
}

void encodeRow(const Row& row, std::vector<uint8_t>& out) {
  for (const auto& v : row) encodeValue(v, out);
}

bool decodeRow(const uint8_t* p, size_t pos, size_t end, Row& out) {
  out.clear();
  while (pos < end) {
    Value v;
    if (!decodeValue(p, pos, end, v)) return false;
    out.push_back(std::move(v));
  }
  return true;
}

// Reads all valid frames of a file.  Returns the offset of the first
// invalid/truncated frame (the valid length of the file).
template <typename Callback>
uint64_t readFrames(const std::string& path, Callback&& cb) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return 0;
  struct stat st;
  uint64_t size = 0;
  if (::fstat(fd, &st) == 0) size = static_cast<uint64_t>(st.st_size);
  uint64_t off = 0;
  while (off + 8 <= size) {
    uint8_t header[8];
    if (::pread(fd, header, 8, static_cast<off_t>(off)) != 8) break;
    const uint32_t len = getU32(header);
    const uint32_t crc = getU32(header + 4);
    if (len == 0 || len > (64u << 20) || off + 8 + len > size) break;
    std::vector<uint8_t> payload(len);
    if (::pread(fd, payload.data(), len, static_cast<off_t>(off + 8)) != static_cast<ssize_t>(len)) break;
    if (crc32(payload.data(), len) != crc) break;
    cb(payload.data(), len, off);
    off += 8 + len;
  }
  ::close(fd);
  return off;
}

std::string tableFileName(int64_t tableId) { return "t_" + std::to_string(tableId) + ".tbl"; }

}  // namespace

// ---------------------------------------------------------------------------
// Vector literals
// ---------------------------------------------------------------------------
std::vector<float> parseVectorLiteral(const std::string& text, const std::string& context) {
  const auto invalid = [&](const std::string& why) -> std::vector<float> {
    throw SqlError{"invalid_vector", "invalid vector literal" + (context.empty() ? "" : " for " + context) + ": " + why};
  };
  size_t i = 0;
  const size_t n = text.size();
  while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i >= n || text[i] != '[') return invalid("expected '['");
  ++i;
  std::vector<float> values;
  while (true) {
    while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < n && text[i] == ']') {
      ++i;
      break;
    }
    const char* start = text.c_str() + i;
    char* endp = nullptr;
    errno = 0;
    const double d = std::strtod(start, &endp);
    if (endp == start) return invalid("expected a number");
    if (!std::isfinite(d)) return invalid("NaN and Infinity are not allowed");
    if (errno == ERANGE) return invalid("value out of range");
    const float f = static_cast<float>(d);
    if (!std::isfinite(f)) return invalid("value overflows float32");
    values.push_back(f);
    i = static_cast<size_t>(endp - text.c_str());
    while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < n && text[i] == ',') { ++i; continue; }
    if (i < n && text[i] == ']') { ++i; break; }
    return invalid("expected ',' or ']'");
  }
  while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i != n) return invalid("trailing characters after ']'");
  return values;
}

// ---------------------------------------------------------------------------
// Table helpers
// ---------------------------------------------------------------------------
int Table::findColumn(const std::string& name) const {
  auto it = colIndex.find(toLower(name));
  return it == colIndex.end() ? -1 : static_cast<int>(it->second);
}

// ---------------------------------------------------------------------------
// Database: file primitives
// ---------------------------------------------------------------------------
bool Database::File::openForAppend(const std::string& path) {
  close();
  fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return false;
  struct stat st;
  size = (::fstat(fd, &st) == 0) ? static_cast<uint64_t>(st.st_size) : 0;
  return true;
}

void Database::File::appendFrame(const uint8_t* payload, size_t len, uint64_t* frameOffset) {
  if (fd < 0) throw SqlError{"io_error", "file is not open"};
  std::vector<uint8_t> buffer;
  buffer.reserve(len + 8);
  putU32(buffer, static_cast<uint32_t>(len));
  putU32(buffer, crc32(payload, len));
  buffer.insert(buffer.end(), payload, payload + len);
  const uint64_t offset = size;
  size_t written = 0;
  while (written < buffer.size()) {
    const ssize_t n = ::write(fd, buffer.data() + written, buffer.size() - written);
    if (n <= 0) {
      if (errno == EINTR) continue;
      throw SqlError{"io_error", std::string("write failed: ") + std::strerror(errno)};
    }
    written += static_cast<size_t>(n);
  }
  size += buffer.size();
  if (frameOffset) *frameOffset = offset;
}

void Database::File::sync() {
  if (fd >= 0 && ::fsync(fd) != 0) throw SqlError{"io_error", std::string("fsync failed: ") + std::strerror(errno)};
}

void Database::File::close() {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
  size = 0;
}

// ---------------------------------------------------------------------------
// Database: construction / open / close
// ---------------------------------------------------------------------------
Database::Database(std::string dataDir) : dataDir_(std::move(dataDir)) {}

Database::~Database() { close(); }

void Database::open() {
  if (!isDirectory(dataDir_)) {
    ::mkdir(dataDir_.c_str(), 0777);
  }
  if (!isDirectory(dataDir_)) throw SqlError{"io_error", "data directory is not usable: " + dataDir_};

  const std::string catalogPath = dataDir_ + "/catalog.log";
  const std::string commitPath = dataDir_ + "/commit.log";

  // ---- commit log: the highest committed transaction id ------------------
  uint64_t maxCommitted = 0;
  const uint64_t commitValid = readFrames(commitPath, [&](const uint8_t* p, size_t len, uint64_t) {
    if (len >= 8) maxCommitted = std::max(maxCommitted, getU64(p));
  });
  (void)commitValid;
  maxCommittedTxn_ = maxCommitted;
  nextTxnId_ = maxCommitted + 1;

  // ---- catalog: table definitions ----------------------------------------
  const uint64_t catalogValid = readFrames(catalogPath, [&](const uint8_t* p, size_t len, uint64_t) {
    if (len < 9) return;
    size_t pos = 0;
    const uint8_t kind = p[pos++];
    const uint64_t txnId = getU64(p + pos);
    pos += 8;
    if (txnId > maxCommitted) return;  // never committed -> invisible
    if (kind == 1) {
      // CREATE TABLE
      const int64_t tableId = getI64(p + pos);
      pos += 8;
      std::string name = getStr(p, pos, len);
      if (pos + 4 > len) return;
      const uint32_t ncols = getU32(p + pos);
      pos += 4;
      auto table = std::make_shared<Table>();
      table->id = tableId;
      table->name = name;
      bool ok = true;
      for (uint32_t i = 0; i < ncols; ++i) {
        ColumnDef def;
        def.name = getStr(p, pos, len);
        if (pos + 6 > len) { ok = false; break; }
        const uint8_t type = p[pos++];
        def.type = static_cast<ColType>(type);
        const uint32_t param = getU32(p + pos);
        pos += 4;
        const uint8_t flags = p[pos++];
        def.primaryKey = (flags & 1u) != 0;
        if (def.type == ColType::VarChar) def.length = static_cast<int>(param);
        if (def.type == ColType::Vector) def.dim = static_cast<int>(param);
        table->columns.push_back(def);
      }
      if (!ok) return;
      for (size_t i = 0; i < table->columns.size(); ++i) {
        table->colIndex[toLower(table->columns[i].name)] = i;
        if (table->columns[i].primaryKey) table->pkCol = static_cast<int>(i);
        if (table->columns[i].type == ColType::Vector) table->vectorCol = static_cast<int>(i);
      }
      tables_[table->id] = table;
      tableByName_[toLower(table->name)] = table->id;
      nextTableId_ = std::max(nextTableId_, static_cast<uint64_t>(table->id + 1));
    } else if (kind == 2) {
      // CREATE VECTOR INDEX
      if (pos + 8 > len) return;
      const int64_t tableId = getI64(p + pos);
      pos += 8;
      std::string indexName = getStr(p, pos, len);
      std::string column = getStr(p, pos, len);
      auto it = tables_.find(tableId);
      if (it == tables_.end()) return;
      it->second->hasVectorIndex = true;
      it->second->indexName = indexName;
      it->second->indexColumn = column;
    }
  });

  // ---- table files: row versions -----------------------------------------
  DIR* dir = ::opendir(dataDir_.c_str());
  if (dir != nullptr) {
    std::vector<std::string> tableFiles;
    while (struct dirent* entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name.size() > 5 && name.compare(0, 2, "t_") == 0 &&
          name.compare(name.size() - 4, 4, ".tbl") == 0) {
        tableFiles.push_back(name);
      }
    }
    ::closedir(dir);
    std::sort(tableFiles.begin(), tableFiles.end());
    for (const std::string& name : tableFiles) {
      const std::string path = dataDir_ + "/" + name;
      const int64_t tableId = std::strtoll(name.c_str() + 2, nullptr, 10);
      auto it = tables_.find(tableId);
      const uint64_t valid = readFrames(path, [&](const uint8_t* p, size_t len, uint64_t offset) {
        if (len < 9) return;
        size_t pos = 0;
        const uint8_t kind = p[pos++];
        const uint64_t txnId = getU64(p + pos);
        pos += 8;
        if (txnId > maxCommitted) return;  // uncommitted -> invisible
        if (kind == 1) {
          if (pos + 8 > len) return;
          const int64_t pk = getI64(p + pos);
          pos += 8;
          Row row;
          if (!decodeRow(p, pos, len, row)) return;
          if (it != tables_.end()) it->second->index[pk] = offset;
        } else if (kind == 2) {
          if (pos + 8 > len) return;
          const int64_t pk = getI64(p + pos);
          if (it != tables_.end()) it->second->index.erase(pk);
        }
      });
      // Drop a torn tail so that later appends stay consistent.
      struct stat st;
      if (::stat(path.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) > valid) {
        if (::truncate(path.c_str(), static_cast<off_t>(valid)) != 0) {
          // ignore: the file stays as it is, the invalid tail is skipped anyway
        }
      }
    }
  }

  // ---- compact the commit log -------------------------------------------
  // The commit log only needs to remember the highest committed transaction.
  // Rewriting it uses a temporary file plus rename, so a crash during the
  // rewrite leaves either the old or the new file, never an empty one.
  {
    const std::string tmpPath = commitPath + ".tmp";
    File tmp;
    if (tmp.openForAppend(tmpPath)) {
      std::vector<uint8_t> payload;
      putU64(payload, maxCommitted);
      uint64_t off = 0;
      tmp.appendFrame(payload.data(), payload.size(), &off);
      tmp.sync();
      tmp.close();
      if (::rename(tmpPath.c_str(), commitPath.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
      }
    }
  }

  // Truncate the catalog log to its valid length as well.
  {
    struct stat st;
    if (::stat(catalogPath.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) > catalogValid) {
      if (::truncate(catalogPath.c_str(), static_cast<off_t>(catalogValid)) != 0) {
        std::fprintf(stderr, "mvdb: cannot truncate catalog.log\n");
      }
    }
  }
  const uint64_t commitSize = [&] {
    struct stat st;
    if (::stat(commitPath.c_str(), &st) != 0) return static_cast<uint64_t>(0);
    return static_cast<uint64_t>(st.st_size);
  }();
  (void)commitSize;

  if (!catalog_.openForAppend(catalogPath)) throw SqlError{"io_error", "cannot open catalog.log"};
  if (!commitLog_.openForAppend(commitPath)) throw SqlError{"io_error", "cannot open commit.log"};

  // Open every table file so that rows can be read back after a restart.
  for (const auto& entry : tables_) {
    File& file = tableFiles_[entry.first];
    if (!file.openForAppend(dataDir_ + "/" + tableFileName(entry.first))) {
      throw SqlError{"io_error", "cannot open table file for table " + entry.second->name};
    }
  }
}

void Database::close() {
  for (auto& kv : tableFiles_) kv.second.close();
  tableFiles_.clear();
  catalog_.close();
  commitLog_.close();
  tables_.clear();
  tableByName_.clear();
  txn_ = Txn();
  inExplicitTxn_ = false;
}

// ---------------------------------------------------------------------------
// Database: table lookup
// ---------------------------------------------------------------------------
std::shared_ptr<Table> Database::findTable(const std::string& name) const {
  for (const auto& table : txn_.createdTables) {
    if (equalsIgnoreCase(table->name, name)) return table;
  }
  auto it = tableByName_.find(toLower(name));
  if (it == tableByName_.end()) return nullptr;
  auto table = tables_.find(it->second);
  return table == tables_.end() ? nullptr : table->second;
}

std::shared_ptr<Table> Database::findTableById(int64_t id) const {
  for (const auto& table : txn_.createdTables) {
    if (table->id == id) return table;
  }
  auto it = tables_.find(id);
  return it == tables_.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Database: reading rows
// ---------------------------------------------------------------------------
bool Database::readRowAt(const Table& table, uint64_t offset, Row& out) const {
  auto fileIt = tableFiles_.find(table.id);
  if (fileIt == tableFiles_.end() || fileIt->second.fd < 0) return false;
  const int fd = fileIt->second.fd;
  uint8_t header[8];
  if (::pread(fd, header, 8, static_cast<off_t>(offset)) != 8) return false;
  const uint32_t len = getU32(header);
  if (len == 0 || len > (64u << 20)) return false;
  std::vector<uint8_t> payload(len);
  if (::pread(fd, payload.data(), len, static_cast<off_t>(offset + 8)) != static_cast<ssize_t>(len)) return false;
  if (crc32(payload.data(), len) != getU32(header + 4)) return false;
  size_t pos = 1 + 8 + 8;  // kind + txnId + pk
  if (pos > len) return false;
  return decodeRow(payload.data(), pos, len, out);
}

bool Database::visibleRow(const Table& table, int64_t pk, Row& out) const {
  auto txnIt = txn_.changes.find(table.id);
  if (txnIt != txn_.changes.end()) {
    auto changeIt = txnIt->second.find(pk);
    if (changeIt != txnIt->second.end()) {
      if (changeIt->second.deleted) return false;
      out = changeIt->second.values;
      return true;
    }
  }
  auto indexIt = table.index.find(pk);
  if (indexIt == table.index.end()) return false;
  return readRowAt(table, indexIt->second, out);
}

std::vector<int64_t> Database::visibleKeys(const Table& table) const {
  std::vector<int64_t> keys;
  auto txnIt = txn_.changes.find(table.id);
  // Merge the committed index and the overlay, both sorted by primary key.
  auto committed = table.index.begin();
  auto overlay = (txnIt == txn_.changes.end()) ? std::map<int64_t, TxnChange>::const_iterator()
                                               : txnIt->second.begin();
  const bool hasOverlay = txnIt != txn_.changes.end();
  while (committed != table.index.end() || (hasOverlay && overlay != txnIt->second.end())) {
    int64_t pk;
    if (!hasOverlay || overlay == txnIt->second.end()) {
      pk = committed->first;
    } else if (committed == table.index.end()) {
      pk = overlay->first;
    } else {
      pk = std::min(committed->first, overlay->first);
    }
    keys.push_back(pk);
    if (committed != table.index.end() && committed->first == pk) ++committed;
    if (hasOverlay && overlay != txnIt->second.end() && overlay->first == pk) ++overlay;
  }
  return keys;
}

// ---------------------------------------------------------------------------
// Database: writing frames
// ---------------------------------------------------------------------------
uint64_t Database::appendRowFrame(Table& table, int64_t pk, const Row& values, uint64_t txnId) {
  std::vector<uint8_t> payload;
  payload.reserve(64 + values.size() * 16);
  payload.push_back(1);
  putU64(payload, txnId);
  putI64(payload, pk);
  encodeRow(values, payload);
  File& file = tableFiles_[table.id];
  uint64_t offset = 0;
  file.appendFrame(payload.data(), payload.size(), &offset);
  return offset;
}

void Database::appendDeleteFrame(Table& table, int64_t pk, uint64_t txnId) {
  std::vector<uint8_t> payload;
  payload.push_back(2);
  putU64(payload, txnId);
  putI64(payload, pk);
  File& file = tableFiles_[table.id];
  uint64_t offset = 0;
  file.appendFrame(payload.data(), payload.size(), &offset);
}

void Database::appendCreateTableFrame(const Table& table, uint64_t txnId) {
  std::vector<uint8_t> payload;
  payload.push_back(1);
  putU64(payload, txnId);
  putI64(payload, table.id);
  putStr(payload, table.name);
  putU32(payload, static_cast<uint32_t>(table.columns.size()));
  for (const auto& column : table.columns) {
    putStr(payload, column.name);
    payload.push_back(static_cast<uint8_t>(column.type));
    putU32(payload, static_cast<uint32_t>(column.type == ColType::VarChar ? column.length : column.dim));
    payload.push_back(column.primaryKey ? 1 : 0);
  }
  uint64_t offset = 0;
  catalog_.appendFrame(payload.data(), payload.size(), &offset);
}

void Database::appendCreateIndexFrame(const Table& table, const std::string& indexName,
                                      const std::string& column, uint64_t txnId) {
  std::vector<uint8_t> payload;
  payload.push_back(2);
  putU64(payload, txnId);
  putI64(payload, table.id);
  putStr(payload, indexName);
  putStr(payload, column);
  uint64_t offset = 0;
  catalog_.appendFrame(payload.data(), payload.size(), &offset);
}

// ---------------------------------------------------------------------------
// Database: transactions
// ---------------------------------------------------------------------------
void Database::commitCurrentTxn() {
  const uint64_t txnId = txn_.id;
  std::vector<File*> touched;
  std::map<std::pair<int64_t, int64_t>, uint64_t> newOffsets;
  std::vector<std::pair<int64_t, int64_t>> deleted;

  // 1. DDL frames for tables created by this transaction.
  for (const auto& table : txn_.createdTables) {
    File& file = tableFiles_[table->id];
    if (file.fd < 0 && !file.openForAppend(dataDir_ + "/" + tableFileName(table->id))) {
      throw SqlError{"io_error", "cannot create table file"};
    }
    touched.push_back(&file);
    appendCreateTableFrame(*table, txnId);
  }
  for (const auto& frame : txn_.indexFrames) {
    auto table = findTableById(frame.tableId);
    if (!table) continue;
    appendCreateIndexFrame(*table, frame.indexName, frame.column, txnId);
  }

  // 2. Row frames.
  for (const auto& entry : txn_.changes) {
    const int64_t tableId = entry.first;
    auto table = findTableById(tableId);
    if (!table) continue;
    File& file = tableFiles_[tableId];
    if (file.fd < 0 && !file.openForAppend(dataDir_ + "/" + tableFileName(tableId))) {
      throw SqlError{"io_error", "cannot open table file"};
    }
    touched.push_back(&file);
    for (const auto& change : entry.second) {
      const int64_t pk = change.first;
      if (change.second.deleted) {
        appendDeleteFrame(*table, pk, txnId);
        deleted.emplace_back(tableId, pk);
      } else {
        const uint64_t offset = appendRowFrame(*table, pk, change.second.values, txnId);
        newOffsets[{tableId, pk}] = offset;
      }
    }
  }

  // 3. Make the frames durable.
  for (File* file : touched) file->sync();
  if (!txn_.createdTables.empty() || !txn_.indexFrames.empty()) catalog_.sync();

  // 4. Commit marker - only now does the transaction become visible.
  {
    std::vector<uint8_t> payload;
    putU64(payload, txnId);
    uint64_t offset = 0;
    commitLog_.appendFrame(payload.data(), payload.size(), &offset);
    commitLog_.sync();
  }
  maxCommittedTxn_ = std::max(maxCommittedTxn_, txnId);

  // 5. Publish the changes in memory.
  for (const auto& table : txn_.createdTables) {
    tables_[table->id] = table;
    tableByName_[toLower(table->name)] = table->id;
  }
  for (const auto& frame : txn_.indexFrames) {
    auto table = findTableById(frame.tableId);
    if (!table) continue;
    table->hasVectorIndex = true;
    table->indexName = frame.indexName;
    table->indexColumn = frame.column;
  }
  for (const auto& entry : newOffsets) {
    auto table = findTableById(entry.first.first);
    if (table) table->index[entry.first.second] = entry.second;
  }
  for (const auto& entry : deleted) {
    auto table = findTableById(entry.first);
    if (table) table->index.erase(entry.second);
  }
  for (const auto& entry : txn_.changes) {
    auto table = findTableById(entry.first);
    if (table) table->version++;
  }

  txn_ = Txn();
}

void Database::rollbackCurrentTxn() {
  txn_ = Txn();
  inExplicitTxn_ = false;
}

}  // namespace mvdb
