// db.hpp - storage engine and query executor for MiniVectorDB.
//
// Storage layout (all files live in the single data directory):
//
//   catalog.log   DDL records (CREATE TABLE / CREATE VECTOR INDEX)
//   commit.log    commit markers, one per committed transaction
//   t_<id>.tbl    row versions of table <id>
//
// Every file is a sequence of self describing frames:
//
//   [u32 payload_len][u32 crc32(payload)][payload ...]
//
// A frame whose length or CRC does not match marks the end of the valid
// data (torn write) and the file is truncated back to the last good frame
// during recovery.
//
// Durability protocol for a transaction:
//   1. append the row/DDL frames to their files
//   2. fsync() every touched file
//   3. append the commit marker to commit.log and fsync() it
//   4. only then answer the client
//
// A transaction without a commit marker is invisible after a restart, so an
// uncommitted transaction can never be partially applied.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"
#include "sql.hpp"

namespace mvdb {

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
struct Value {
  enum class Type { Null, Int, Double, Str, Vec };
  Type type = Type::Null;
  int64_t i = 0;
  double d = 0.0;
  std::string s;
  std::vector<float> v;

  static Value null() { return Value(); }
  static Value ofInt(int64_t x) { Value v; v.type = Type::Int; v.i = x; return v; }
  static Value ofDouble(double x) { Value v; v.type = Type::Double; v.d = x; return v; }
  static Value ofStr(std::string x) { Value v; v.type = Type::Str; v.s = std::move(x); return v; }
  static Value ofVec(std::vector<float> x) { Value v; v.type = Type::Vec; v.v = std::move(x); return v; }
  bool isNull() const { return type == Type::Null; }
  bool isNumeric() const { return type == Type::Int || type == Type::Double; }
  double asDouble() const { return type == Type::Int ? static_cast<double>(i) : d; }
};

using Row = std::vector<Value>;

// Parses a vector literal such as "[3,4]" into float32 components.
// Throws SqlError{"invalid_vector", ...} for malformed input, NaN, Infinity
// or values that overflow float32.
std::vector<float> parseVectorLiteral(const std::string& text, const std::string& context);

// ---------------------------------------------------------------------------
// Table metadata
// ---------------------------------------------------------------------------
struct Table {
  int64_t id = 0;
  std::string name;
  std::vector<ColumnDef> columns;
  std::map<std::string, size_t> colIndex;  // lower case name -> column index
  int pkCol = 0;
  int vectorCol = -1;
  int64_t version = 0;  // bumped on every committed change (index freshness)
  bool hasVectorIndex = false;
  std::string indexName;
  std::string indexColumn;
  std::map<int64_t, uint64_t> index;  // primary key -> offset of the newest row frame

  int findColumn(const std::string& name) const;
};

// ---------------------------------------------------------------------------
// Execution result
// ---------------------------------------------------------------------------
struct ExecResult {
  bool ok = true;
  std::string errorCode;
  std::string errorMessage;
  std::vector<std::string> columns;
  std::vector<JValue> rows;
  int64_t affectedRows = 0;
};

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------
class AnnIndex;  // ann.hpp

class Database {
 public:
  explicit Database(std::string dataDir);
  ~Database();

  // Opens the data directory and replays the durable state.
  void open();
  // Releases resources (uncommitted work is discarded).
  void close();

  // Runs one request (one or more ';'-separated statements).
  ExecResult execute(const std::string& sql, bool annMode);

 private:
  // --- storage primitives -------------------------------------------------
  struct Frame {
    std::vector<uint8_t> payload;
  };

  struct File {
    int fd = -1;
    uint64_t size = 0;
    bool openForAppend(const std::string& path);
    void appendFrame(const uint8_t* payload, size_t len, uint64_t* frameOffset);
    void sync();
    void close();
  };

  struct TxnChange {
    bool deleted = false;
    Row values;
  };

  struct IndexFrame {
    int64_t tableId = 0;
    std::string indexName;
    std::string column;
  };

  struct Txn {
    bool active = false;
    uint64_t id = 0;
    std::map<int64_t, std::map<int64_t, TxnChange>> changes;  // tableId -> pk -> change
    std::vector<std::shared_ptr<Table>> createdTables;
    std::vector<IndexFrame> indexFrames;
  };

  // --- helpers ------------------------------------------------------------
  std::string dataDir_;
  File catalog_;
  File commitLog_;
  std::map<int64_t, std::shared_ptr<Table>> tables_;          // committed tables
  std::map<std::string, int64_t> tableByName_;
  std::map<int64_t, File> tableFiles_;
  uint64_t nextTxnId_ = 1;
  uint64_t nextTableId_ = 1;
  uint64_t maxCommittedTxn_ = 0;
  Txn txn_;
  bool inExplicitTxn_ = false;

  std::shared_ptr<Table> findTable(const std::string& name) const;
  std::shared_ptr<Table> findTableById(int64_t id) const;
  bool tableVisible(const std::shared_ptr<Table>& table) const;

  // Reads a committed row from disk.
  bool readRowAt(const Table& table, uint64_t offset, Row& out) const;
  // Returns the visible version of a row (overlay first, then disk).
  bool visibleRow(const Table& table, int64_t pk, Row& out) const;

  uint64_t appendRowFrame(Table& table, int64_t pk, const Row& values, uint64_t txnId);
  void appendDeleteFrame(Table& table, int64_t pk, uint64_t txnId);
  void appendCreateTableFrame(const Table& table, uint64_t txnId);
  void appendCreateIndexFrame(const Table& table, const std::string& indexName,
                               const std::string& column, uint64_t txnId);
  void commitCurrentTxn();
  void rollbackCurrentTxn();

  // Recovery.
  void recover();

  // Statement execution.
  ExecResult execStmt(const Stmt& stmt, bool annMode);
  ExecResult execCreateTable(const Stmt& stmt);
  ExecResult execCreateVectorIndex(const Stmt& stmt);
  ExecResult execInsert(const Stmt& stmt);
  ExecResult execSelect(const Stmt& stmt, bool annMode);
  ExecResult execUpdate(const Stmt& stmt);
  ExecResult execDelete(const Stmt& stmt);

  // Value helpers.
  struct SubqueryCache {
    const Expr* expr = nullptr;
    std::vector<Value> values;
  };
  struct EvalCtx {
    const Table* table = nullptr;
    const Row* row = nullptr;
    const std::vector<SubqueryCache>* subs = nullptr;
  };

  Value evalExpr(const Expr& expr, const EvalCtx& ctx) const;
  bool evalPredicate(const Expr& expr, const EvalCtx& ctx) const;
  std::vector<Value> runSubquery(const Stmt& sub, bool annMode) const;
  // Collects every IN (SELECT ...) subquery of an expression tree.
  void collectSubqueries(const Expr* expr, std::vector<SubqueryCache>& out) const;
  // Converts an expression into a value of the given column type.
  Value coerceValue(const ColumnDef& column, const Expr& expr) const;
  // True when a primary key is visible (either on disk or in the overlay).
  bool keyVisible(const Table& table, int64_t pk) const;
  // Starts an implicit transaction when none is active.
  void ensureTxn();
  // Commits the implicit transaction when the statement is not inside an
  // explicit BEGIN ... COMMIT block.
  void finishMutation();

  // Collects the pks of a table that are visible to the current transaction.
  std::vector<int64_t> visibleKeys(const Table& table) const;

  // ANN support.
  struct AnnCache {
    int64_t version = -1;
    std::shared_ptr<class AnnIndex> index;
  };
  std::map<int64_t, AnnCache> annCache_;
  // Builds (or reuses) the ANN index of a table.
  AnnIndex* getAnnIndex(const Table& table);
};

}  // namespace mvdb
