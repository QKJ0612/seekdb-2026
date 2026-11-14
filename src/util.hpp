// util.hpp - small helpers shared by the MiniVectorDB sources.
//
// The project must not depend on any third party library, so everything here
// is hand written: CRC32 (used by the log framing), UTF-8 helpers and a
// JSON-safe floating point formatter.
#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mvdb {

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3 / zlib polynomial 0xEDB88320).
// ---------------------------------------------------------------------------
namespace detail {
inline const uint32_t* crc32Table() {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  return table;
}
}  // namespace detail

// Chained variant: crc32Update(crc32Update(0, a, na), b, nb) == crc32(a+b).
inline uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
  const uint32_t* table = detail::crc32Table();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

inline uint32_t crc32(const void* data, size_t len) { return crc32Update(0, data, len); }

// ---------------------------------------------------------------------------
// Text helpers.
// ---------------------------------------------------------------------------

// Number of Unicode code points in a UTF-8 string (continuation bytes are not
// counted).  VARCHAR(n) counts characters, not bytes.
inline size_t utf8CharCount(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0u) != 0x80u) ++n;
  return n;
}

inline std::string toUpper(const std::string& s) {
  std::string r = s;
  for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

inline std::string toLower(const std::string& s) {
  std::string r = s;
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

inline bool equalsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// Format a double so that it is always a valid JSON number and always looks
// like a floating point value ("5" becomes "5.0").  Non finite values are
// clamped: the protocol forbids NaN/Infinity on stdout.
inline std::string formatDouble(double v) {
  if (!std::isfinite(v)) return "0.0";
  char buf[64];
  for (int prec = 15; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  std::string s(buf);
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos && s.find("inf") == std::string::npos &&
      s.find("nan") == std::string::npos) {
    s += ".0";
  }
  return s;
}

// ---------------------------------------------------------------------------
// Filesystem helpers (POSIX, the target platform is Linux).
// ---------------------------------------------------------------------------
inline bool pathExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

inline bool isDirectory(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  return S_ISDIR(st.st_mode);
}

// A directory is usable when it exists and a probe file can be created in it.
inline bool isWritableDirectory(const std::string& path) {
  if (!isDirectory(path)) return false;
  std::string probe = path + "/.mvdb_probe";
  int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  ::close(fd);
  ::unlink(probe.c_str());
  return true;
}

}  // namespace mvdb
