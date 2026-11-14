// json.hpp - a tiny hand written JSON reader/writer.
//
// The judge speaks JSON Lines, so the server needs a parser for the request
// objects and a serializer for the responses.  Objects are stored as an
// ordered vector of key/value pairs, which keeps the field order of a
// response stable and makes duplicate keys impossible to emit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mvdb {

enum class JType { Null, Bool, Int, Double, String, Array, Object };

struct JValue;
using JArray = std::vector<JValue>;
using JObject = std::vector<std::pair<std::string, JValue>>;

struct JValue {
  JType type = JType::Null;
  bool b = false;
  int64_t i = 0;
  double d = 0.0;
  std::string s;
  JArray arr;
  JObject obj;

  JValue() = default;

  static JValue makeNull() { return JValue(); }
  static JValue makeBool(bool v) { JValue x; x.type = JType::Bool; x.b = v; return x; }
  static JValue makeInt(int64_t v) { JValue x; x.type = JType::Int; x.i = v; return x; }
  static JValue makeDouble(double v) { JValue x; x.type = JType::Double; x.d = v; return x; }
  static JValue makeString(std::string v) { JValue x; x.type = JType::String; x.s = std::move(v); return x; }
  static JValue makeArray(JArray v) { JValue x; x.type = JType::Array; x.arr = std::move(v); return x; }
  static JValue makeObject(JObject v) { JValue x; x.type = JType::Object; x.obj = std::move(v); return x; }

  // Looks up a member of an object (nullptr when absent).
  const JValue* find(const std::string& key) const {
    if (type != JType::Object) return nullptr;
    for (const auto& kv : obj)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  bool isString() const { return type == JType::String; }
  bool isObject() const { return type == JType::Object; }
  std::string asString() const { return type == JType::String ? s : std::string(); }
};

// Parses a complete JSON document.  Returns false and fills `err` on failure.
bool jsonParse(const std::string& text, JValue& out, std::string& err);

// Serializes a value into compact JSON (no spaces, no trailing newline).
std::string jsonDump(const JValue& v);

}  // namespace mvdb
