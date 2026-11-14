// json.cpp - implementation of the tiny JSON reader/writer.
#include "json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "util.hpp"

namespace mvdb {
namespace {

class Parser {
 public:
  Parser(const char* begin, const char* end) : p_(begin), end_(end) {}

  bool parse(JValue& out, std::string& err) {
    skipWs();
    if (!parseValue(out, err)) return false;
    skipWs();
    if (p_ != end_) {
      err = "trailing characters after JSON value";
      return false;
    }
    return true;
  }

 private:
  const char* p_;
  const char* end_;

  bool eof() const { return p_ >= end_; }
  char peek() const { return p_ < end_ ? *p_ : '\0'; }

  void skipWs() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool lit(const char* text) {
    size_t n = std::strlen(text);
    if (static_cast<size_t>(end_ - p_) < n) return false;
    if (std::memcmp(p_, text, n) != 0) return false;
    p_ += n;
    return true;
  }

  bool parseValue(JValue& out, std::string& err) {
    if (eof()) { err = "unexpected end of input"; return false; }
    char c = peek();
    switch (c) {
      case '{': return parseObject(out, err);
      case '[': return parseArray(out, err);
      case '"': {
        std::string s;
        if (!parseString(s, err)) return false;
        out = JValue::makeString(std::move(s));
        return true;
      }
      case 't':
        if (!lit("true")) { err = "invalid literal"; return false; }
        out = JValue::makeBool(true);
        return true;
      case 'f':
        if (!lit("false")) { err = "invalid literal"; return false; }
        out = JValue::makeBool(false);
        return true;
      case 'n':
        if (!lit("null")) { err = "invalid literal"; return false; }
        out = JValue::makeNull();
        return true;
      default:
        return parseNumber(out, err);
    }
  }

  bool parseObject(JValue& out, std::string& err) {
    ++p_;  // '{'
    JObject members;
    skipWs();
    if (peek() == '}') { ++p_; out = JValue::makeObject(std::move(members)); return true; }
    while (true) {
      skipWs();
      if (peek() != '"') { err = "expected object key"; return false; }
      std::string key;
      if (!parseString(key, err)) return false;
      for (const auto& kv : members) {
        if (kv.first == key) { err = "duplicate object key: " + key; return false; }
      }
      skipWs();
      if (peek() != ':') { err = "expected ':'"; return false; }
      ++p_;
      skipWs();
      JValue value;
      if (!parseValue(value, err)) return false;
      members.emplace_back(std::move(key), std::move(value));
      skipWs();
      if (peek() == ',') { ++p_; continue; }
      if (peek() == '}') { ++p_; break; }
      err = "expected ',' or '}'";
      return false;
    }
    out = JValue::makeObject(std::move(members));
    return true;
  }

  bool parseArray(JValue& out, std::string& err) {
    ++p_;  // '['
    JArray items;
    skipWs();
    if (peek() == ']') { ++p_; out = JValue::makeArray(std::move(items)); return true; }
    while (true) {
      skipWs();
      JValue value;
      if (!parseValue(value, err)) return false;
      items.push_back(std::move(value));
      skipWs();
      if (peek() == ',') { ++p_; continue; }
      if (peek() == ']') { ++p_; break; }
      err = "expected ',' or ']'";
      return false;
    }
    out = JValue::makeArray(std::move(items));
    return true;
  }

  static void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
      s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(uint32_t& value) {
    if (end_ - p_ < 4) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = p_[i];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    p_ += 4;
    return true;
  }

  bool parseString(std::string& out, std::string& err) {
    ++p_;  // opening quote
    std::string s;
    while (true) {
      if (eof()) { err = "unterminated string"; return false; }
      char c = *p_++;
      if (c == '"') break;
      if (c != '\\') {
        if (static_cast<unsigned char>(c) < 0x20) { err = "control character in string"; return false; }
        s.push_back(c);
        continue;
      }
      if (eof()) { err = "unterminated escape"; return false; }
      char e = *p_++;
      switch (e) {
        case '"': s.push_back('"'); break;
        case '\\': s.push_back('\\'); break;
        case '/': s.push_back('/'); break;
        case 'b': s.push_back('\b'); break;
        case 'f': s.push_back('\f'); break;
        case 'n': s.push_back('\n'); break;
        case 'r': s.push_back('\r'); break;
        case 't': s.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) { err = "invalid \\u escape"; return false; }
          if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            const char* save = p_;
            p_ += 2;
            uint32_t low = 0;
            if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
              p_ = save;
            }
          }
          appendUtf8(s, cp);
          break;
        }
        default: err = "invalid escape character"; return false;
      }
    }
    out = std::move(s);
    return true;
  }

  bool parseNumber(JValue& out, std::string& err) {
    const char* start = p_;
    if (peek() == '-') ++p_;
    if (eof() || !std::isdigit(static_cast<unsigned char>(peek()))) {
      err = "invalid number";
      return false;
    }
    while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
    bool isInt = true;
    if (peek() == '.') {
      isInt = false;
      ++p_;
      while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
    }
    if (peek() == 'e' || peek() == 'E') {
      isInt = false;
      ++p_;
      if (peek() == '+' || peek() == '-') ++p_;
      while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
    }
    std::string text(start, p_);
    if (isInt) {
      errno = 0;
      char* endp = nullptr;
      long long v = std::strtoll(text.c_str(), &endp, 10);
      if (errno != ERANGE) {
        out = JValue::makeInt(static_cast<int64_t>(v));
        return true;
      }
    }
    errno = 0;
    double d = std::strtod(text.c_str(), nullptr);
    if (!std::isfinite(d)) { err = "non finite number"; return false; }
    out = JValue::makeDouble(d);
    return true;
  }
};

void dumpString(const std::string& s, std::string& out) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void dumpValue(const JValue& v, std::string& out) {
  switch (v.type) {
    case JType::Null: out += "null"; break;
    case JType::Bool: out += v.b ? "true" : "false"; break;
    case JType::Int: out += std::to_string(v.i); break;
    case JType::Double: out += formatDouble(v.d); break;
    case JType::String: dumpString(v.s, out); break;
    case JType::Array: {
      out.push_back('[');
      for (size_t i = 0; i < v.arr.size(); ++i) {
        if (i) out.push_back(',');
        dumpValue(v.arr[i], out);
      }
      out.push_back(']');
      break;
    }
    case JType::Object: {
      out.push_back('{');
      for (size_t i = 0; i < v.obj.size(); ++i) {
        if (i) out.push_back(',');
        dumpString(v.obj[i].first, out);
        out.push_back(':');
        dumpValue(v.obj[i].second, out);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

bool jsonParse(const std::string& text, JValue& out, std::string& err) {
  Parser parser(text.data(), text.data() + text.size());
  return parser.parse(out, err);
}

std::string jsonDump(const JValue& v) {
  std::string out;
  dumpValue(v, out);
  return out;
}

}  // namespace mvdb
