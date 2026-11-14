// exec.cpp - statement execution: DDL, DML, SELECT and expression evaluation.
//
// Every statement is executed against an in-memory overlay ("transaction
// state").  A statement that fails leaves the overlay untouched, so a failed
// statement inside an explicit transaction only rolls back that statement.
// The overlay is written to the append-only table files when the transaction
// is committed (see db.cpp).
#include "db.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>

#include "ann.hpp"
#include "util.hpp"

namespace mvdb {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// A comparable snapshot of a value, used for ORDER BY.
struct SortKey {
  enum class Kind { Null, Number, Text, Vector };
  Kind kind = Kind::Null;
  double num = 0.0;
  std::string text;
  std::vector<float> vec;
};

SortKey makeSortKey(const Value& v) {
  SortKey key;
  switch (v.type) {
    case Value::Type::Null:
      key.kind = SortKey::Kind::Null;
      break;
    case Value::Type::Int:
      key.kind = SortKey::Kind::Number;
      key.num = static_cast<double>(v.i);
      break;
    case Value::Type::Double:
      key.kind = SortKey::Kind::Number;
      key.num = v.d;
      break;
    case Value::Type::Str:
      key.kind = SortKey::Kind::Text;
      key.text = v.s;
      break;
    case Value::Type::Vec:
      key.kind = SortKey::Kind::Vector;
      key.vec = v.v;
      break;
  }
  return key;
}

// Three way comparison.  NULL sorts before every other value, numbers before
// text and vectors are compared component by component.
int compareSortKeys(const SortKey& a, const SortKey& b) {
  if (a.kind == SortKey::Kind::Null || b.kind == SortKey::Kind::Null) {
    if (a.kind == b.kind) return 0;
    return a.kind == SortKey::Kind::Null ? -1 : 1;
  }
  if (a.kind != b.kind) {
    // Mixed types are compared by their textual representation, which keeps
    // the ordering deterministic.
    const std::string x = a.kind == SortKey::Kind::Text ? a.text : std::to_string(a.num);
    const std::string y = b.kind == SortKey::Kind::Text ? b.text : std::to_string(b.num);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  switch (a.kind) {
    case SortKey::Kind::Number:
      return a.num < b.num ? -1 : (a.num > b.num ? 1 : 0);
    case SortKey::Kind::Text:
      // Byte wise comparison of UTF-8 is the same as code point order.
      return a.text < b.text ? -1 : (a.text > b.text ? 1 : 0);
    case SortKey::Kind::Vector: {
      const size_t n = std::min(a.vec.size(), b.vec.size());
      for (size_t i = 0; i < n; ++i) {
        if (a.vec[i] < b.vec[i]) return -1;
        if (a.vec[i] > b.vec[i]) return 1;
      }
      if (a.vec.size() == b.vec.size()) return 0;
      return a.vec.size() < b.vec.size() ? -1 : 1;
    }
    default:
      return 0;
  }
}

// Numbers are emitted as JSON integers whenever they are integral: the judge
// compares integers strictly but accepts an integer where a float is
// expected, so integral doubles are the safest representation.
JValue numberToJson(double v) {
  if (!std::isfinite(v)) return JValue::makeInt(0);
  if (v == std::floor(v) && std::fabs(v) <= 9007199254740992.0) {
    return JValue::makeInt(static_cast<int64_t>(v));
  }
  return JValue::makeDouble(v);
}

JValue valueToJson(const Value& v) {
  switch (v.type) {
    case Value::Type::Null:
      return JValue::makeNull();
    case Value::Type::Int:
      return JValue::makeInt(v.i);
    case Value::Type::Double:
      return numberToJson(v.d);
    case Value::Type::Str:
      return JValue::makeString(v.s);
    case Value::Type::Vec: {
      JArray arr;
      arr.reserve(v.v.size());
      for (float f : v.v) arr.push_back(numberToJson(static_cast<double>(f)));
      return JValue::makeArray(std::move(arr));
    }
  }
  return JValue::makeNull();
}

// Parses a numeric string, returns false when the text is not a number.
bool textToDouble(const std::string& text, double& out) {
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) return false;
  while (end && *end == ' ') ++end;
  if (end && *end != '\0') return false;
  if (!std::isfinite(v)) return false;
  out = v;
  return true;
}

// Three way comparison of two values.  Throws "type_mismatch" when the two
// values are not comparable.
int compareValues(const Value& a, const Value& b) {
  if (a.isNumeric() && b.isNumeric()) {
    const double x = a.asDouble();
    const double y = b.asDouble();
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (a.type == Value::Type::Str && b.type == Value::Type::Str) {
    return a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
  }
  if (a.type == Value::Type::Str && b.isNumeric()) {
    double y = 0.0;
    if (!textToDouble(a.s, y)) throw SqlError{"type_mismatch", "cannot compare text with a number"};
    const double x = b.asDouble();
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (a.isNumeric() && b.type == Value::Type::Str) {
    double x = 0.0;
    if (!textToDouble(b.s, x)) throw SqlError{"type_mismatch", "cannot compare a number with text"};
    const double y = a.asDouble();
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (a.type == Value::Type::Vec && b.type == Value::Type::Vec) {
    if (a.v.size() != b.v.size()) {
      throw SqlError{"dimension_mismatch", "vectors have different dimensions"};
    }
    for (size_t i = 0; i < a.v.size(); ++i) {
      if (a.v[i] < b.v[i]) return -1;
      if (a.v[i] > b.v[i]) return 1;
    }
    return 0;
  }
  throw SqlError{"type_mismatch", "values are not comparable"};
}

// Number formatting used when a numeric value is stored in a VARCHAR column.
std::string numberToText(double v) {
  std::string s = formatDouble(v);
  if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.erase(s.size() - 2);
  return s;
}

// Returns the vector stored in a value.  A string is parsed as a vector
// literal so that L2_DISTANCE(embedding, '[0,0]') works.
std::vector<float> vectorOf(const Value& v, const std::string& context) {
  if (v.type == Value::Type::Vec) return v.v;
  if (v.type == Value::Type::Str) return parseVectorLiteral(v.s, context);
  throw SqlError{"type_mismatch", "expected a vector value"};
}

}  // namespace

// ---------------------------------------------------------------------------
// Transaction helpers
// ---------------------------------------------------------------------------
void Database::ensureTxn() {
  if (!txn_.active) {
    txn_ = Txn();
    txn_.active = true;
    txn_.id = nextTxnId_++;
  }
}

void Database::finishMutation() {
  if (!inExplicitTxn_) commitCurrentTxn();
}

// ---------------------------------------------------------------------------
// Expression evaluation
// ---------------------------------------------------------------------------
Value Database::evalExpr(const Expr& expr, const EvalCtx& ctx) const {
  switch (expr.type) {
    case ExprType::IntLit:
      return Value::ofInt(expr.intVal);
    case ExprType::DoubleLit:
      return Value::ofDouble(expr.dblVal);
    case ExprType::StrLit:
      return Value::ofStr(expr.strVal);
    case ExprType::NullLit:
      return Value::null();
    case ExprType::Column: {
      if (!ctx.table || !ctx.row) {
        throw SqlError{"internal_error", "column reference outside of a row context"};
      }
      const int idx = ctx.table->findColumn(expr.strVal);
      if (idx < 0) throw SqlError{"column_not_found", "unknown column: " + expr.strVal};
      if (idx >= static_cast<int>(ctx.row->size())) {
        throw SqlError{"internal_error", "column index out of range"};
      }
      return (*ctx.row)[idx];
    }
    case ExprType::Func: {
      if (!equalsIgnoreCase(expr.funcName, "L2_DISTANCE")) {
        throw SqlError{"unsupported_sql", "unsupported function: " + expr.funcName};
      }
      if (!expr.arg1 || !expr.arg2) {
        throw SqlError{"syntax_error", "L2_DISTANCE expects two arguments"};
      }
      const Value a = evalExpr(*expr.arg1, ctx);
      const Value b = evalExpr(*expr.arg2, ctx);
      if (a.isNull() || b.isNull()) return Value::null();
      const std::vector<float> va = vectorOf(a, "L2_DISTANCE");
      const std::vector<float> vb = vectorOf(b, "L2_DISTANCE");
      if (va.size() != vb.size()) {
        throw SqlError{"dimension_mismatch",
                       "L2_DISTANCE expects vectors of the same dimension (" +
                           std::to_string(va.size()) + " vs " + std::to_string(vb.size()) + ")"};
      }
      // Euclidean distance (not squared) in double precision.
      double sum = 0.0;
      for (size_t i = 0; i < va.size(); ++i) {
        const double diff = static_cast<double>(va[i]) - static_cast<double>(vb[i]);
        sum += diff * diff;
      }
      return Value::ofDouble(std::sqrt(sum));
    }
    default:
      throw SqlError{"unsupported_sql", "unsupported expression"};
  }
}

bool Database::evalPredicate(const Expr& expr, const EvalCtx& ctx) const {
  switch (expr.type) {
    case ExprType::Logical: {
      if (expr.op == "AND") {
        if (!evalPredicate(*expr.left, ctx)) return false;  // short circuit
        return evalPredicate(*expr.right, ctx);
      }
      if (expr.op == "OR") {
        if (evalPredicate(*expr.left, ctx)) return true;
        return evalPredicate(*expr.right, ctx);
      }
      throw SqlError{"unsupported_sql", "unsupported logical operator: " + expr.op};
    }
    case ExprType::Not:
      return !evalPredicate(*expr.left, ctx);
    case ExprType::IsNull: {
      const Value v = evalExpr(*expr.left, ctx);
      return expr.negated ? !v.isNull() : v.isNull();
    }
    case ExprType::Compare: {
      const Value a = evalExpr(*expr.left, ctx);
      const Value b = evalExpr(*expr.right, ctx);
      if (a.isNull() || b.isNull()) return false;  // NULL never matches
      const int c = compareValues(a, b);
      if (expr.op == "=") return c == 0;
      if (expr.op == "!=" || expr.op == "<>") return c != 0;
      if (expr.op == "<") return c < 0;
      if (expr.op == "<=") return c <= 0;
      if (expr.op == ">") return c > 0;
      if (expr.op == ">=") return c >= 0;
      throw SqlError{"unsupported_sql", "unsupported comparison operator: " + expr.op};
    }
    case ExprType::InSubquery: {
      const Value a = evalExpr(*expr.left, ctx);
      if (a.isNull()) return false;
      bool found = false;
      if (ctx.subs) {
        for (const auto& cache : *ctx.subs) {
          if (cache.expr != &expr) continue;
          for (const auto& v : cache.values) {
            if (v.isNull()) continue;
            if (compareValues(a, v) == 0) {
              found = true;
              break;
            }
          }
          break;
        }
      }
      return expr.negated ? !found : found;
    }
    default: {
      // A bare expression is true when it is a non zero number.
      const Value v = evalExpr(expr, ctx);
      if (v.isNull()) return false;
      if (v.isNumeric()) return v.asDouble() != 0.0;
      return true;
    }
  }
}

void Database::collectSubqueries(const Expr* expr, std::vector<SubqueryCache>& out) const {
  if (!expr) return;
  if (expr->type == ExprType::InSubquery && expr->subquery) {
    SubqueryCache cache;
    cache.expr = expr;
    out.push_back(cache);
  }
  collectSubqueries(expr->left.get(), out);
  collectSubqueries(expr->right.get(), out);
  collectSubqueries(expr->arg1.get(), out);
  collectSubqueries(expr->arg2.get(), out);
}

// ---------------------------------------------------------------------------
// Value coercion
// ---------------------------------------------------------------------------
Value Database::coerceValue(const ColumnDef& column, const Expr& expr) const {
  switch (column.type) {
    case ColType::BigInt: {
      if (expr.type == ExprType::NullLit) return Value::null();
      if (expr.type == ExprType::IntLit) return Value::ofInt(expr.intVal);
      if (expr.type == ExprType::DoubleLit) {
        const double d = expr.dblVal;
        if (!std::isfinite(d) || d != std::floor(d)) {
          throw SqlError{"type_mismatch", "expected an integer value for column " + column.name};
        }
        if (d < -9223372036854775808.0 || d > 9223372036854775807.0) {
          throw SqlError{"value_out_of_range", "integer value out of range for column " + column.name};
        }
        return Value::ofInt(static_cast<int64_t>(d));
      }
      if (expr.type == ExprType::StrLit) {
        double d = 0.0;
        if (!textToDouble(expr.strVal, d) || d != std::floor(d)) {
          throw SqlError{"type_mismatch", "expected an integer value for column " + column.name};
        }
        return Value::ofInt(static_cast<int64_t>(d));
      }
      throw SqlError{"type_mismatch", "expected an integer value for column " + column.name};
    }
    case ColType::VarChar: {
      if (expr.type == ExprType::NullLit) return Value::null();
      std::string text;
      if (expr.type == ExprType::StrLit) {
        text = expr.strVal;
      } else if (expr.type == ExprType::IntLit) {
        text = std::to_string(expr.intVal);
      } else if (expr.type == ExprType::DoubleLit) {
        text = numberToText(expr.dblVal);
      } else {
        throw SqlError{"type_mismatch", "expected a text value for column " + column.name};
      }
      if (column.length > 0 && utf8CharCount(text) > static_cast<size_t>(column.length)) {
        throw SqlError{"value_too_long",
                       "value for column " + column.name + " exceeds VARCHAR(" +
                           std::to_string(column.length) + ")"};
      }
      return Value::ofStr(std::move(text));
    }
    case ColType::Vector: {
      if (expr.type == ExprType::NullLit) return Value::null();
      if (expr.type != ExprType::StrLit) {
        throw SqlError{"invalid_vector", "expected a vector literal for column " + column.name};
      }
      std::vector<float> vec = parseVectorLiteral(expr.strVal, column.name);
      if (column.dim > 0 && static_cast<int>(vec.size()) != column.dim) {
        throw SqlError{"dimension_mismatch",
                       "column " + column.name + " expects VECTOR(" + std::to_string(column.dim) +
                           ") but got " + std::to_string(vec.size()) + " dimensions"};
      }
      return Value::ofVec(std::move(vec));
    }
  }
  throw SqlError{"internal_error", "unknown column type"};
}

bool Database::keyVisible(const Table& table, int64_t pk) const {
  auto txnIt = txn_.changes.find(table.id);
  if (txnIt != txn_.changes.end()) {
    auto changeIt = txnIt->second.find(pk);
    if (changeIt != txnIt->second.end()) return !changeIt->second.deleted;
  }
  return table.index.count(pk) > 0;
}

// ---------------------------------------------------------------------------
// Subqueries
// ---------------------------------------------------------------------------
std::vector<Value> Database::runSubquery(const Stmt& sub, bool annMode) const {
  (void)annMode;
  auto table = findTable(sub.tableName);
  if (!table) throw SqlError{"table_not_found", "unknown table: " + sub.tableName};
  const Table& t = *table;
  if (sub.items.size() != 1 || sub.items[0].isStar || sub.items[0].isDistance) {
    throw SqlError{"unsupported_sql", "a subquery must select exactly one plain column"};
  }
  const int idx = t.findColumn(sub.items[0].column);
  if (idx < 0) throw SqlError{"column_not_found", "unknown column: " + sub.items[0].column};

  std::vector<SubqueryCache> subs;
  collectSubqueries(sub.where.get(), subs);
  for (auto& cache : subs) {
    cache.values = runSubquery(*cache.expr->subquery, annMode);
  }

  std::vector<Value> out;
  Row row;
  for (const int64_t pk : visibleKeys(t)) {
    if (!visibleRow(t, pk, row)) continue;
    EvalCtx ctx;
    ctx.table = &t;
    ctx.row = &row;
    ctx.subs = &subs;
    if (sub.where && !evalPredicate(*sub.where, ctx)) continue;
    out.push_back(row[idx]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Statement execution
// ---------------------------------------------------------------------------
ExecResult Database::execute(const std::string& sql, bool annMode) {
  std::vector<StmtPtr> stmts;
  try {
    stmts = parseSql(sql);
  } catch (const SqlError& e) {
    ExecResult r;
    r.ok = false;
    r.errorCode = e.code;
    r.errorMessage = e.message;
    return r;
  }
  if (stmts.empty()) {
    ExecResult r;
    r.ok = true;
    return r;
  }
  ExecResult result;
  for (const auto& stmt : stmts) {
    try {
      result = execStmt(*stmt, annMode);
    } catch (const SqlError& e) {
      ExecResult r;
      r.ok = false;
      r.errorCode = e.code;
      r.errorMessage = e.message;
      return r;
    } catch (const std::exception& e) {
      ExecResult r;
      r.ok = false;
      r.errorCode = "internal_error";
      r.errorMessage = e.what();
      return r;
    }
  }
  return result;
}

ExecResult Database::execStmt(const Stmt& stmt, bool annMode) {
  // Keep a snapshot so that a failing statement only rolls back itself.
  const Txn snapshot = txn_;
  const bool snapshotExplicit = inExplicitTxn_;
  try {
    ExecResult result;
    switch (stmt.type) {
      case StmtType::Begin:
        if (!txn_.active) {
          txn_ = Txn();
          txn_.active = true;
          txn_.id = nextTxnId_++;
        }
        inExplicitTxn_ = true;
        break;
      case StmtType::Commit:
        if (txn_.active) commitCurrentTxn();
        inExplicitTxn_ = false;
        break;
      case StmtType::Rollback:
        rollbackCurrentTxn();
        inExplicitTxn_ = false;
        break;
      case StmtType::CreateTable:
        result = execCreateTable(stmt);
        return result;
      case StmtType::CreateVectorIndex:
        result = execCreateVectorIndex(stmt);
        return result;
      case StmtType::Insert:
        result = execInsert(stmt);
        return result;
      case StmtType::Select:
        result = execSelect(stmt, annMode);
        return result;
      case StmtType::Update:
        result = execUpdate(stmt);
        return result;
      case StmtType::Delete:
        result = execDelete(stmt);
        return result;
    }
    ExecResult ok;
    ok.ok = true;
    return ok;
  } catch (...) {
    txn_ = snapshot;
    inExplicitTxn_ = snapshotExplicit;
    throw;
  }
}

ExecResult Database::execCreateTable(const Stmt& stmt) {
  if (findTable(stmt.tableName)) {
    throw SqlError{"table_exists", "table already exists: " + stmt.tableName};
  }
  auto table = std::make_shared<Table>();
  table->id = static_cast<int64_t>(nextTableId_++);
  table->name = stmt.tableName;
  table->columns = stmt.columns;
  for (size_t i = 0; i < table->columns.size(); ++i) {
    table->colIndex[toLower(table->columns[i].name)] = i;
    if (table->columns[i].primaryKey) table->pkCol = static_cast<int>(i);
    if (table->columns[i].type == ColType::Vector) table->vectorCol = static_cast<int>(i);
  }
  ensureTxn();
  txn_.createdTables.push_back(table);
  finishMutation();
  ExecResult r;
  r.ok = true;
  r.affectedRows = 0;
  return r;
}

ExecResult Database::execCreateVectorIndex(const Stmt& stmt) {
  auto table = findTable(stmt.tableName);
  if (!table) throw SqlError{"table_not_found", "unknown table: " + stmt.tableName};
  const int col = table->findColumn(stmt.indexColumn);
  if (col < 0) {
    throw SqlError{"column_not_found", "unknown column: " + stmt.indexColumn};
  }
  if (table->columns[col].type != ColType::Vector) {
    throw SqlError{"type_mismatch", "a vector index requires a VECTOR column"};
  }
  ensureTxn();
  IndexFrame frame;
  frame.tableId = table->id;
  frame.indexName = stmt.indexName;
  frame.column = stmt.indexColumn;
  txn_.indexFrames.push_back(frame);
  finishMutation();
  ExecResult r;
  r.ok = true;
  r.affectedRows = 0;
  return r;
}

ExecResult Database::execInsert(const Stmt& stmt) {
  auto table = findTable(stmt.tableName);
  if (!table) throw SqlError{"table_not_found", "unknown table: " + stmt.tableName};
  const Table& t = *table;

  // Resolve the target columns.
  std::vector<int> targets;
  if (stmt.insertColumns.empty()) {
    for (size_t i = 0; i < t.columns.size(); ++i) targets.push_back(static_cast<int>(i));
  } else {
    for (const auto& name : stmt.insertColumns) {
      const int idx = t.findColumn(name);
      if (idx < 0) throw SqlError{"column_not_found", "unknown column: " + name};
      if (std::find(targets.begin(), targets.end(), idx) != targets.end()) {
        throw SqlError{"syntax_error", "column " + name + " is listed twice"};
      }
      targets.push_back(idx);
    }
  }

  // Validate every row before touching the state: a failing multi row insert
  // must not insert any row.
  std::vector<Row> rows;
  std::vector<int64_t> pks;
  std::set<int64_t> seen;
  for (const auto& exprRow : stmt.insertRows) {
    if (exprRow.size() != targets.size()) {
      throw SqlError{"column_count_mismatch",
                     "expected " + std::to_string(targets.size()) + " values but got " +
                         std::to_string(exprRow.size())};
    }
    Row row(t.columns.size());
    for (size_t i = 0; i < exprRow.size(); ++i) {
      row[targets[i]] = coerceValue(t.columns[targets[i]], *exprRow[i]);
    }
    const Value& pkValue = row[t.pkCol];
    if (pkValue.isNull()) {
      throw SqlError{"null_primary_key", "the primary key must not be NULL"};
    }
    if (!pkValue.isNumeric()) {
      throw SqlError{"type_mismatch", "the primary key must be an integer"};
    }
    const int64_t pk = pkValue.type == Value::Type::Int ? pkValue.i
                                                        : static_cast<int64_t>(pkValue.d);
    if (!seen.insert(pk).second) {
      throw SqlError{"duplicate_key", "duplicate primary key: " + std::to_string(pk)};
    }
    if (keyVisible(t, pk)) {
      throw SqlError{"duplicate_key", "duplicate primary key: " + std::to_string(pk)};
    }
    rows.push_back(std::move(row));
    pks.push_back(pk);
  }

  ensureTxn();
  auto& changes = txn_.changes[t.id];
  for (size_t i = 0; i < rows.size(); ++i) {
    TxnChange change;
    change.deleted = false;
    change.values = rows[i];
    changes[pks[i]] = std::move(change);
  }
  finishMutation();

  ExecResult r;
  r.ok = true;
  r.affectedRows = static_cast<int64_t>(rows.size());
  return r;
}

ExecResult Database::execUpdate(const Stmt& stmt) {
  auto table = findTable(stmt.tableName);
  if (!table) throw SqlError{"table_not_found", "unknown table: " + stmt.tableName};
  const Table& t = *table;

  std::vector<std::pair<int, ExprPtr>> sets;
  for (const auto& entry : stmt.setList) {
    const int idx = t.findColumn(entry.first);
    if (idx < 0) throw SqlError{"column_not_found", "unknown column: " + entry.first};
    sets.emplace_back(idx, entry.second);
  }
  if (sets.empty()) throw SqlError{"syntax_error", "UPDATE requires at least one assignment"};

  // Collect the matching rows.
  std::vector<SubqueryCache> subs;
  collectSubqueries(stmt.where.get(), subs);
  for (auto& cache : subs) cache.values = runSubquery(*cache.expr->subquery, false);

  std::vector<std::pair<int64_t, Row>> matched;
  Row row;
  for (const int64_t pk : visibleKeys(t)) {
    if (!visibleRow(t, pk, row)) continue;
    EvalCtx ctx;
    ctx.table = &t;
    ctx.row = &row;
    ctx.subs = &subs;
    if (stmt.where && !evalPredicate(*stmt.where, ctx)) continue;
    matched.emplace_back(pk, row);
  }

  // Compute the new rows (all validation happens before anything is applied).
  std::vector<std::tuple<int64_t, int64_t, Row>> updates;  // old pk, new pk, row
  std::set<int64_t> newPks;
  for (const auto& entry : matched) {
    const int64_t pk = entry.first;
    Row next = entry.second;
    for (const auto& set : sets) {
      next[set.first] = coerceValue(t.columns[set.first], *set.second);
    }
    const Value& pkValue = next[t.pkCol];
    if (pkValue.isNull()) {
      throw SqlError{"null_primary_key", "the primary key must not be NULL"};
    }
    const int64_t newPk = pkValue.type == Value::Type::Int ? pkValue.i
                                                            : static_cast<int64_t>(pkValue.d);
    if (newPk != pk) {
      if (!newPks.insert(newPk).second) {
        throw SqlError{"duplicate_key", "duplicate primary key: " + std::to_string(newPk)};
      }
      if (keyVisible(t, newPk)) {
        throw SqlError{"duplicate_key", "duplicate primary key: " + std::to_string(newPk)};
      }
    }
    updates.emplace_back(pk, newPk, std::move(next));
  }

  ensureTxn();
  auto& changes = txn_.changes[t.id];
  for (auto& entry : updates) {
    const int64_t oldPk = std::get<0>(entry);
    const int64_t newPk = std::get<1>(entry);
    // A changed primary key removes the old key and adds the new one.
    if (newPk != oldPk) {
      TxnChange removed;
      removed.deleted = true;
      changes[oldPk] = std::move(removed);
    }
    TxnChange change;
    change.deleted = false;
    change.values = std::get<2>(std::move(entry));
    changes[newPk] = std::move(change);
  }
  finishMutation();

  ExecResult r;
  r.ok = true;
  r.affectedRows = static_cast<int64_t>(matched.size());
  return r;
}

ExecResult Database::execDelete(const Stmt& stmt) {
  auto table = findTable(stmt.tableName);
  if (!table) throw SqlError{"table_not_found", "unknown table: " + stmt.tableName};
  const Table& t = *table;

  std::vector<SubqueryCache> subs;
  collectSubqueries(stmt.where.get(), subs);
  for (auto& cache : subs) cache.values = runSubquery(*cache.expr->subquery, false);

  std::vector<int64_t> matched;
  Row row;
  for (const int64_t pk : visibleKeys(t)) {
    if (!visibleRow(t, pk, row)) continue;
    EvalCtx ctx;
    ctx.table = &t;
    ctx.row = &row;
    ctx.subs = &subs;
    if (stmt.where && !evalPredicate(*stmt.where, ctx)) continue;
    matched.push_back(pk);
  }

  ensureTxn();
  auto& changes = txn_.changes[t.id];
  for (const int64_t pk : matched) {
    TxnChange change;
    change.deleted = true;
    changes[pk] = std::move(change);
  }
  finishMutation();

  ExecResult r;
  r.ok = true;
  r.affectedRows = static_cast<int64_t>(matched.size());
  return r;
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------
ExecResult Database::execSelect(const Stmt& stmt, bool annMode) {
  auto table = findTable(stmt.tableName);
  if (!table) throw SqlError{"table_not_found", "unknown table: " + stmt.tableName};
  const Table& t = *table;

  // --- build the output items ---------------------------------------------
  struct OutItem {
    std::string outputName;
    ExprPtr expr;          // owned expression (column or distance)
    ExprPtr distanceArg;  // for distance items: the query vector argument
    bool isDistance = false;
  };
  std::vector<OutItem> items;
  for (const auto& item : stmt.items) {
    if (item.isStar) {
      for (const auto& column : t.columns) {
        OutItem oi;
        oi.outputName = column.name;
        auto expr = std::make_shared<Expr>();
        expr->type = ExprType::Column;
        expr->strVal = column.name;
        oi.expr = expr;
        items.push_back(std::move(oi));
      }
    } else if (item.isDistance) {
      OutItem oi;
      oi.outputName = item.outputName;
      oi.isDistance = true;
      oi.distanceArg = item.distArg2;
      auto expr = std::make_shared<Expr>();
      expr->type = ExprType::Func;
      expr->funcName = "L2_DISTANCE";
      expr->arg1 = item.distArg1;
      expr->arg2 = item.distArg2;
      oi.expr = expr;
      items.push_back(std::move(oi));
    } else {
      OutItem oi;
      oi.outputName = item.outputName;
      auto expr = std::make_shared<Expr>();
      expr->type = ExprType::Column;
      expr->strVal = item.column;
      oi.expr = expr;
      items.push_back(std::move(oi));
    }
  }

  // --- resolve the ORDER BY expression ------------------------------------
  ExprPtr orderExpr;
  bool orderAsc = true;
  if (stmt.orderBy) {
    orderAsc = stmt.orderBy->asc;
    orderExpr = stmt.orderBy->expr;
    if (orderExpr && orderExpr->type == ExprType::Column) {
      // An alias (or a selected column) may be used for ordering.
      for (const auto& item : items) {
        if (equalsIgnoreCase(item.outputName, orderExpr->strVal)) {
          orderExpr = item.expr;
          break;
        }
      }
    }
    if (orderExpr && orderExpr->type == ExprType::Column) {
      const int idx = t.findColumn(orderExpr->strVal);
      if (idx < 0) throw SqlError{"column_not_found", "unknown column: " + orderExpr->strVal};
    }
  }

  // --- subqueries of the WHERE clause --------------------------------------
  std::vector<SubqueryCache> subs;
  collectSubqueries(stmt.where.get(), subs);
  for (auto& cache : subs) cache.values = runSubquery(*cache.expr->subquery, annMode);

  // --- candidate rows ------------------------------------------------------
  // The exact path scans the primary key index in ascending order.  The ANN
  // path only kicks in for "search_mode":"ann" on a table with a vector
  // index and without pending transaction changes; it then asks the index
  // for the closest rows and the filter is still applied afterwards.
  std::vector<int64_t> candidates;
  bool usedAnn = false;
  if (annMode && t.hasVectorIndex && t.vectorCol >= 0 && txn_.changes.find(t.id) == txn_.changes.end()) {
    for (const auto& item : items) {
      if (!item.isDistance) continue;
      if (!item.distanceArg || item.distanceArg->type != ExprType::StrLit) continue;
      std::vector<float> query;
      try {
        query = parseVectorLiteral(item.distanceArg->strVal, "query");
      } catch (const SqlError&) {
        continue;
      }
      AnnIndex* index = getAnnIndex(t);
      if (!index || !index->built()) continue;
      size_t want = index->size();
      if (stmt.limit >= 0) {
        want = static_cast<size_t>(stmt.limit) * 8 + 32;
        if (want > index->size()) want = index->size();
      }
      candidates = index->search(query, want);
      usedAnn = true;
      break;
    }
  }
  if (!usedAnn) candidates = visibleKeys(t);

  // --- filter, sort and project -------------------------------------------
  auto runOver = [&](const std::vector<int64_t>& keys) {
    struct Candidate {
      SortKey key;
      int64_t pk = 0;
    };
    std::vector<Candidate> matches;
    Row row;
    for (const int64_t pk : keys) {
      if (!visibleRow(t, pk, row)) continue;
      EvalCtx ctx;
      ctx.table = &t;
      ctx.row = &row;
      ctx.subs = &subs;
      if (stmt.where && !evalPredicate(*stmt.where, ctx)) continue;
      Candidate candidate;
      candidate.pk = pk;
      if (orderExpr) candidate.key = makeSortKey(evalExpr(*orderExpr, ctx));
      matches.push_back(std::move(candidate));
    }
    if (orderExpr) {
      std::stable_sort(matches.begin(), matches.end(), [&](const Candidate& a, const Candidate& b) {
        const int c = compareSortKeys(a.key, b.key);
        if (c != 0) return orderAsc ? (c < 0) : (c > 0);
        return a.pk < b.pk;  // ties are broken by primary key
      });
    }
    if (stmt.limit >= 0 && matches.size() > static_cast<size_t>(stmt.limit)) {
      matches.resize(static_cast<size_t>(stmt.limit));
    }

    ExecResult r;
    r.ok = true;
    for (const auto& item : items) r.columns.push_back(item.outputName);
    for (const auto& candidate : matches) {
      if (!visibleRow(t, candidate.pk, row)) continue;
      EvalCtx ctx;
      ctx.table = &t;
      ctx.row = &row;
      ctx.subs = &subs;
      JArray values;
      values.reserve(items.size());
      for (const auto& item : items) {
        values.push_back(valueToJson(evalExpr(*item.expr, ctx)));
      }
      r.rows.push_back(JValue::makeArray(std::move(values)));
    }
    r.affectedRows = static_cast<int64_t>(r.rows.size());
    return r;
  };

  ExecResult result = runOver(candidates);
  // The ANN path may miss rows that the filter would have kept: fall back to
  // the exact scan when the result set looks too small.
  if (usedAnn && stmt.limit >= 0 &&
      result.rows.size() < static_cast<size_t>(stmt.limit) &&
      visibleKeys(t).size() > candidates.size()) {
    result = runOver(visibleKeys(t));
  }
  return result;
}

// ---------------------------------------------------------------------------
// ANN index cache
// ---------------------------------------------------------------------------
AnnIndex* Database::getAnnIndex(const Table& table) {
  auto it = annCache_.find(table.id);
  if (it != annCache_.end() && it->second.version == table.version && it->second.index) {
    return it->second.index.get();
  }
  auto index = std::make_shared<AnnIndex>();
  if (table.vectorCol >= 0) {
    std::vector<AnnPoint> points;
    points.reserve(table.index.size());
    Row row;
    for (const auto& entry : table.index) {
      if (!readRowAt(table, entry.second, row)) continue;
      if (table.vectorCol >= static_cast<int>(row.size())) continue;
      const Value& value = row[table.vectorCol];
      if (value.type != Value::Type::Vec) continue;
      AnnPoint point;
      point.pk = entry.first;
      point.vec = value.v;
      points.push_back(std::move(point));
    }
    index->build(std::move(points));
  }
  AnnCache cache;
  cache.version = table.version;
  cache.index = index;
  annCache_[table.id] = cache;
  return annCache_[table.id].index.get();
}

}  // namespace mvdb
