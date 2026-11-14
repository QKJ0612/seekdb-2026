// sql.cpp - lexer and parser for the supported SQL subset.
//
// The parser is intentionally small: it only understands the statements that
// the competition exercises.  Anything outside of that subset is rejected
// with the error code "unsupported_sql" so that a statement never silently
// succeeds with a wrong effect.
#include "sql.hpp"

#include <cctype>
#include <cstdlib>
#include <cerrno>

#include "util.hpp"

namespace mvdb {
namespace {

[[noreturn]] void fail(const char* code, const std::string& message) { throw SqlError{code, message}; }

enum class TokKind { End, Ident, Int, Double, Str, Punct };

struct Tok {
  TokKind kind = TokKind::End;
  std::string text;  // identifier text, punctuation or decoded string literal
  int64_t ival = 0;
  double dval = 0.0;
};

inline bool isIdentStart(unsigned char c) {
  return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80;
}
inline bool isIdentChar(unsigned char c) {
  return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------
std::vector<Tok> tokenize(const std::string& src) {
  std::vector<Tok> out;
  const size_t n = src.size();
  size_t i = 0;
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') { ++i; continue; }
    // Comments: "# ...", "-- ...", "/* ... */".
    if (c == '#') {
      while (i < n && src[i] != '\n') ++i;
      continue;
    }
    if (c == '-' && i + 1 < n && src[i + 1] == '-' &&
        (i == 0 || std::isspace(static_cast<unsigned char>(src[i - 1])))) {
      while (i < n && src[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && src[i + 1] == '*') {
      size_t j = i + 2;
      while (j + 1 < n && !(src[j] == '*' && src[j + 1] == '/')) ++j;
      if (j + 1 >= n) fail("syntax_error", "unterminated comment");
      i = j + 2;
      continue;
    }
    // String literal.
    if (c == '\'') {
      std::string value;
      ++i;
      while (true) {
        if (i >= n) fail("syntax_error", "unterminated string literal");
        char ch = src[i++];
        if (ch == '\'') {
          if (i < n && src[i] == '\'') { value.push_back('\''); ++i; continue; }
          break;
        }
        if (ch == '\\' && i < n) {
          char e = src[i++];
          switch (e) {
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            case 'r': value.push_back('\r'); break;
            case '0': value.push_back('\0'); break;
            case 'Z': value.push_back('\032'); break;
            case '\\': value.push_back('\\'); break;
            case '\'': value.push_back('\''); break;
            case '"': value.push_back('"'); break;
            default: value.push_back(e); break;
          }
          continue;
        }
        value.push_back(ch);
      }
      Tok t;
      t.kind = TokKind::Str;
      t.text = std::move(value);
      out.push_back(std::move(t));
      continue;
    }
    // Quoted identifier (backticks).
    if (c == '`') {
      std::string value;
      ++i;
      while (true) {
        if (i >= n) fail("syntax_error", "unterminated quoted identifier");
        char ch = src[i++];
        if (ch == '`') {
          if (i < n && src[i] == '`') { value.push_back('`'); ++i; continue; }
          break;
        }
        value.push_back(ch);
      }
      Tok t;
      t.kind = TokKind::Ident;
      t.text = std::move(value);
      out.push_back(std::move(t));
      continue;
    }
    // Number.
    if (std::isdigit(c) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
      size_t start = i;
      bool isDouble = false;
      while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
      if (i < n && src[i] == '.') {
        isDouble = true;
        ++i;
        while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
      }
      if (i < n && (src[i] == 'e' || src[i] == 'E')) {
        isDouble = true;
        ++i;
        if (i < n && (src[i] == '+' || src[i] == '-')) ++i;
        if (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) {
          while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
        } else {
          fail("syntax_error", "malformed number literal");
        }
      }
      std::string text = src.substr(start, i - start);
      Tok t;
      t.text = text;
      if (isDouble) {
        t.kind = TokKind::Double;
        t.dval = std::strtod(text.c_str(), nullptr);
      } else {
        errno = 0;
        char* endp = nullptr;
        long long v = std::strtoll(text.c_str(), &endp, 10);
        if (errno == ERANGE) fail("syntax_error", "integer literal out of range: " + text);
        t.kind = TokKind::Int;
        t.ival = static_cast<int64_t>(v);
      }
      out.push_back(std::move(t));
      continue;
    }
    // Identifier / keyword.
    if (isIdentStart(c)) {
      size_t start = i;
      while (i < n && isIdentChar(static_cast<unsigned char>(src[i]))) ++i;
      Tok t;
      t.kind = TokKind::Ident;
      t.text = src.substr(start, i - start);
      out.push_back(std::move(t));
      continue;
    }
    // Punctuation.
    {
      Tok t;
      t.kind = TokKind::Punct;
      if (i + 1 < n) {
        std::string two = src.substr(i, 2);
        if (two == "!=" || two == "<>" || two == "<=" || two == ">=" || two == "||") {
          t.text = two;
          i += 2;
          out.push_back(std::move(t));
          continue;
        }
      }
      char ch = src[i];
      if (ch == '(' || ch == ')' || ch == ',' || ch == ';' || ch == '=' || ch == '<' || ch == '>' ||
          ch == '*' || ch == '+' || ch == '-' || ch == '/' || ch == '?' || ch == '.') {
        t.text = std::string(1, ch);
        ++i;
        out.push_back(std::move(t));
        continue;
      }
    }
    fail("syntax_error", std::string("unexpected character '") + static_cast<char>(c) + "'");
  }
  Tok end;
  end.kind = TokKind::End;
  out.push_back(end);
  return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
 public:
  explicit Parser(const std::string& sql) : toks_(tokenize(sql)) {}

  std::vector<StmtPtr> parseAll() {
    std::vector<StmtPtr> stmts;
    while (peek().kind != TokKind::End) {
      if (peek().kind == TokKind::Punct && peek().text == ";") { ++pos_; continue; }
      stmts.push_back(parseStatement());
      if (peek().kind == TokKind::Punct && peek().text == ";") { ++pos_; continue; }
      if (peek().kind != TokKind::End) fail("syntax_error", "unexpected trailing input");
    }
    if (stmts.empty()) fail("syntax_error", "empty statement");
    return stmts;
  }

 private:
  std::vector<Tok> toks_;
  size_t pos_ = 0;

  const Tok& peek(size_t ahead = 0) const {
    size_t idx = pos_ + ahead;
    return idx < toks_.size() ? toks_[idx] : toks_.back();
  }
  const Tok& next() { return toks_[pos_ < toks_.size() - 1 ? pos_++ : pos_]; }
  bool isPunct(const char* p) const {
    return peek().kind == TokKind::Punct && peek().text == p;
  }
  bool isKeyword(const char* kw) const {
    return peek().kind == TokKind::Ident && equalsIgnoreCase(peek().text, kw);
  }
  bool isKeywordAt(size_t ahead, const char* kw) const {
    const Tok& t = peek(ahead);
    return t.kind == TokKind::Ident && equalsIgnoreCase(t.text, kw);
  }
  void expectPunct(const char* p) {
    if (!isPunct(p)) fail("syntax_error", std::string("expected '") + p + "'");
    ++pos_;
  }
  void expectKeyword(const char* kw) {
    if (!isKeyword(kw)) fail("syntax_error", std::string("expected keyword ") + kw);
    ++pos_;
  }
  std::string expectIdent() {
    if (peek().kind != TokKind::Ident) fail("syntax_error", "expected identifier");
    return next().text;
  }
  int64_t expectInt() {
    if (peek().kind != TokKind::Int) fail("syntax_error", "expected integer literal");
    return next().ival;
  }

  StmtPtr parseStatement() {
    if (peek().kind != TokKind::Ident) fail("syntax_error", "expected statement");
    const std::string kw = toUpper(peek().text);
    if (kw == "SELECT") return parseSelect();
    if (kw == "INSERT") return parseInsert();
    if (kw == "UPDATE") return parseUpdate();
    if (kw == "DELETE") return parseDelete();
    if (kw == "CREATE") return parseCreate();
    if (kw == "BEGIN") { ++pos_; auto s = std::make_shared<Stmt>(); s->type = StmtType::Begin; return s; }
    if (kw == "START") {
      ++pos_;
      if (isKeyword("TRANSACTION")) ++pos_;
      auto s = std::make_shared<Stmt>();
      s->type = StmtType::Begin;
      return s;
    }
    if (kw == "COMMIT") { ++pos_; auto s = std::make_shared<Stmt>(); s->type = StmtType::Commit; return s; }
    if (kw == "ROLLBACK") { ++pos_; auto s = std::make_shared<Stmt>(); s->type = StmtType::Rollback; return s; }
    fail("unsupported_sql", "unsupported statement: " + kw);
  }

  // ---- CREATE ------------------------------------------------------------
  StmtPtr parseCreate() {
    expectKeyword("CREATE");
    if (isKeyword("TABLE")) return parseCreateTable();
    if (isKeyword("VECTOR")) {
      ++pos_;
      expectKeyword("INDEX");
      return parseCreateVectorIndex();
    }
    fail("unsupported_sql", "unsupported CREATE statement");
  }

  StmtPtr parseCreateTable() {
    expectKeyword("TABLE");
    auto s = std::make_shared<Stmt>();
    s->type = StmtType::CreateTable;
    s->tableName = expectIdent();
    expectPunct("(");
    bool sawPrimaryKey = false;
    while (true) {
      if (isPunct(")")) break;
      // Table level constraint: PRIMARY KEY (col)
      if (isKeyword("PRIMARY")) {
        ++pos_;
        expectKeyword("KEY");
        expectPunct("(");
        std::string col = expectIdent();
        expectPunct(")");
        for (auto& c : s->columns) {
          if (equalsIgnoreCase(c.name, col)) {
            c.primaryKey = true;
            sawPrimaryKey = true;
          }
        }
      } else {
        ColumnDef def;
        def.name = expectIdent();
        const std::string typeKw = toUpper(expectIdent());
        if (typeKw == "BIGINT" || typeKw == "INT" || typeKw == "INTEGER") {
          def.type = ColType::BigInt;
        } else if (typeKw == "VARCHAR") {
          expectPunct("(");
          int64_t len = expectInt();
          expectPunct(")");
          if (len <= 0 || len > 1048576) fail("unsupported_sql", "invalid VARCHAR length");
          def.type = ColType::VarChar;
          def.length = static_cast<int>(len);
        } else if (typeKw == "VECTOR") {
          expectPunct("(");
          int64_t dim = expectInt();
          expectPunct(")");
          if (dim <= 0 || dim > 65536) fail("unsupported_sql", "invalid VECTOR dimension");
          def.type = ColType::Vector;
          def.dim = static_cast<int>(dim);
        } else {
          fail("unsupported_sql", "unsupported column type: " + typeKw);
        }
        // Column modifiers.
        while (true) {
          if (isKeyword("PRIMARY")) {
            ++pos_;
            expectKeyword("KEY");
            def.primaryKey = true;
          } else if (isKeyword("NOT")) {
            ++pos_;
            expectKeyword("NULL");
          } else if (isKeyword("NULL")) {
            ++pos_;
          } else if (isKeyword("AUTO_INCREMENT")) {
            ++pos_;
          } else {
            break;
          }
        }
        if (def.primaryKey) sawPrimaryKey = true;
        s->columns.push_back(std::move(def));
      }
      if (isPunct(",")) { ++pos_; continue; }
      break;
    }
    expectPunct(")");
    validateSchema(*s, sawPrimaryKey);
    return s;
  }

  static void validateSchema(const Stmt& s, bool sawPrimaryKey) {
    if (!sawPrimaryKey) fail("unsupported_sql", "table must declare a BIGINT PRIMARY KEY");
    int pkCount = 0, vectorCount = 0;
    for (const auto& c : s.columns) {
      if (c.primaryKey) {
        ++pkCount;
        if (c.type != ColType::BigInt) fail("unsupported_sql", "PRIMARY KEY must be BIGINT");
      }
      if (c.type == ColType::Vector) ++vectorCount;
    }
    if (pkCount != 1) fail("unsupported_sql", "exactly one PRIMARY KEY column is required");
    if (vectorCount > 1) fail("unsupported_sql", "at most one VECTOR column is supported");
  }

  StmtPtr parseCreateVectorIndex() {
    auto s = std::make_shared<Stmt>();
    s->type = StmtType::CreateVectorIndex;
    if (peek().kind == TokKind::Ident && !isKeyword("ON")) s->indexName = expectIdent();
    expectKeyword("ON");
    s->tableName = expectIdent();
    expectPunct("(");
    s->indexColumn = expectIdent();
    expectPunct(")");
    return s;
  }

  // ---- INSERT ------------------------------------------------------------
  StmtPtr parseInsert() {
    expectKeyword("INSERT");
    if (isKeyword("INTO")) ++pos_;
    auto s = std::make_shared<Stmt>();
    s->type = StmtType::Insert;
    s->tableName = expectIdent();
    if (isPunct("(")) {
      ++pos_;
      while (true) {
        s->insertColumns.push_back(expectIdent());
        if (isPunct(",")) { ++pos_; continue; }
        break;
      }
      expectPunct(")");
    }
    expectKeyword("VALUES");
    while (true) {
      expectPunct("(");
      std::vector<ExprPtr> row;
      while (true) {
        row.push_back(parseExpr());
        if (isPunct(",")) { ++pos_; continue; }
        break;
      }
      expectPunct(")");
      s->insertRows.push_back(std::move(row));
      if (isPunct(",")) { ++pos_; continue; }
      break;
    }
    return s;
  }

  // ---- SELECT ------------------------------------------------------------
  StmtPtr parseSelect() {
    expectKeyword("SELECT");
    auto s = std::make_shared<Stmt>();
    s->type = StmtType::Select;
    while (true) {
      SelectItem item;
      if (isPunct("*")) {
        ++pos_;
        item.isStar = true;
        item.outputName = "*";
      } else {
        ExprPtr e = parseExpr();
        if (e->type == ExprType::Func) {
          if (!equalsIgnoreCase(e->funcName, "L2_DISTANCE")) fail("unsupported_sql", "unsupported function");
          item.isDistance = true;
          item.distArg1 = e->arg1;
          item.distArg2 = e->arg2;
          item.outputName = "distance";
        } else if (e->type == ExprType::Column) {
          item.column = e->strVal;
          item.outputName = e->strVal;
        } else {
          fail("unsupported_sql", "unsupported select expression");
        }
      }
      if (isKeyword("AS")) {
        ++pos_;
        item.outputName = expectIdent();
      } else if (peek().kind == TokKind::Ident && !isKeyword("FROM") && !isKeyword("WHERE") &&
                 !isKeyword("ORDER") && !isKeyword("LIMIT")) {
        item.outputName = expectIdent();
      }
      s->items.push_back(std::move(item));
      if (isPunct(",")) { ++pos_; continue; }
      break;
    }
    expectKeyword("FROM");
    s->tableName = expectIdent();
    if (isKeyword("WHERE")) {
      ++pos_;
      s->where = parseExpr();
    }
    if (isKeyword("ORDER")) {
      ++pos_;
      expectKeyword("BY");
      auto ob = std::make_shared<OrderBy>();
      ob->expr = parseExpr();
      if (isKeyword("ASC")) { ++pos_; ob->asc = true; }
      else if (isKeyword("DESC")) { ++pos_; ob->asc = false; }
      s->orderBy = ob;
    }
    if (isKeyword("LIMIT")) {
      ++pos_;
      int64_t limit = expectInt();
      if (limit < 0) fail("syntax_error", "LIMIT must not be negative");
      s->limit = limit;
    }
    return s;
  }

  // ---- UPDATE / DELETE ---------------------------------------------------
  StmtPtr parseUpdate() {
    expectKeyword("UPDATE");
    auto s = std::make_shared<Stmt>();
    s->type = StmtType::Update;
    s->tableName = expectIdent();
    expectKeyword("SET");
    while (true) {
      std::string col = expectIdent();
      expectPunct("=");
      ExprPtr value = parseExpr();
      s->setList.emplace_back(col, std::move(value));
      if (isPunct(",")) { ++pos_; continue; }
      break;
    }
    if (isKeyword("WHERE")) {
      ++pos_;
      s->where = parseExpr();
    }
    return s;
  }

  StmtPtr parseDelete() {
    expectKeyword("DELETE");
    expectKeyword("FROM");
    auto s = std::make_shared<Stmt>();
    s->type = StmtType::Delete;
    s->tableName = expectIdent();
    if (isKeyword("WHERE")) {
      ++pos_;
      s->where = parseExpr();
    }
    return s;
  }

  // ---- expressions -------------------------------------------------------
  ExprPtr parseExpr() { return parseOr(); }

  ExprPtr parseOr() {
    ExprPtr left = parseAnd();
    while (isKeyword("OR")) {
      ++pos_;
      ExprPtr right = parseAnd();
      auto e = std::make_shared<Expr>();
      e->type = ExprType::Logical;
      e->op = "OR";
      e->left = std::move(left);
      e->right = std::move(right);
      left = std::move(e);
    }
    return left;
  }

  ExprPtr parseAnd() {
    ExprPtr left = parseNot();
    while (isKeyword("AND")) {
      ++pos_;
      ExprPtr right = parseNot();
      auto e = std::make_shared<Expr>();
      e->type = ExprType::Logical;
      e->op = "AND";
      e->left = std::move(left);
      e->right = std::move(right);
      left = std::move(e);
    }
    return left;
  }

  ExprPtr parseNot() {
    if (isKeyword("NOT")) {
      ++pos_;
      auto e = std::make_shared<Expr>();
      e->type = ExprType::Not;
      e->left = parseNot();
      return e;
    }
    return parsePredicate();
  }

  ExprPtr parsePredicate() {
    ExprPtr left = parsePrimary();
    if (isKeyword("IS")) {
      ++pos_;
      bool negated = false;
      if (isKeyword("NOT")) { ++pos_; negated = true; }
      expectKeyword("NULL");
      auto e = std::make_shared<Expr>();
      e->type = ExprType::IsNull;
      e->negated = negated;
      e->left = std::move(left);
      return e;
    }
    if (isKeyword("IN")) {
      ++pos_;
      expectPunct("(");
      auto sub = parseSelect();
      expectPunct(")");
      auto e = std::make_shared<Expr>();
      e->type = ExprType::InSubquery;
      e->left = std::move(left);
      e->subquery = std::move(sub);
      return e;
    }
    if (isKeyword("NOT") && isKeywordAt(1, "IN")) {
      ++pos_;
      ++pos_;
      expectPunct("(");
      auto sub = parseSelect();
      expectPunct(")");
      auto e = std::make_shared<Expr>();
      e->type = ExprType::InSubquery;
      e->negated = true;
      e->left = std::move(left);
      e->subquery = std::move(sub);
      return e;
    }
    if (peek().kind == TokKind::Punct) {
      const std::string& op = peek().text;
      if (op == "=" || op == "!=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=") {
        ++pos_;
        ExprPtr right = parsePrimary();
        auto e = std::make_shared<Expr>();
        e->type = ExprType::Compare;
        e->op = (op == "<>") ? "!=" : op;
        e->left = std::move(left);
        e->right = std::move(right);
        return e;
      }
    }
    return left;
  }

  ExprPtr parsePrimary() {
    const Tok& t = peek();
    if (t.kind == TokKind::Punct) {
      if (t.text == "(") {
        ++pos_;
        ExprPtr e = parseExpr();
        expectPunct(")");
        return e;
      }
      if (t.text == "-") {
        ++pos_;
        if (peek().kind == TokKind::Int) {
          auto e = std::make_shared<Expr>();
          e->type = ExprType::IntLit;
          e->intVal = -next().ival;
          return e;
        }
        if (peek().kind == TokKind::Double) {
          auto e = std::make_shared<Expr>();
          e->type = ExprType::DoubleLit;
          e->dblVal = -next().dval;
          return e;
        }
        fail("syntax_error", "unsupported unary expression");
      }
      fail("syntax_error", "unexpected token in expression");
    }
    if (t.kind == TokKind::Int) {
      auto e = std::make_shared<Expr>();
      e->type = ExprType::IntLit;
      e->intVal = next().ival;
      return e;
    }
    if (t.kind == TokKind::Double) {
      auto e = std::make_shared<Expr>();
      e->type = ExprType::DoubleLit;
      e->dblVal = next().dval;
      return e;
    }
    if (t.kind == TokKind::Str) {
      auto e = std::make_shared<Expr>();
      e->type = ExprType::StrLit;
      e->strVal = next().text;
      return e;
    }
    // Identifier: keyword literal, column reference or function call.
    if (isKeyword("NULL")) {
      ++pos_;
      auto e = std::make_shared<Expr>();
      e->type = ExprType::NullLit;
      return e;
    }
    if (isKeyword("TRUE") || isKeyword("FALSE")) {
      bool v = isKeyword("TRUE");
      ++pos_;
      auto e = std::make_shared<Expr>();
      e->type = ExprType::IntLit;
      e->intVal = v ? 1 : 0;
      return e;
    }
    std::string name = expectIdent();
    if (isPunct("(")) {
      const std::string func = toUpper(name);
      if (func != "L2_DISTANCE") fail("unsupported_sql", "unsupported function: " + name);
      ++pos_;
      auto e = std::make_shared<Expr>();
      e->type = ExprType::Func;
      e->funcName = "L2_DISTANCE";
      e->arg1 = parseExpr();
      expectPunct(",");
      e->arg2 = parseExpr();
      expectPunct(")");
      return e;
    }
    auto e = std::make_shared<Expr>();
    e->type = ExprType::Column;
    e->strVal = name;
    return e;
  }
};

}  // namespace

std::vector<StmtPtr> parseSql(const std::string& text) {
  Parser parser(text);
  return parser.parseAll();
}

}  // namespace mvdb
