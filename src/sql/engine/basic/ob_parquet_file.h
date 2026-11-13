/*
 * Copyright (c) 2025 OceanBase.
 * Minimal parquet reader for the "file as a database" table functions.
 * Supports Arrow2-Rust style files: thrift-compact footer, DataPageV2 (and
 * plain DataPageV1) pages, PLAIN encoding, UNCOMPRESSED or SNAPPY codec,
 * flat schemas with optional (bit width 1) definition levels.
 */

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_FILE_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_FILE_H_

#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace sql
{

// thrift compact protocol reader with per-struct field-id scoping.
struct ObPQCompact
{
  const char *b_;
  int64_t n_;
  int64_t i_;
  int32_t last_;
  int err_;
  ObPQCompact(const char *b, int64_t n)
      : b_(b), n_(n), i_(0), last_(0), err_(common::OB_SUCCESS) {}

  uint8_t u8()
  {
    if (i_ >= n_) {
      err_ = common::OB_INVALID_DATA;
      return 0;
    }
    return static_cast<uint8_t>(b_[i_++]);
  }
  uint64_t varint()
  {
    uint64_t v = 0;
    int s = 0;
    for (;;) {
      uint8_t x = u8();
      v |= static_cast<uint64_t>(x & 0x7F) << s;
      if (0 == (x & 0x80)) {
        break;
      }
      s += 7;
    }
    return v;
  }
  int64_t zig()
  {
    uint64_t u = varint();
    return static_cast<int64_t>(u >> 1) ^ -(static_cast<int64_t>(u & 1));
  }
  bool field(int16_t &fid, uint8_t &type)
  {
    uint8_t x = u8();
    if (0 == x) {
      return false;
    }
    type = x & 0x0F;
    int d = x >> 4;
    if (0 == d) {
      last_ = static_cast<int32_t>(zig());
    } else {
      last_ += d;
    }
    fid = static_cast<int16_t>(last_);
    return true;
  }
  void skip(uint8_t type)
  {
    switch (type) {
      case 1:
      case 2:
        break;
      case 3:
        u8();
        break;
      case 4:
      case 5:
      case 6:
        (void)zig();
        break;
      case 7:
        i_ += 8;
        break;
      case 8: {
        uint64_t l = varint();
        i_ += static_cast<int64_t>(l);
        break;
      }
      case 9:
      case 10: {
        uint8_t h = u8();
        uint64_t sz = h >> 4;
        uint8_t et = h & 0x0F;
        if (15 == sz) {
          sz = varint();
        }
        for (uint64_t k = 0; k < sz; ++k) {
          skip(et);
        }
        break;
      }
      case 11: {
        uint64_t sz = varint();
        if (sz > 0) {
          uint8_t h = u8();
          uint8_t kt = h >> 4;
          uint8_t vt = h & 0x0F;
          for (uint64_t k = 0; k < sz; ++k) {
            skip(kt);
            skip(vt);
          }
        }
        break;
      }
      case 12: {
        int32_t saved = last_;
        last_ = 0;
        int16_t f = 0;
        uint8_t t = 0;
        while (field(f, t)) {
          skip(t);
        }
        last_ = saved;
        break;
      }
      default:
        err_ = common::OB_INVALID_DATA;
        break;
    }
  }
  common::ObString binary()
  {
    uint64_t l = varint();
    if (i_ + static_cast<int64_t>(l) > n_) {
      err_ = common::OB_INVALID_DATA;
      return common::ObString();
    }
    common::ObString s(static_cast<int32_t>(l), b_ + i_);
    i_ += static_cast<int64_t>(l);
    return s;
  }
};

// snappy block decompressor.
inline int snappy_decompress(const char *src, int64_t src_len, char *dst, int64_t dst_len)
{
  int ret = common::OB_SUCCESS;
  int64_t i = 0;
  int64_t o = 0;
  uint64_t ulen = 0;
  int sh = 0;
  bool have_len = false;
  while (i < src_len) {
    uint8_t x = static_cast<uint8_t>(src[i++]);
    ulen |= static_cast<uint64_t>(x & 0x7F) << sh;
    if (0 == (x & 0x80)) {
      have_len = true;
      break;
    }
    sh += 7;
  }
  if (!have_len || ulen != static_cast<uint64_t>(dst_len)) {
    ret = common::OB_INVALID_DATA;
  }
  while ((common::OB_SUCCESS == ret) && i < src_len) {
    uint8_t tag = static_cast<uint8_t>(src[i++]);
    switch (tag & 3) {
      case 0: {  // literal
        uint64_t len = 0;
        uint8_t h = tag >> 2;
        if (h < 60) {
          len = h + 1;
        } else {
          int nb = h - 59;
          len = 0;
          for (int k = 0; k < nb; ++k) {
            if (i >= src_len) {
              ret = common::OB_INVALID_DATA;
              break;
            }
            len |= static_cast<uint64_t>(static_cast<uint8_t>(src[i++])) << (8 * k);
          }
          ++len;
        }
        if ((common::OB_SUCCESS == ret)) {
          if (i + static_cast<int64_t>(len) > src_len
              || o + static_cast<int64_t>(len) > dst_len) {
            ret = common::OB_INVALID_DATA;
          } else {
            MEMCPY(dst + o, src + i, len);
            i += static_cast<int64_t>(len);
            o += static_cast<int64_t>(len);
          }
        }
        break;
      }
      case 1: {  // copy with 1-byte offset
        if (i >= src_len) {
          ret = common::OB_INVALID_DATA;
          break;
        }
        uint64_t len = 4 + ((tag >> 2) & 0x7);
        uint64_t off = (static_cast<uint64_t>(tag & 0xE0) << 3)
                       | static_cast<uint8_t>(src[i++]);
        if (0 == off || off > static_cast<uint64_t>(o)
            || o + static_cast<int64_t>(len) > dst_len) {
          ret = common::OB_INVALID_DATA;
        } else {
          const char *p = dst + o - static_cast<int64_t>(off);
          for (uint64_t k = 0; k < len; ++k) {
            dst[o + k] = p[k];
          }
          o += static_cast<int64_t>(len);
        }
        break;
      }
      case 2: {  // copy with 2-byte offset
        if (i + 1 >= src_len) {
          ret = common::OB_INVALID_DATA;
          break;
        }
        uint64_t len = 1 + (tag >> 2);
        uint64_t off = static_cast<uint8_t>(src[i])
                       | (static_cast<uint64_t>(static_cast<uint8_t>(src[i + 1])) << 8);
        i += 2;
        if (0 == off || off > static_cast<uint64_t>(o)
            || o + static_cast<int64_t>(len) > dst_len) {
          ret = common::OB_INVALID_DATA;
        } else {
          const char *p = dst + o - static_cast<int64_t>(off);
          for (uint64_t k = 0; k < len; ++k) {
            dst[o + k] = p[k];
          }
          o += static_cast<int64_t>(len);
        }
        break;
      }
      default: {  // copy with 4-byte offset
        if (i + 3 >= src_len) {
          ret = common::OB_INVALID_DATA;
          break;
        }
        uint64_t len = 1 + (tag >> 2);
        uint64_t off = static_cast<uint8_t>(src[i])
                       | (static_cast<uint64_t>(static_cast<uint8_t>(src[i + 1])) << 8)
                       | (static_cast<uint64_t>(static_cast<uint8_t>(src[i + 2])) << 16)
                       | (static_cast<uint64_t>(static_cast<uint8_t>(src[i + 3])) << 24);
        i += 4;
        if (0 == off || off > static_cast<uint64_t>(o)
            || o + static_cast<int64_t>(len) > dst_len) {
          ret = common::OB_INVALID_DATA;
        } else {
          const char *p = dst + o - static_cast<int64_t>(off);
          for (uint64_t k = 0; k < len; ++k) {
            dst[o + k] = p[k];
          }
          o += static_cast<int64_t>(len);
        }
        break;
      }
    }
  }
  if ((common::OB_SUCCESS == ret) && o != dst_len) {
    ret = common::OB_INVALID_DATA;
  }
  return ret;
}

struct ObPQColMeta
{
  common::ObString name_;
  int32_t type_;   // 0 bool, 1 int32, 2 int64, 4 float, 5 double, 6 byte_array, 7 fixed
  int32_t rep_;    // 0 required, 1 optional
  int32_t codec_;  // 0 uncompressed, 1 snappy
  int64_t num_values_;
  int64_t data_page_offset_;
  ObPQColMeta() : name_(), type_(0), rep_(0), codec_(0), num_values_(0), data_page_offset_(0) {}
};

// per-column page cursor state (owned by the executor op)
struct ObPQPageState
{
  int64_t next_page_off_;   // file offset right after current page's data; 0 = not loaded
  int64_t rows_in_page_;
  int64_t rows_served_;
  int64_t nonnull_served_;  // consumed non-null PLAIN values (fixed width columns)
  int64_t val_pos_;         // cursor for byte_array columns
  int32_t def_len_;
  int32_t rep_len_;
  const char *def_buf_;     // points into the file buffer (levels are uncompressed in v2)
  char *val_buf_;           // decompressed values (or points into file when uncompressed)
  int64_t val_len_;
  int32_t width_;           // 0 => byte_array / bool; else fixed bytes per value
  ObPQPageState() { MEMSET(this, 0, sizeof(*this)); }
};

// def levels use RLE/bit-packed hybrid encoding. For flat schemas with max
// definition level 1 (OPTIONAL columns) the bit width is 1. Returns true when
// the level at index idx denotes a NULL.
inline bool pq_def_level_is_null(const char *buf, int64_t len, int64_t idx)
{
  bool is_null = true;
  int64_t pos = 0;
  uint64_t h = 0;
  int sh = 0;
  while (pos < len) {
    uint8_t x = static_cast<uint8_t>(buf[pos++]);
    h |= static_cast<uint64_t>(x & 0x7F) << sh;
    if (0 == (x & 0x80)) {
      break;
    }
    sh += 7;
  }
  if (h & 1) {  // bit-packed run
    const uint64_t groups = h >> 1;
    const int64_t gi = idx >> 3;
    if (gi < static_cast<int64_t>(groups) && pos + gi < len) {
      is_null = (0 == ((static_cast<uint8_t>(buf[pos + gi]) >> (idx & 7)) & 1));
    }
  } else {  // RLE run
    uint64_t v = 0;
    sh = 0;
    while (pos < len) {
      uint8_t x = static_cast<uint8_t>(buf[pos++]);
      v |= static_cast<uint64_t>(x & 0x7F) << sh;
      if (0 == (x & 0x80)) {
        break;
      }
      sh += 7;
    }
    is_null = (0 == v);
  }
  return is_null;
}

class ObParquetFile
{
public:
  static const int64_t MAX_COLS = 16;
  ObParquetFile() : buf_(NULL), len_(0), num_rows_(0), col_count_(0) {}
  int parse_footer(const char *buf, int64_t len);
  int read_page(int64_t col_idx, common::ObIAllocator &alloc, ObPQPageState &st) const;
  int64_t num_rows() const { return num_rows_; }
  int32_t col_count() const { return col_count_; }
  const ObPQColMeta &col(int64_t i) const { return cols_[i]; }

private:
  const char *buf_;
  int64_t len_;
  int64_t num_rows_;
  int32_t col_count_;
  ObPQColMeta cols_[MAX_COLS];
};

inline int ObParquetFile::parse_footer(const char *buf, int64_t len)
{
  int ret = common::OB_SUCCESS;
  buf_ = buf;
  len_ = len;
  num_rows_ = 0;
  col_count_ = 0;
  if (len < 12 || 0 != MEMCMP(buf, "PAR1", 4)) {
    ret = common::OB_INVALID_DATA;
  } else {
    uint32_t flen = 0;
    MEMCPY(&flen, buf + len - 8, 4);
    if (flen <= 0 || flen > static_cast<uint32_t>(len) - 12) {
      ret = common::OB_INVALID_DATA;
    } else {
      ObPQCompact r(buf + len - 8 - flen, flen);
      int16_t fid = 0;
      uint8_t type = 0;
      while ((common::OB_SUCCESS == ret) && r.field(fid, type)) {
        if (1 == fid) {
          (void)r.zig();  // version
        } else if (2 == fid) {
          // schema: list<struct SchemaElement>
          uint8_t h = r.u8();
          uint64_t sz = h >> 4;
          uint8_t et = h & 0x0F;
          if (15 == sz) {
            sz = r.varint();
          }
          for (uint64_t k = 0; (common::OB_SUCCESS == ret) && k < sz; ++k) {
            int32_t saved = r.last_;
            r.last_ = 0;
            ObPQColMeta m;
            int32_t num_children = -1;
            int16_t f2 = 0;
            uint8_t t2 = 0;
            while (r.field(f2, t2)) {
              if (1 == f2) {
                m.type_ = static_cast<int32_t>(r.zig());
              } else if (2 == f2) {
                (void)r.zig();
              } else if (3 == f2) {
                m.rep_ = static_cast<int32_t>(r.zig());
              } else if (4 == f2) {
                m.name_ = r.binary();
              } else if (5 == f2) {
                num_children = static_cast<int32_t>(r.zig());
              } else {
                r.skip(t2);
              }
            }
            r.last_ = saved;
            if ((common::OB_SUCCESS == ret) && num_children <= 0 && col_count_ < MAX_COLS) {
              cols_[col_count_++] = m;
            }
          }
        } else if (3 == fid) {
          num_rows_ = r.zig();
        } else if (4 == fid) {
          // row_groups: list<struct RowGroup>; only the first is consumed.
          uint8_t h = r.u8();
          uint64_t sz = h >> 4;
          uint8_t et = h & 0x0F;
          if (15 == sz) {
            sz = r.varint();
          }
          for (uint64_t k = 0; (common::OB_SUCCESS == ret) && k < sz; ++k) {
            int32_t saved = r.last_;
            r.last_ = 0;
            int16_t f2 = 0;
            uint8_t t2 = 0;
            while (r.field(f2, t2)) {
              if (1 == f2) {
                // columns: list<struct ColumnChunk>
                uint8_t h2 = r.u8();
                uint64_t sz2 = h2 >> 4;
                uint8_t et2 = h2 & 0x0F;
                if (15 == sz2) {
                  sz2 = r.varint();
                }
                for (uint64_t k2 = 0; (common::OB_SUCCESS == ret) && k2 < sz2; ++k2) {
                  int32_t saved2 = r.last_;
                  r.last_ = 0;
                  int16_t f3 = 0;
                  uint8_t t3 = 0;
                  while (r.field(f3, t3)) {
                    if (3 == f3 && k2 < static_cast<uint64_t>(col_count_)) {
                      // meta_data struct
                      int32_t saved3 = r.last_;
                      r.last_ = 0;
                      ObPQColMeta &m = cols_[k2];
                      int16_t f4 = 0;
                      uint8_t t4 = 0;
                      while (r.field(f4, t4)) {
                        if (1 == f4) {
                          m.type_ = static_cast<int32_t>(r.zig());
                        } else if (2 == f4) {
                          uint8_t h3 = r.u8();
                          uint64_t sz3 = h3 >> 4;
                          uint8_t et3 = h3 & 0x0F;
                          if (15 == sz3) {
                            sz3 = r.varint();
                          }
                          for (uint64_t k3 = 0; k3 < sz3; ++k3) {
                            (void)r.zig();
                          }
                        } else if (3 == f4) {
                          uint8_t h3 = r.u8();
                          uint64_t sz3 = h3 >> 4;
                          uint8_t et3 = h3 & 0x0F;
                          if (15 == sz3) {
                            sz3 = r.varint();
                          }
                          for (uint64_t k3 = 0; k3 < sz3; ++k3) {
                            (void)r.binary();
                          }
                        } else if (4 == f4) {
                          m.codec_ = static_cast<int32_t>(r.zig());
                        } else if (5 == f4) {
                          m.num_values_ = r.zig();
                        } else if (9 == f4) {
                          m.data_page_offset_ = r.zig();
                        } else {
                          r.skip(t4);
                        }
                      }
                      r.last_ = saved3;
                    } else {
                      r.skip(t3);
                    }
                  }
                  r.last_ = saved2;
                }
              } else {
                r.skip(t2);
              }
            }
            r.last_ = saved;
            break;  // first row group only
          }
        } else {
          r.skip(type);
        }
      }
      if ((common::OB_SUCCESS == ret)) {
        ret = r.err_;
      }
    }
  }
  return ret;
}

inline int ObParquetFile::read_page(int64_t col_idx, common::ObIAllocator &alloc,
                                    ObPQPageState &st) const
{
  int ret = common::OB_SUCCESS;
  if (col_idx < 0 || col_idx >= col_count_) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    const ObPQColMeta &m = cols_[col_idx];
    const int64_t off = (st.next_page_off_ > 0) ? st.next_page_off_ : m.data_page_offset_;
    ObPQCompact r(buf_ + off, len_ - off);
    int32_t page_type = -1;
    int32_t uncompressed_size = 0;
    int32_t compressed_size = 0;
    int32_t num_values = 0;
    int32_t num_rows = 0;
    int32_t encoding = -1;
    int32_t def_len = 0;
    int32_t rep_len = 0;
    bool is_compressed = false;
    int16_t fid = 0;
    uint8_t type = 0;
    while ((common::OB_SUCCESS == ret) && r.field(fid, type)) {
      if (1 == fid) {
        page_type = static_cast<int32_t>(r.zig());
      } else if (2 == fid) {
        uncompressed_size = static_cast<int32_t>(r.zig());
      } else if (3 == fid) {
        compressed_size = static_cast<int32_t>(r.zig());
      } else if (8 == fid) {
        // DataPageHeaderV2
        int32_t saved = r.last_;
        r.last_ = 0;
        int16_t f2 = 0;
        uint8_t t2 = 0;
        while (r.field(f2, t2)) {
          if (1 == f2) {
            num_values = static_cast<int32_t>(r.zig());
          } else if (2 == f2) {
            (void)r.zig();  // num_nulls
          } else if (3 == f2) {
            num_rows = static_cast<int32_t>(r.zig());
          } else if (4 == f2) {
            encoding = static_cast<int32_t>(r.zig());
          } else if (5 == f2) {
            def_len = static_cast<int32_t>(r.zig());
          } else if (6 == f2) {
            rep_len = static_cast<int32_t>(r.zig());
          } else if (7 == f2) {
            is_compressed = true;
          } else {
            r.skip(t2);
          }
        }
        r.last_ = saved;
      } else if (5 == fid) {
        // DataPageHeader (v1): whole page may be compressed; levels inside.
        int32_t saved = r.last_;
        r.last_ = 0;
        int16_t f2 = 0;
        uint8_t t2 = 0;
        while (r.field(f2, t2)) {
          if (1 == f2) {
            num_values = static_cast<int32_t>(r.zig());
          } else if (2 == f2) {
            encoding = static_cast<int32_t>(r.zig());
          } else if (3 == f2) {
            def_len = static_cast<int32_t>(r.zig());
          } else if (4 == f2) {
            rep_len = static_cast<int32_t>(r.zig());
          } else {
            r.skip(t2);
          }
        }
        r.last_ = saved;
        num_rows = num_values;
      } else {
        r.skip(type);
      }
    }
    if ((common::OB_SUCCESS == ret)) {
      ret = r.err_;
    }
    if ((common::OB_SUCCESS == ret)) {
      const int64_t data_off = off + r.i_;
      const int64_t val_raw_len = static_cast<int64_t>(uncompressed_size) - rep_len - def_len;
      const int64_t val_comp_len = static_cast<int64_t>(compressed_size) - rep_len - def_len;
      if (data_off + compressed_size > len_ || val_raw_len < 0 || val_comp_len < 0) {
        ret = common::OB_INVALID_DATA;
      } else {
        st.rows_in_page_ = num_rows;
        st.rows_served_ = 0;
        st.nonnull_served_ = 0;
        st.val_pos_ = 0;
        st.def_len_ = def_len;
        st.rep_len_ = rep_len;
        st.def_buf_ = buf_ + data_off + rep_len;
        st.next_page_off_ = data_off + compressed_size;
        const char *val_src = buf_ + data_off + rep_len + def_len;
        if (0 == m.codec_) {
          st.val_buf_ = const_cast<char *>(val_src);
          st.val_len_ = val_raw_len;
        } else if (1 == m.codec_) {
          if (!is_compressed) {
            st.val_buf_ = const_cast<char *>(val_src);
            st.val_len_ = val_raw_len;
          } else {
            char *out = static_cast<char *>(alloc.alloc(val_raw_len > 0 ? val_raw_len : 1));
            if (OB_ISNULL(out)) {
              ret = common::OB_ALLOCATE_MEMORY_FAILED;
            } else if (common::OB_SUCCESS != snappy_decompress(val_src, val_comp_len, out, val_raw_len)) {
            } else {
              st.val_buf_ = out;
              st.val_len_ = val_raw_len;
            }
          }
        } else {
          ret = common::OB_NOT_SUPPORTED;
        }
        if ((common::OB_SUCCESS == ret)) {
          switch (m.type_) {
            case 1:
              st.width_ = 4;
              break;
            case 2:
              st.width_ = 8;
              break;
            case 4:
              st.width_ = 4;
              break;
            case 5:
              st.width_ = 8;
              break;
            default:
              st.width_ = 0;  // bool (1 byte) / byte_array / fixed
              break;
          }
        }
      }
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase

#endif /* OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_FILE_H_ */
