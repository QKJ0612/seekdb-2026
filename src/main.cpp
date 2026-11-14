// main.cpp - the MiniVectorDB server.
//
// Protocol (JSON Lines over stdin/stdout):
//   * the server prints {"type":"ready"} and flushes before anything else
//   * every request is one JSON object on one line
//   * every request is answered with exactly one JSON object on one line
//   * stdout carries protocol lines only, diagnostics go to stderr
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "db.hpp"
#include "json.hpp"
#include "util.hpp"

namespace mvdb {
namespace {

// The judge never accepts a response larger than 16 MiB.
constexpr size_t kMaxResponseBytes = 16u * 1024u * 1024u;

// Chooses the directory that holds the database files.
//   1. VECTORDB_DATA_DIR when it exists and is writable (the judge injects
//      a host path that does not exist inside the container)
//   2. argv[1] (used by the local harness)
//   3. /data (the volume mounted by the judge)
std::string resolveDataDir(int argc, char** argv) {
  const char* env = std::getenv("VECTORDB_DATA_DIR");
  if (env != nullptr && *env != '\0') {
    const std::string dir(env);
    if (isDirectory(dir) && isWritableDirectory(dir)) return dir;
  }
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    const std::string dir(argv[1]);
    if (isDirectory(dir) && isWritableDirectory(dir)) return dir;
  }
  return "/data";
}

void writeLine(const std::string& text) {
  if (std::fwrite(text.data(), 1, text.size(), stdout) != text.size()) {
    std::fflush(stdout);
    return;
  }
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// Builds {"id":<id>,"ok":false,"error":{"code":..,"message":..}}.
std::string errorResponse(const JValue& id, const std::string& code, const std::string& message) {
  JObject error;
  error.emplace_back("code", JValue::makeString(code));
  error.emplace_back("message", JValue::makeString(message));
  JObject obj;
  obj.emplace_back("id", id);
  obj.emplace_back("ok", JValue::makeBool(false));
  obj.emplace_back("error", JValue::makeObject(std::move(error)));
  return jsonDump(JValue::makeObject(std::move(obj)));
}

void handleRequest(const std::string& line, Database& db) {
  JValue request;
  std::string parseError;
  if (!jsonParse(line, request, parseError) || !request.isObject()) {
    writeLine(errorResponse(JValue::makeNull(), "invalid_request",
                             "malformed request: " +
                                 (parseError.empty() ? std::string("expected a JSON object") : parseError)));
    return;
  }

  const JValue* idValue = request.find("id");
  const JValue id = idValue != nullptr ? *idValue : JValue::makeNull();

  const JValue* opValue = request.find("op");
  const std::string op = (opValue != nullptr && opValue->isString()) ? opValue->asString() : "execute";

  if (op == "shutdown") {
    JObject obj;
    obj.emplace_back("id", id);
    obj.emplace_back("ok", JValue::makeBool(true));
    obj.emplace_back("type", JValue::makeString("result"));
    writeLine(jsonDump(JValue::makeObject(std::move(obj))));
    std::fflush(stdout);
    std::exit(0);
  }

  if (op != "execute") {
    writeLine(errorResponse(id, "invalid_request", "unsupported operation: " + op));
    return;
  }

  const JValue* sqlValue = request.find("sql");
  if (sqlValue == nullptr || !sqlValue->isString()) {
    writeLine(errorResponse(id, "invalid_request", "the request has no sql field"));
    return;
  }

  // "exact" is the default; only "ann" switches the vector search to the
  // approximate index.
  bool annMode = false;
  const JValue* modeValue = request.find("search_mode");
  if (modeValue != nullptr && modeValue->isString()) {
    const std::string mode = toLower(modeValue->asString());
    annMode = (mode == "ann");
  }

  const auto started = std::chrono::steady_clock::now();
  ExecResult result;
  try {
    result = db.execute(sqlValue->asString(), annMode);
  } catch (const std::exception& e) {
    writeLine(errorResponse(id, "internal_error", e.what()));
    return;
  }
  const auto finished = std::chrono::steady_clock::now();
  const double elapsedMs =
      std::chrono::duration<double, std::milli>(finished - started).count();

  JObject obj;
  obj.emplace_back("id", id);
  obj.emplace_back("ok", JValue::makeBool(result.ok));
  if (!result.ok) {
    JObject error;
    error.emplace_back("code",
                       JValue::makeString(result.errorCode.empty() ? "internal_error" : result.errorCode));
    error.emplace_back("message", JValue::makeString(result.errorMessage));
    obj.emplace_back("error", JValue::makeObject(std::move(error)));
  } else {
    JArray columns;
    columns.reserve(result.columns.size());
    for (const auto& column : result.columns) columns.push_back(JValue::makeString(column));
    obj.emplace_back("columns", JValue::makeArray(std::move(columns)));
    obj.emplace_back("rows", JValue::makeArray(std::move(result.rows)));
    obj.emplace_back("affected_rows", JValue::makeInt(result.affectedRows));
    obj.emplace_back("message", JValue::makeString("OK"));
    obj.emplace_back("elapsed_ms", JValue::makeDouble(elapsedMs));
  }

  const std::string response = jsonDump(JValue::makeObject(std::move(obj)));
  if (response.size() > kMaxResponseBytes) {
    writeLine(errorResponse(id, "response_too_large", "the result does not fit into 16 MiB"));
    return;
  }
  writeLine(response);
}

}  // namespace
}  // namespace mvdb

int main(int argc, char** argv) {
  using namespace mvdb;

  // Small command line helpers, they never write to stdout in protocol mode.
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i] != nullptr ? argv[i] : "");
    if (arg == "--version" || arg == "-v") {
      std::printf("minivectordb 1.0 - MiniVectorDB, JSON lines protocol over stdio\n");
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: vectordb [DATA_DIR]\n"
          "  The data directory is taken from VECTORDB_DATA_DIR when that directory\n"
          "  exists and is writable, otherwise from the argument, otherwise /data.\n");
      return 0;
    }
  }

  const std::string dataDir = resolveDataDir(argc, argv);
  std::unique_ptr<Database> db;
  try {
    db = std::make_unique<Database>(dataDir);
    db->open();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "mvdb: startup failed: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr, "mvdb: ready, data directory: %s\n", dataDir.c_str());

  // The judge waits for this line before it sends anything.
  writeLine("{\"type\":\"ready\"}");

  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;  // ignore blank lines
    handleRequest(line, *db);
  }
  return 0;
}
