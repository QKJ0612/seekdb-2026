// sql.hpp - AST and parser for the supported SQL subset.
//
// Supported statements:
//   CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(n), v VECTOR(n))
//   CREATE VECTOR INDEX [name] ON t (v)
//   INSERT INTO t [ (cols) ] VALUES (...), (...)
//   SELECT <items> FROM t [WHERE ...] [ORDER BY ... [ASC|DESC]] [LIMIT n]
//   UPDATE t SET c = v [, ...] [WHERE ...]
//   DELETE FROM t [WHERE ...]
//   BEGIN | COMMIT | ROLLBACK
//
// Everything else is rejected with the error code "unsupported_sql".
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mvdb {

enum class ColType { BigInt, VarChar, Vector };

struct ColumnDef {
  std::string name;
  ColType type = ColType::BigInt;
  int length = 0;  // VARCHAR(n): maximum number of Unicode characters
  int dim = 0;     // VECTOR(n): number of float32 components
  bool primaryKey = false;
};

enum class ExprType {
  IntLit,
  DoubleLit,
  StrLit,
  NullLit,
  Column,
  Compare,
  Logical,
  Not,
  IsNull,
  InSubquery,
  Func,
};

struct Stmt;
using StmtPtr = std::shared_ptr<Stmt>;
struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
  ExprType type = ExprType::NullLit;
  int64_t intVal = 0;
  double dblVal = 0.0;
  std::string strVal;  // literal text or column name
  std::string op;      // comparison / logical operator
  ExprPtr left, right;
  bool negated = false;
  std::string funcName;  // only "L2_DISTANCE"
  ExprPtr arg1, arg2;
  StmtPtr subquery;
};

enum class StmtType {
  CreateTable,
  CreateVectorIndex,
  Insert,
  Select,
  Update,
  Delete,
  Begin,
  Commit,
  Rollback,
};

struct SelectItem {
  bool isStar = false;
  bool isDistance = false;
  std::string column;         // plain column reference
  std::string outputName;     // final output column name
  ExprPtr distArg1, distArg2; // arguments of L2_DISTANCE
};

struct OrderBy {
  ExprPtr expr;
  bool asc = true;
};

struct Stmt {
  StmtType type = StmtType::Select;
  std::string tableName;
  std::vector<ColumnDef> columns;
  std::string indexName;
  std::string indexColumn;
  std::vector<std::string> insertColumns;
  std::vector<std::vector<ExprPtr>> insertRows;
  std::vector<SelectItem> items;
  ExprPtr where;
  std::shared_ptr<OrderBy> orderBy;
  int64_t limit = -1;
  std::vector<std::pair<std::string, ExprPtr>> setList;
};

// A SQL level failure.  `code` is the snake_case error code reported to the
// judge ("unsupported_sql", "syntax_error", ...).
struct SqlError {
  std::string code;
  std::string message;
};

// Parses one or more ';'-separated statements.  Throws SqlError on failure.
std::vector<StmtPtr> parseSql(const std::string& text);

}  // namespace mvdb
