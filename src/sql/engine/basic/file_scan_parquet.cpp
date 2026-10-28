/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_parquet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oceanbase::sql::file_scan
{
namespace
{
// ---------------------------------------------------------------------------
// Thrift compact protocol
// ---------------------------------------------------------------------------
constexpr int T_STOP = 0x00;
constexpr int T_BOOLEAN_TRUE = 0x01;
constexpr int T_BOOLEAN_FALSE = 0x02;
constexpr int T_BYTE = 0x03;
constexpr int T_I16 = 0x04;
constexpr int T_I32 = 0x05;
constexpr int T_I64 = 0x06;
constexpr int T_DOUBLE = 0x07;
constexpr int T_BINARY = 0x08;
constexpr int T_LIST = 0x09;
constexpr int T_SET = 0x0A;
constexpr int T_MAP = 0x0B;
constexpr int T_STRUCT = 0x0C;

// Sequential reader over one Thrift compact buffer. The reader never throws;
// it reports truncation (the buffer was too small) separately from a malformed
// structure so that the caller can retry with a larger window.
class CompactReader
{
public:
  CompactReader(const uint8_t *data, uint64_t size) : data_(data), size_(size) {}
  bool truncated() const { return truncated_; }
  uint64_t position() const { return position_; }

  bool read_byte(uint8_t &value)
  {
    if (position_ >= size_) { truncated_ = true; return false; }
    value = data_[position_++];
    return true;
  }

  bool read_varint(uint64_t &value)
  {
    uint64_t result = 0;
    int shift = 0;
    for (;;) {
      if (position_ >= size_) { truncated_ = true; return false; }
      const uint8_t byte = data_[position_++];
      result |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) { break; }
      shift += 7;
      if (shift > 63) { truncated_ = true; return false; }
    }
    value = result;
    return true;
  }

  bool read_zigzag(int64_t &value)
  {
    uint64_t raw = 0;
    if (!read_varint(raw)) { return false; }
    value = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    return true;
  }

  bool read_i32(int32_t &value)
  {
    int64_t wide = 0;
    if (!read_zigzag(wide)) { return false; }
    value = static_cast<int32_t>(wide);
    return true;
  }

  bool read_i64(int64_t &value)
  {
    int64_t wide = 0;
    if (!read_zigzag(wide)) { return false; }
    value = wide;
    return true;
  }

  bool read_binary(std::string &value)
  {
    uint64_t length = 0;
    if (!read_varint(length)) { return false; }
    if (length > size_ - position_) { truncated_ = true; return false; }
    value.assign(reinterpret_cast<const char *>(data_ + position_), static_cast<size_t>(length));
    position_ += length;
    return true;
  }

  bool skip(uint64_t length)
  {
    if (length > size_ - position_) { truncated_ = true; return false; }
    position_ += length;
    return true;
  }

  bool read_list_header(uint32_t &count, int &element_type)
  {
    uint8_t header = 0;
    if (!read_byte(header)) { return false; }
    element_type = header & 0x0f;
    const uint32_t short_count = header >> 4;
    if (short_count == 0x0f) {
      uint64_t wide = 0;
      if (!read_varint(wide)) { return false; }
      if (wide > size_) { truncated_ = true; return false; }
      count = static_cast<uint32_t>(wide);
    } else {
      count = short_count;
    }
    return true;
  }

private:
  const uint8_t *data_ = nullptr;
  uint64_t size_ = 0;
  uint64_t position_ = 0;
  bool truncated_ = false;
};

// Reads the next struct field header. `stop` becomes true at the struct end.
bool read_field(CompactReader &reader, int16_t &last_field_id, int &type, bool &stop)
{
  uint8_t header = 0;
  if (!reader.read_byte(header)) { return false; }
  if (header == T_STOP) { stop = true; return true; }
  type = header & 0x0f;
  const int delta = header >> 4;
  int32_t field_id = 0;
  if (delta == 0) {
    int64_t zigzag = 0;
    if (!reader.read_zigzag(zigzag)) { return false; }
    field_id = static_cast<int32_t>(zigzag);
  } else {
    field_id = static_cast<int32_t>(last_field_id) + delta;
  }
  last_field_id = static_cast<int16_t>(field_id);
  return true;
}

bool skip_field(CompactReader &reader, int type)
{
  switch (type) {
    case T_BOOLEAN_TRUE:
    case T_BOOLEAN_FALSE: return true;
    case T_BYTE: {
      uint8_t value = 0;
      return reader.read_byte(value);
    }
    case T_I16:
    case T_I32:
    case T_I64: {
      int64_t value = 0;
      return reader.read_zigzag(value);
    }
    case T_DOUBLE: return reader.skip(8);
    case T_BINARY: {
      std::string value;
      return reader.read_binary(value);
    }
    case T_LIST:
    case T_SET: {
      uint32_t count = 0;
      int element_type = 0;
      if (!reader.read_list_header(count, element_type)) { return false; }
      for (uint32_t i = 0; i < count; ++i) {
        if (!skip_field(reader, element_type)) { return false; }
      }
      return true;
    }
    case T_MAP: {
      uint64_t count = 0;
      if (!reader.read_varint(count)) { return false; }
      if (count == 0) { return true; }
      uint8_t types = 0;
      if (!reader.read_byte(types)) { return false; }
      for (uint64_t i = 0; i < count; ++i) {
        if (!skip_field(reader, types >> 4)) { return false; }
        if (!skip_field(reader, types & 0x0f)) { return false; }
      }
      return true;
    }
    case T_STRUCT: {
      int16_t last = 0;
      int field_type = 0;
      bool stop = false;
      for (;;) {
        if (!read_field(reader, last, field_type, stop)) { return false; }
        if (stop) { return true; }
        if (!skip_field(reader, field_type)) { return false; }
      }
    }
    default: return false;
  }
}

// ---------------------------------------------------------------------------
// Parquet metadata structures
// ---------------------------------------------------------------------------
constexpr int PT_BOOLEAN = 0;
constexpr int PT_INT32 = 1;
constexpr int PT_INT64 = 2;
constexpr int PT_INT96 = 3;
constexpr int PT_FLOAT = 4;
constexpr int PT_DOUBLE = 5;
constexpr int PT_BYTE_ARRAY = 6;
constexpr int PT_FIXED_LEN_BYTE_ARRAY = 7;

constexpr int REPETITION_REQUIRED = 0;
constexpr int REPETITION_OPTIONAL = 1;
constexpr int REPETITION_REPEATED = 2;

constexpr int CONVERTED_UTF8 = 0;
constexpr int CONVERTED_DECIMAL = 5;
constexpr int CONVERTED_DATE = 6;
constexpr int CONVERTED_TIMESTAMP_MILLIS = 9;
constexpr int CONVERTED_TIMESTAMP_MICROS = 10;

constexpr int LOGICAL_DATE = 6;
constexpr int LOGICAL_TIMESTAMP = 8;
constexpr int LOGICAL_INTEGER = 10;

constexpr int ENCODING_PLAIN = 0;
constexpr int ENCODING_PLAIN_DICTIONARY = 2;
constexpr int ENCODING_RLE = 3;
constexpr int ENCODING_RLE_DICTIONARY = 8;

constexpr int CODEC_UNCOMPRESSED = 0;
constexpr int CODEC_SNAPPY = 1;

constexpr int PAGE_DATA = 0;
constexpr int PAGE_INDEX = 1;
constexpr int PAGE_DICTIONARY = 2;
constexpr int PAGE_DATA_V2 = 3;

struct SchemaElement
{
  int type = -1;
  int type_length = 0;
  int repetition_type = REPETITION_REQUIRED;
  std::string name;
  int num_children = -1;
  int converted_type = -1;
  int logical_type = -1;
  int logical_bit_width = 0;
  bool logical_is_signed = true;
  int logical_time_unit = 0; // 0 millis, 1 micros, 2 nanos
};

struct ColumnMeta
{
  int type = -1;
  std::vector<int> encodings;
  std::vector<std::string> path;
  int codec = CODEC_UNCOMPRESSED;
  int64_t num_values = 0;
  int64_t total_uncompressed_size = 0;
  int64_t total_compressed_size = 0;
  int64_t data_page_offset = 0;
  int64_t dictionary_page_offset = -1;
};

struct RowGroup
{
  std::vector<ColumnMeta> columns;
  int64_t num_rows = 0;
};

struct PageHeader
{
  int type = PAGE_DATA;
  int32_t uncompressed_page_size = 0;
  int32_t compressed_page_size = 0;
  // Data page (v1)
  int32_t num_values = 0;
  int encoding = ENCODING_PLAIN;
  int definition_level_encoding = ENCODING_RLE;
  int repetition_level_encoding = ENCODING_RLE;
  // Data page v2
  int32_t num_nulls = 0;
  int32_t num_rows = 0;
  int32_t definition_levels_byte_length = 0;
  int32_t repetition_levels_byte_length = 0;
  bool is_compressed = true;
  // Dictionary page
  int32_t dictionary_num_values = 0;
  int dictionary_encoding = ENCODING_PLAIN;
};

bool parse_logical_type(CompactReader &reader, SchemaElement &out)
{
  int16_t last = 0;
  int type = 0;
  bool stop = false;
  for (;;) {
    if (!read_field(reader, last, type, stop)) { return false; }
    if (stop) { break; }
    if (type != T_STRUCT) {
      if (!skip_field(reader, type)) { return false; }
      continue;
    }
    out.logical_type = last;
    if (last == LOGICAL_INTEGER) {
      int16_t inner = 0;
      int inner_type = 0;
      bool inner_stop = false;
      for (;;) {
        if (!read_field(reader, inner, inner_type, inner_stop)) { return false; }
        if (inner_stop) { break; }
        if (inner == 1 && inner_type == T_I32) {
          int32_t width = 0;
          if (!reader.read_i32(width)) { return false; }
          out.logical_bit_width = width;
        } else if (inner == 2 && (inner_type == T_BOOLEAN_TRUE || inner_type == T_BOOLEAN_FALSE)) {
          out.logical_is_signed = inner_type == T_BOOLEAN_TRUE;
        } else if (!skip_field(reader, inner_type)) {
          return false;
        }
      }
    } else if (last == LOGICAL_TIMESTAMP) {
      int16_t inner = 0;
      int inner_type = 0;
      bool inner_stop = false;
      for (;;) {
        if (!read_field(reader, inner, inner_type, inner_stop)) { return false; }
        if (inner_stop) { break; }
        if (inner == 2 && inner_type == T_STRUCT) {
          // TimeUnit union: 1 millis, 2 micros, 3 nanos.
          int16_t unit = 0;
          int unit_type = 0;
          bool unit_stop = false;
          for (;;) {
            if (!read_field(reader, unit, unit_type, unit_stop)) { return false; }
            if (unit_stop) { break; }
            out.logical_time_unit = unit - 1;
            if (!skip_field(reader, unit_type)) { return false; }
          }
        } else if (!skip_field(reader, inner_type)) {
          return false;
        }
      }
    } else if (!skip_field(reader, type)) {
      return false;
    }
  }
  return true;
}

bool parse_schema_element(CompactReader &reader, SchemaElement &out)
{
  int16_t last = 0;
  int type = 0;
  bool stop = false;
  for (;;) {
    if (!read_field(reader, last, type, stop)) { return false; }
    if (stop) { break; }
    switch (last) {
      case 1: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.type = value;
        break;
      }
      case 2: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.type_length = value;
        break;
      }
      case 3: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.repetition_type = value;
        break;
      }
      case 4: {
        if (type != T_BINARY) { if (!skip_field(reader, type)) { return false; } break; }
        if (!reader.read_binary(out.name)) { return false; }
        break;
      }
      case 5: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.num_children = value;
        break;
      }
      case 6: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.converted_type = value;
        break;
      }
      case 10: {
        if (type != T_STRUCT) { if (!skip_field(reader, type)) { return false; } break; }
        if (!parse_logical_type(reader, out)) { return false; }
        break;
      }
      default: {
        if (!skip_field(reader, type)) { return false; }
        break;
      }
    }
  }
  return true;
}

bool parse_column_meta(CompactReader &reader, ColumnMeta &out)
{
  int16_t last = 0;
  int type = 0;
  bool stop = false;
  for (;;) {
    if (!read_field(reader, last, type, stop)) { return false; }
    if (stop) { break; }
    switch (last) {
      case 1: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.type = value;
        break;
      }
      case 2: {
        if (type != T_LIST) { if (!skip_field(reader, type)) { return false; } break; }
        uint32_t count = 0;
        int element_type = 0;
        if (!reader.read_list_header(count, element_type)) { return false; }
        for (uint32_t i = 0; i < count; ++i) {
          int32_t value = 0;
          if (element_type == T_I32) {
            if (!reader.read_i32(value)) { return false; }
          } else if (!skip_field(reader, element_type)) {
            return false;
          }
          out.encodings.push_back(value);
        }
        break;
      }
      case 3: {
        if (type != T_LIST) { if (!skip_field(reader, type)) { return false; } break; }
        uint32_t count = 0;
        int element_type = 0;
        if (!reader.read_list_header(count, element_type)) { return false; }
        for (uint32_t i = 0; i < count; ++i) {
          std::string value;
          if (element_type == T_BINARY) {
            if (!reader.read_binary(value)) { return false; }
          } else if (!skip_field(reader, element_type)) {
            return false;
          }
          out.path.push_back(value);
        }
        break;
      }
      case 4: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.codec = value;
        break;
      }
      case 5: {
        if (type != T_I64) { if (!skip_field(reader, type)) { return false; } break; }
        int64_t value = 0;
        if (!reader.read_i64(value)) { return false; }
        out.num_values = value;
        break;
      }
      case 9: {
        if (type != T_I64) { if (!skip_field(reader, type)) { return false; } break; }
        int64_t value = 0;
        if (!reader.read_i64(value)) { return false; }
        out.data_page_offset = value;
        break;
      }
      case 11: {
        if (type != T_I64) { if (!skip_field(reader, type)) { return false; } break; }
        int64_t value = 0;
        if (!reader.read_i64(value)) { return false; }
        out.dictionary_page_offset = value;
        break;
      }
      default: {
        if (!skip_field(reader, type)) { return false; }
        break;
      }
    }
  }
  return true;
}

bool parse_column_chunk(CompactReader &reader, ColumnMeta &out, bool &has_meta)
{
  int16_t last = 0;
  int type = 0;
  bool stop = false;
  for (;;) {
    if (!read_field(reader, last, type, stop)) { return false; }
    if (stop) { break; }
    if (last == 3 && type == T_STRUCT) {
      if (!parse_column_meta(reader, out)) { return false; }
      has_meta = true;
      continue;
    }
    if (!skip_field(reader, type)) { return false; }
  }
  return true;
}

bool parse_row_group(CompactReader &reader, RowGroup &out)
{
  int16_t last = 0;
  int type = 0;
  bool stop = false;
  for (;;) {
    if (!read_field(reader, last, type, stop)) { return false; }
    if (stop) { break; }
    if (last == 1 && type == T_LIST) {
      uint32_t count = 0;
      int element_type = 0;
      if (!reader.read_list_header(count, element_type)) { return false; }
      for (uint32_t i = 0; i < count; ++i) {
        ColumnMeta meta;
        bool has_meta = false;
        if (element_type == T_STRUCT) {
          if (!parse_column_chunk(reader, meta, has_meta)) { return false; }
        } else if (!skip_field(reader, element_type)) {
          return false;
        }
        if (!has_meta) { return false; }
        out.columns.push_back(std::move(meta));
      }
      continue;
    }
    if (last == 3 && type == T_I64) {
      int64_t value = 0;
      if (!reader.read_i64(value)) { return false; }
      out.num_rows = value;
      continue;
    }
    if (!skip_field(reader, type)) { return false; }
  }
  return true;
}

bool parse_file_metadata(CompactReader &reader, std::vector<SchemaElement> &schema,
                          std::vector<RowGroup> &row_groups, int64_t &num_rows)
{
  int16_t last = 0;
  int type = 0;
  bool stop = false;
  for (;;) {
    if (!read_field(reader, last, type, stop)) { return false; }
    if (stop) { break; }
    if (last == 2 && type == T_LIST) {
      uint32_t count = 0;
      int element_type = 0;
      if (!reader.read_list_header(count, element_type)) { return false; }
      for (uint32_t i = 0; i < count; ++i) {
        SchemaElement element;
        if (element_type == T_STRUCT) {
          if (!parse_schema_element(reader, element)) { return false; }
        } else if (!skip_field(reader, element_type)) {
          return false;
        }
        schema.push_back(std::move(element));
      }
      continue;
    }
    if (last == 3 && type == T_I64) {
      int64_t value = 0;
      if (!reader.read_i64(value)) { return false; }
      num_rows = value;
      continue;
    }
    if (last == 4 && type == T_LIST) {
      uint32_t count = 0;
      int element_type = 0;
      if (!reader.read_list_header(count, element_type)) { return false; }
      for (uint32_t i = 0; i < count; ++i) {
        RowGroup group;
        if (element_type == T_STRUCT) {
          if (!parse_row_group(reader, group)) { return false; }
        } else if (!skip_field(reader, element_type)) {
          return false;
        }
        row_groups.push_back(std::move(group));
      }
      continue;
    }
    if (!skip_field(reader, type)) { return false; }
  }
  return true;
}

bool parse_page_header(CompactReader &reader, PageHeader &out)
{
  int16_t last = 0;
  int type = 0;
  bool stop = false;
  for (;;) {
    if (!read_field(reader, last, type, stop)) { return false; }
    if (stop) { break; }
    switch (last) {
      case 1: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.type = value;
        break;
      }
      case 2: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.uncompressed_page_size = value;
        break;
      }
      case 3: {
        if (type != T_I32) { if (!skip_field(reader, type)) { return false; } break; }
        int32_t value = 0;
        if (!reader.read_i32(value)) { return false; }
        out.compressed_page_size = value;
        break;
      }
      case 5: {
        if (type != T_STRUCT) { if (!skip_field(reader, type)) { return false; } break; }
        int16_t inner = 0;
        int inner_type = 0;
        bool inner_stop = false;
        for (;;) {
          if (!read_field(reader, inner, inner_type, inner_stop)) { return false; }
          if (inner_stop) { break; }
          if (inner == 1 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.num_values = value;
          } else if (inner == 2 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.encoding = value;
          } else if (inner == 3 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.definition_level_encoding = value;
          } else if (inner == 4 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.repetition_level_encoding = value;
          } else if (!skip_field(reader, inner_type)) {
            return false;
          }
        }
        break;
      }
      case 7: {
        if (type != T_STRUCT) { if (!skip_field(reader, type)) { return false; } break; }
        int16_t inner = 0;
        int inner_type = 0;
        bool inner_stop = false;
        for (;;) {
          if (!read_field(reader, inner, inner_type, inner_stop)) { return false; }
          if (inner_stop) { break; }
          if (inner == 1 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.dictionary_num_values = value;
          } else if (inner == 2 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.dictionary_encoding = value;
          } else if (!skip_field(reader, inner_type)) {
            return false;
          }
        }
        break;
      }
      case 8: {
        if (type != T_STRUCT) { if (!skip_field(reader, type)) { return false; } break; }
        int16_t inner = 0;
        int inner_type = 0;
        bool inner_stop = false;
        for (;;) {
          if (!read_field(reader, inner, inner_type, inner_stop)) { return false; }
          if (inner_stop) { break; }
          if (inner == 1 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.num_values = value;
          } else if (inner == 2 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.num_nulls = value;
          } else if (inner == 3 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.num_rows = value;
          } else if (inner == 4 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.encoding = value;
          } else if (inner == 5 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.definition_levels_byte_length = value;
          } else if (inner == 6 && inner_type == T_I32) {
            int32_t value = 0;
            if (!reader.read_i32(value)) { return false; }
            out.repetition_levels_byte_length = value;
          } else if (inner == 7 && (inner_type == T_BOOLEAN_TRUE || inner_type == T_BOOLEAN_FALSE)) {
            out.is_compressed = inner_type == T_BOOLEAN_TRUE;
          } else if (!skip_field(reader, inner_type)) {
            return false;
          }
        }
        break;
      }
      default: {
        if (!skip_field(reader, type)) { return false; }
        break;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Snappy raw block decompression
// ---------------------------------------------------------------------------
bool snappy_copy(std::vector<uint8_t> &out, size_t copied, uint64_t offset, uint64_t length, uint64_t total)
{
  if (offset == 0 || offset > copied) { return false; }
  if (length > total - copied) { return false; }
  const size_t source = copied - static_cast<size_t>(offset);
  for (uint64_t i = 0; i < length; ++i) {
    const uint8_t byte = out[source + i];
    out.push_back(byte);
  }
  return true;
}

// Decompresses a Snappy "raw" block: a varint uncompressed length followed by
// literal and copy elements.
bool snappy_decompress(const uint8_t *src, uint64_t src_size, uint64_t expected_size,
                       std::vector<uint8_t> &out)
{
  uint64_t offset = 0;
  uint64_t length = 0;
  int shift = 0;
  for (;;) {
    if (offset >= src_size) { return false; }
    const uint8_t byte = src[offset++];
    length |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) { break; }
    shift += 7;
    if (shift > 63) { return false; }
  }
  if (length != expected_size) { return false; }
  out.clear();
  out.reserve(static_cast<size_t>(length));
  size_t copied = 0;
  while (copied < length) {
    if (offset >= src_size) { return false; }
    const uint8_t tag = src[offset++];
    switch (tag & 0x03) {
      case 0: { // literal
        uint64_t literal_length = tag >> 2;
        if (literal_length >= 60) {
          const uint64_t extra = literal_length - 59;
          if (extra > src_size - offset) { return false; }
          uint64_t value = 0;
          for (uint64_t i = 0; i < extra; ++i) {
            value |= static_cast<uint64_t>(src[offset + i]) << (8 * i);
          }
          offset += extra;
          literal_length = value;
        }
        ++literal_length;
        if (literal_length > src_size - offset) { return false; }
        if (literal_length > length - copied) { return false; }
        out.insert(out.end(), src + offset, src + offset + literal_length);
        offset += literal_length;
        copied += static_cast<size_t>(literal_length);
        break;
      }
      case 1: { // copy with a 1-byte offset
        if (offset >= src_size) { return false; }
        const uint64_t copy_length = 4 + ((tag >> 2) & 0x07);
        const uint64_t copy_offset = (static_cast<uint64_t>(tag >> 5) << 8) | src[offset];
        ++offset;
        if (!snappy_copy(out, copied, copy_offset, copy_length, length)) { return false; }
        break;
      }
      case 2: { // copy with a 2-byte offset
        if (offset + 2 > src_size) { return false; }
        const uint64_t copy_length = 1 + (tag >> 2);
        const uint64_t copy_offset = static_cast<uint64_t>(src[offset])
            | (static_cast<uint64_t>(src[offset + 1]) << 8);
        offset += 2;
        if (!snappy_copy(out, copied, copy_offset, copy_length, length)) { return false; }
        break;
      }
      default: { // copy with a 4-byte offset
        if (offset + 4 > src_size) { return false; }
        const uint64_t copy_length = 1 + (tag >> 2);
        uint64_t copy_offset = 0;
        for (int i = 0; i < 4; ++i) {
          copy_offset |= static_cast<uint64_t>(src[offset + i]) << (8 * i);
        }
        offset += 4;
        if (!snappy_copy(out, copied, copy_offset, copy_length, length)) { return false; }
        break;
      }
    }
  }
  return copied == length;
}

// ---------------------------------------------------------------------------
// RLE / bit-packing hybrid decoding
// ---------------------------------------------------------------------------
class BitReader
{
public:
  BitReader(const uint8_t *data, uint64_t size) : data_(data), size_(size) {}
  bool read_bits(int width, uint64_t &value)
  {
    if (width == 0) { value = 0; return true; }
    if (width > 32) { return false; }
    uint64_t buffer = 0;
    int available = 0;
    while (available < width) {
      if (position_ >= size_) { return false; }
      buffer |= static_cast<uint64_t>(data_[position_++]) << available;
      available += 8;
    }
    value = buffer & ((1ULL << width) - 1);
    return true;
  }

private:
  const uint8_t *data_ = nullptr;
  uint64_t size_ = 0;
  uint64_t position_ = 0;
};

bool decode_rle_hybrid(const uint8_t *data, uint64_t size, int bit_width, int64_t count,
                       std::vector<int32_t> &values)
{
  values.clear();
  if (count <= 0) { return true; }
  if (bit_width < 0 || bit_width > 32) { return false; }
  values.reserve(static_cast<size_t>(count));
  uint64_t position = 0;
  while (static_cast<int64_t>(values.size()) < count) {
    uint64_t header = 0;
    int shift = 0;
    for (;;) {
      if (position >= size) { return false; }
      const uint8_t byte = data[position++];
      header |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) { break; }
      shift += 7;
      if (shift > 63) { return false; }
    }
    if (header & 1) {
      // A bit-packed run always carries whole groups of eight values, so it may
      // hold more values than the page needs. The padding is consumed but only
      // the requested values are kept.
      const uint64_t groups = header >> 1;
      const uint64_t run = groups * 8;
      const uint64_t bytes = (run * static_cast<uint64_t>(bit_width) + 7) / 8;
      if (bytes > size - position) { return false; }
      // Values are packed LSB first and may straddle a byte boundary, so read
      // the little-endian window that covers the current value instead of one
      // byte per value.
      const uint64_t mask = (bit_width >= 32) ? 0xFFFFFFFFull : ((1ull << bit_width) - 1ull);
      for (uint64_t i = 0; i < run; ++i) {
        const uint64_t bit_offset = i * static_cast<uint64_t>(bit_width);
        const uint64_t byte_index = bit_offset / 8;
        const uint64_t shift = bit_offset % 8;
        uint64_t window = data[position + byte_index];
        if (shift + static_cast<uint64_t>(bit_width) > 8 && byte_index + 1 < bytes) {
          window |= static_cast<uint64_t>(data[position + byte_index + 1]) << 8;
        }
        const uint64_t value = (window >> shift) & mask;
        if (static_cast<int64_t>(values.size()) < count) {
          values.push_back(static_cast<int32_t>(value));
        }
      }
      position += bytes;
    } else {
      const uint64_t run = header >> 1;
      const int bytes = (bit_width + 7) / 8;
      if (static_cast<uint64_t>(bytes) > size - position) { return false; }
      uint64_t value = 0;
      for (int i = 0; i < bytes; ++i) {
        value |= static_cast<uint64_t>(data[position + i]) << (8 * i);
      }
      position += static_cast<uint64_t>(bytes);
      for (uint64_t i = 0; i < run; ++i) {
        if (static_cast<int64_t>(values.size()) < count) {
          values.push_back(static_cast<int32_t>(value));
        }
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Value rendering
// ---------------------------------------------------------------------------
enum class ValueKind { BOOLEAN, INT32, INT64, FLOAT, DOUBLE, BYTE_ARRAY, DATE, TIMESTAMP };

int64_t floor_div(int64_t value, int64_t divisor)
{
  int64_t quotient = value / divisor;
  if ((value % divisor) != 0 && ((value < 0) != (divisor < 0))) { --quotient; }
  return quotient;
}

void append_padded(std::string &out, int value, int width)
{
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%0*d", width, value);
  out.append(buffer);
}

void format_date(int32_t days, std::string &out)
{
  // Civil calendar from a day number relative to 1970-01-01.
  const int32_t shifted = days + 719468;
  const int32_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  const uint32_t day_of_era = static_cast<uint32_t>(shifted - era * 146097);
  const uint32_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524
                                - day_of_era / 146096) / 365;
  const int32_t year = static_cast<int32_t>(year_of_era) + era * 400;
  const uint32_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const uint32_t month_index = (5 * day_of_year + 2) / 153;
  const uint32_t day = day_of_year - (153 * month_index + 2) / 5 + 1;
  const uint32_t month = month_index < 10 ? month_index + 3 : month_index - 9;
  // Months 1 and 2 belong to the prior civil year in the 3-March epoch.
  const int32_t full_year = month <= 2 ? year + 1 : year;
  out.clear();
  out.append(std::to_string(full_year));
  out.push_back('-');
  append_padded(out, static_cast<int>(month), 2);
  out.push_back('-');
  append_padded(out, static_cast<int>(day), 2);
}

void format_timestamp(int64_t value, int unit, std::string &out)
{
  int64_t seconds = value;
  if (unit == 0) { seconds = floor_div(value, 1000); }
  else if (unit == 1) { seconds = floor_div(value, 1000000); }
  else { seconds = floor_div(value, 1000000000); }
  const int64_t days = floor_div(seconds, 86400);
  const int64_t remainder = seconds - days * 86400;
  format_date(static_cast<int32_t>(days), out);
  out.push_back(' ');
  append_padded(out, static_cast<int>(remainder / 3600), 2);
  out.push_back(':');
  append_padded(out, static_cast<int>((remainder % 3600) / 60), 2);
  out.push_back(':');
  append_padded(out, static_cast<int>(remainder % 60), 2);
}

void format_double(double value, std::string &out)
{
  char buffer[64];
  for (int precision = 6; precision < 17; ++precision) {
    snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    if (strtod(buffer, nullptr) == value) { break; }
  }
  out.assign(buffer);
}

const char *physical_type_name(int type)
{
  switch (type) {
    case PT_BOOLEAN: return "BOOLEAN";
    case PT_INT32: return "INT32";
    case PT_INT64: return "INT64";
    case PT_INT96: return "INT96";
    case PT_FLOAT: return "FLOAT";
    case PT_DOUBLE: return "DOUBLE";
    case PT_BYTE_ARRAY: return "BYTE_ARRAY";
    case PT_FIXED_LEN_BYTE_ARRAY: return "FIXED_LEN_BYTE_ARRAY";
    default: return "UNKNOWN";
  }
}

const char *logical_type_name(int converted, int logical)
{
  if (logical == LOGICAL_DATE || converted == CONVERTED_DATE) { return "DATE"; }
  if (logical == LOGICAL_TIMESTAMP || converted == CONVERTED_TIMESTAMP_MILLIS
      || converted == CONVERTED_TIMESTAMP_MICROS) {
    return "TIMESTAMP";
  }
  if (logical == LOGICAL_INTEGER) { return "INTEGER"; }
  if (converted == CONVERTED_UTF8) { return "STRING"; }
  return "";
}

// Decodes PLAIN encoded values of one page into their rendered text form.
bool decode_plain_page(ValueKind kind, int logical_time_unit, const uint8_t *data, uint64_t size,
                        uint64_t &position, int64_t count, uint64_t max_string_bytes,
                        std::vector<std::string> &out, std::string &error)
{
  for (int64_t i = 0; i < count; ++i) {
    switch (kind) {
      case ValueKind::BOOLEAN: {
        const uint64_t bit_index = position * 8 + static_cast<uint64_t>(i);
        const uint64_t byte_index = bit_index / 8;
        if (byte_index >= size) { error = "The Parquet boolean page is truncated"; return false; }
        const bool value = ((data[byte_index] >> (bit_index % 8)) & 0x01) != 0;
        out.emplace_back(value ? "true" : "false");
        break;
      }
      case ValueKind::INT32:
      case ValueKind::DATE: {
        if (size - position < 4) { error = "The Parquet integer page is truncated"; return false; }
        int32_t value = 0;
        memcpy(&value, data + position, sizeof(value));
        position += 4;
        std::string text;
        if (kind == ValueKind::DATE) { format_date(value, text); }
        else { text.assign(std::to_string(value)); }
        out.push_back(std::move(text));
        break;
      }
      case ValueKind::INT64:
      case ValueKind::TIMESTAMP: {
        if (size - position < 8) { error = "The Parquet big integer page is truncated"; return false; }
        int64_t value = 0;
        memcpy(&value, data + position, sizeof(value));
        position += 8;
        std::string text;
        if (kind == ValueKind::TIMESTAMP) { format_timestamp(value, logical_time_unit, text); }
        else { text.assign(std::to_string(value)); }
        out.push_back(std::move(text));
        break;
      }
      case ValueKind::FLOAT: {
        if (size - position < 4) { error = "The Parquet float page is truncated"; return false; }
        float value = 0;
        memcpy(&value, data + position, sizeof(value));
        position += 4;
        if (!std::isfinite(value)) { error = "A Parquet float value is not finite"; return false; }
        std::string text;
        format_double(static_cast<double>(value), text);
        out.push_back(std::move(text));
        break;
      }
      case ValueKind::DOUBLE: {
        if (size - position < 8) { error = "The Parquet double page is truncated"; return false; }
        double value = 0;
        memcpy(&value, data + position, sizeof(value));
        position += 8;
        if (!std::isfinite(value)) { error = "A Parquet double value is not finite"; return false; }
        std::string text;
        format_double(value, text);
        out.push_back(std::move(text));
        break;
      }
      case ValueKind::BYTE_ARRAY: {
        if (size - position < 4) { error = "The Parquet byte array page is truncated"; return false; }
        uint32_t length = 0;
        memcpy(&length, data + position, sizeof(length));
        position += 4;
        if (length > max_string_bytes || length > size - position) {
          error = "A Parquet byte array value is out of range";
          return false;
        }
        out.emplace_back(reinterpret_cast<const char *>(data + position), length);
        position += length;
        break;
      }
    }
  }
  return true;
}
} // namespace

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
struct ParquetReader::Impl
{
  struct ColumnState
  {
    const ColumnMeta *meta = nullptr;
    uint64_t next_offset = 0;
    int64_t values_left = 0;
    std::vector<std::string> dictionary;
    bool dictionary_loaded = false;
    std::vector<Field> pending;
    size_t pending_position = 0;
  };

  struct ColumnSchema
  {
    std::string name;
    ValueKind kind = ValueKind::BYTE_ARRAY;
    int physical = PT_BYTE_ARRAY;
    int converted = -1;
    int logical = -1;
    int logical_time_unit = 0;
    int max_definition_level = 0;
    Type type = Type::VARCHAR;
  };

  RandomAccessInput &input;
  ParquetOptions options;
  uint64_t file_size = 0;
  std::vector<ColumnSchema> columns;
  std::vector<RowGroup> row_groups;

  // Iteration state.
  size_t group_index = 0;
  int64_t rows_read = 0;
  std::vector<ColumnState> states;

  Diagnostic diagnostic;

  Impl(RandomAccessInput &input, const ParquetOptions &options) : input(input), options(options) {}

  Status fail(Status status, const std::string &message)
  {
    diagnostic.message = message;
    return status;
  }

  Status read_exact(uint64_t offset, char *buffer, uint64_t length)
  {
    if (offset > file_size || length > file_size - offset) {
      return fail(Status::INVALID, "The Parquet read is outside the file boundary");
    }
    Diagnostic local;
    if (input.read_at(offset, buffer, length, local) != Status::OK) {
      diagnostic.message = local.message.empty() ? "Cannot read the Parquet file" : local.message;
      return Status::IO_ERROR;
    }
    return Status::OK;
  }

  Status read_page_header(uint64_t offset, PageHeader &header, uint64_t &header_length);
  Status load_dictionary(size_t index, const PageHeader &header, const uint8_t *page, uint64_t page_size);
  Status decode_page(size_t index, const PageHeader &header, const uint8_t *page, uint64_t page_size);
  Status read_next_page(ColumnState &state);
  Status start_row_group();
};

Status ParquetReader::Impl::read_page_header(uint64_t offset, PageHeader &header, uint64_t &header_length)
{
  uint64_t probe = 4096;
  for (;;) {
    if (offset >= file_size) {
      return fail(Status::INVALID, "The Parquet page header is outside the file boundary");
    }
    uint64_t available = std::min(probe, file_size - offset);
    if (available > options.max_page_bytes) { available = options.max_page_bytes; }
    std::vector<uint8_t> buffer(static_cast<size_t>(available));
    Diagnostic local;
    if (input.read_at(offset, reinterpret_cast<char *>(buffer.data()), available, local) != Status::OK) {
      diagnostic.message = local.message.empty() ? "Cannot read the Parquet file" : local.message;
      return Status::IO_ERROR;
    }
    CompactReader reader(buffer.data(), buffer.size());
    PageHeader parsed;
    if (parse_page_header(reader, parsed)) {
      header = parsed;
      header_length = reader.position();
      return Status::OK;
    }
    if (!reader.truncated()) {
      return fail(Status::INVALID, "The Parquet page header is malformed");
    }
    if (available >= options.max_page_bytes || probe >= options.max_page_bytes) {
      return fail(Status::INVALID, "The Parquet page header is too large");
    }
    probe = std::min(probe * 2, options.max_page_bytes);
  }
}

Status ParquetReader::Impl::load_dictionary(size_t index, const PageHeader &header,
                                            const uint8_t *page, uint64_t page_size)
{
  const ColumnSchema &column = columns[index];
  ColumnState &state = states[index];
  const int64_t count = header.dictionary_num_values;
  if (count < 0 || static_cast<uint64_t>(count) > options.max_dictionary_values) {
    return fail(Status::LIMIT, "The Parquet dictionary has too many values");
  }
  if (header.dictionary_encoding != ENCODING_PLAIN
      && header.dictionary_encoding != ENCODING_PLAIN_DICTIONARY) {
    return fail(Status::INVALID, "The Parquet dictionary encoding is not supported");
  }
  std::vector<uint8_t> plain;
  if (state.meta->codec == CODEC_UNCOMPRESSED) {
    if (page_size != static_cast<uint64_t>(header.uncompressed_page_size)) {
      return fail(Status::INVALID, "The Parquet dictionary page size does not match its header");
    }
    plain.assign(page, page + page_size);
  } else if (state.meta->codec == CODEC_SNAPPY) {
    if (!snappy_decompress(page, page_size,
                           static_cast<uint64_t>(header.uncompressed_page_size), plain)) {
      return fail(Status::INVALID, "Cannot decompress the Parquet dictionary page");
    }
  } else {
    return fail(Status::INVALID, "The Parquet compression codec is not supported");
  }
  uint64_t position = 0;
  std::vector<std::string> values;
  std::string error;
  if (!decode_plain_page(column.kind, column.logical_time_unit, plain.data(), plain.size(),
                         position, count, options.max_string_bytes, values, error)) {
    return fail(Status::INVALID, error);
  }
  state.dictionary = std::move(values);
  state.dictionary_loaded = true;
  return Status::OK;
}

Status ParquetReader::Impl::decode_page(size_t index, const PageHeader &header,
                                        const uint8_t *page, uint64_t page_size)
{
  const ColumnSchema &column = columns[index];
  ColumnState &state = states[index];
  const int64_t num_values = header.num_values;
  if (num_values < 0 || static_cast<uint64_t>(num_values) > options.max_values_per_page
      || num_values > static_cast<int64_t>(options.max_row_values)) {
    return fail(Status::LIMIT, "The Parquet page has too many values");
  }

  std::vector<uint8_t> plain;
  const uint8_t *values = nullptr;
  uint64_t values_size = 0;
  std::vector<int32_t> definition_levels;

  if (header.type == PAGE_DATA_V2) {
    const uint64_t repetition_bytes = static_cast<uint64_t>(header.repetition_levels_byte_length);
    const uint64_t definition_bytes = static_cast<uint64_t>(header.definition_levels_byte_length);
    if (repetition_bytes != 0) {
      return fail(Status::INVALID, "Repeated Parquet columns are not supported");
    }
    if (repetition_bytes + definition_bytes > page_size) {
      return fail(Status::INVALID, "The Parquet level sections exceed the page size");
    }
    if (static_cast<uint64_t>(header.uncompressed_page_size) < repetition_bytes + definition_bytes) {
      return fail(Status::INVALID, "The Parquet page size does not match its header");
    }
    if (column.max_definition_level > 0) {
      int bit_width = 0;
      while ((1 << bit_width) <= column.max_definition_level) { ++bit_width; }
      if (!decode_rle_hybrid(page + repetition_bytes, definition_bytes, bit_width, num_values, definition_levels)) {
        return fail(Status::INVALID, "The Parquet definition levels are malformed");
      }
    }
    const uint64_t compressed_values = page_size - repetition_bytes - definition_bytes;
    const uint64_t plain_values = static_cast<uint64_t>(header.uncompressed_page_size)
        - repetition_bytes - definition_bytes;
    if (header.is_compressed && state.meta->codec != CODEC_UNCOMPRESSED) {
      if (state.meta->codec != CODEC_SNAPPY) {
        return fail(Status::INVALID, "The Parquet compression codec is not supported");
      }
      if (!snappy_decompress(page + repetition_bytes + definition_bytes, compressed_values,
                             plain_values, plain)) {
        return fail(Status::INVALID, "Cannot decompress the Parquet data page");
      }
      values = plain.data();
      values_size = plain.size();
    } else {
      values = page + repetition_bytes + definition_bytes;
      values_size = compressed_values;
    }
  } else {
    if (state.meta->codec == CODEC_UNCOMPRESSED) {
      if (page_size != static_cast<uint64_t>(header.uncompressed_page_size)) {
        return fail(Status::INVALID, "The Parquet page size does not match its header");
      }
      plain.assign(page, page + page_size);
    } else if (state.meta->codec == CODEC_SNAPPY) {
      if (!snappy_decompress(page, page_size,
                             static_cast<uint64_t>(header.uncompressed_page_size), plain)) {
        return fail(Status::INVALID, "Cannot decompress the Parquet data page");
      }
    } else {
      return fail(Status::INVALID, "The Parquet compression codec is not supported");
    }
    uint64_t position = 0;
    if (column.max_definition_level > 0) {
      if (header.definition_level_encoding != ENCODING_RLE) {
        return fail(Status::INVALID, "The Parquet definition level encoding is not supported");
      }
      if (plain.size() - position < 4) {
        return fail(Status::INVALID, "The Parquet page is missing definition levels");
      }
      uint32_t length = 0;
      memcpy(&length, plain.data() + position, sizeof(length));
      position += 4;
      if (length > plain.size() - position) {
        return fail(Status::INVALID, "The Parquet definition levels exceed the page");
      }
      int bit_width = 0;
      while ((1 << bit_width) <= column.max_definition_level) { ++bit_width; }
      if (!decode_rle_hybrid(plain.data() + position, length, bit_width, num_values,
                             definition_levels)) {
      }
      position += length;
    }
    values = plain.empty() ? nullptr : plain.data() + position;
    values_size = plain.size() - position;
  }

  // The definition levels decide which slots are null; the page header only
  // reports the null count of a v2 page.
  int64_t null_count = 0;
  if (column.max_definition_level > 0) {
    for (int32_t level : definition_levels) {
      if (level < column.max_definition_level) { ++null_count; }
    }
    if (header.type == PAGE_DATA_V2 && header.num_nulls != null_count) {
      return fail(Status::INVALID, "The Parquet page null count does not match its definition levels");
    }
  }
  const int64_t value_count = num_values - null_count;
  if (value_count < 0) {
    return fail(Status::INVALID, "The Parquet page reports more nulls than values");
  }

  std::vector<std::string> rendered;
  rendered.reserve(static_cast<size_t>(value_count));
  uint64_t position = 0;
  if (header.encoding == ENCODING_PLAIN) {
    std::string error;
    if (!decode_plain_page(column.kind, column.logical_time_unit, values, values_size, position,
                           value_count, options.max_string_bytes, rendered, error)) {
      return fail(Status::INVALID, error);
    }
  } else if (header.encoding == ENCODING_RLE_DICTIONARY
             || header.encoding == ENCODING_PLAIN_DICTIONARY) {
    if (!state.dictionary_loaded) {
      return fail(Status::INVALID, "The Parquet page uses a dictionary that was never loaded");
    }
    if (values_size < 1) {
      return fail(Status::INVALID, "The Parquet dictionary indices are malformed");
    }
    const int bit_width = values[0];
    std::vector<int32_t> indices;
    if (!decode_rle_hybrid(values + 1, values_size - 1, bit_width, value_count, indices)) {
      return fail(Status::INVALID, "The Parquet dictionary indices are malformed");
    }
    for (int32_t dictionary_index : indices) {
      if (dictionary_index < 0
          || static_cast<size_t>(dictionary_index) >= state.dictionary.size()) {
        return fail(Status::INVALID, "A Parquet dictionary index is out of range");
      }
      rendered.push_back(state.dictionary[dictionary_index]);
    }
  } else {
    return fail(Status::INVALID, "The Parquet page encoding is not supported");
  }

  state.pending.clear();
  state.pending_position = 0;
  state.pending.reserve(static_cast<size_t>(num_values));
  size_t next_value = 0;
  for (int64_t i = 0; i < num_values; ++i) {
    bool is_null = false;
    if (column.max_definition_level > 0) {
      if (static_cast<size_t>(i) >= definition_levels.size()) {
        return fail(Status::INVALID, "The Parquet page has fewer definition levels than values");
      }
      is_null = definition_levels[i] < column.max_definition_level;
    }
    if (is_null) {
      Field field;
      field.is_null = true;
      state.pending.push_back(std::move(field));
    } else {
      if (next_value >= rendered.size()) {
        return fail(Status::INVALID, "The Parquet page has fewer values than its definition levels");
      }
      Field field;
      field.text = rendered[next_value++];
      state.pending.push_back(std::move(field));
    }
  }
  return Status::OK;
}

Status ParquetReader::Impl::read_next_page(ColumnState &state)
{
  const size_t index = &state - states.data();
  for (;;) {
    PageHeader header;
    uint64_t header_length = 0;
    const Status header_status = read_page_header(state.next_offset, header, header_length);
    if (header_status != Status::OK) { return header_status; }
    if (header.compressed_page_size < 0 || header.uncompressed_page_size < 0) {
      return fail(Status::INVALID, "The Parquet page size is negative");
    }
    const uint64_t compressed = static_cast<uint64_t>(header.compressed_page_size);
    const uint64_t uncompressed = static_cast<uint64_t>(header.uncompressed_page_size);
    if (compressed > options.max_page_bytes || uncompressed > options.max_page_bytes) {
      return fail(Status::LIMIT, "The Parquet page exceeds the page size limit");
    }
    const uint64_t data_offset = state.next_offset + header_length;
    if (data_offset > file_size || compressed > file_size - data_offset) {
      return fail(Status::INVALID, "The Parquet page is outside the file boundary");
    }
    std::vector<uint8_t> page(static_cast<size_t>(compressed));
    if (compressed > 0) {
      const Status read_status = read_exact(data_offset, reinterpret_cast<char *>(page.data()),
                                             compressed);
      if (read_status != Status::OK) { return read_status; }
    }
    state.next_offset = data_offset + compressed;
    if (header.type == PAGE_DICTIONARY) {
      const Status status = load_dictionary(index, header, page.data(), page.size());
      if (status != Status::OK) { return status; }
      continue;
    }
    if (header.type == PAGE_INDEX) {
      return fail(Status::INVALID, "An unexpected Parquet index page was found");
    }
    if (header.type != PAGE_DATA && header.type != PAGE_DATA_V2) {
      return fail(Status::INVALID, "The Parquet page type is not supported");
    }
    const Status status = decode_page(index, header, page.data(), page.size());
    if (status != Status::OK) { return status; }
    state.values_left -= header.num_values;
    return Status::OK;
  }
}

Status ParquetReader::Impl::start_row_group()
{
  states.clear();
  const RowGroup &group = row_groups[group_index];
  if (group.columns.size() != columns.size()) {
    return fail(Status::INVALID, "A Parquet row group does not match the file schema");
  }
  states.resize(columns.size());
  for (size_t i = 0; i < columns.size(); ++i) {
    ColumnState &state = states[i];
    state.meta = &group.columns[i];
    state.values_left = state.meta->num_values;
    state.pending.clear();
    state.pending_position = 0;
    state.dictionary.clear();
    state.dictionary_loaded = false;
    if (state.meta->num_values < 0) {
      return fail(Status::INVALID, "A Parquet column chunk has a negative value count");
    }
    if (state.meta->type != columns[i].physical) {
      return fail(Status::INVALID, "A Parquet column chunk type does not match the file schema");
    }
    if (state.meta->path.size() != 1 || state.meta->path[0] != columns[i].name) {
      return fail(Status::INVALID, "A Parquet column chunk path does not match the file schema");
    }
    if (state.meta->data_page_offset < 0) {
      return fail(Status::INVALID, "A Parquet column chunk has a negative data page offset");
    }
    state.next_offset = state.meta->dictionary_page_offset >= 0
        ? static_cast<uint64_t>(state.meta->dictionary_page_offset)
        : static_cast<uint64_t>(state.meta->data_page_offset);
  }
  rows_read = 0;
  return Status::OK;
}

ParquetReader::ParquetReader(RandomAccessInput &input, const ParquetOptions &options)
  : input_(input), options_(options) {}

ParquetReader::~ParquetReader() = default;

Status ParquetReader::prepare(CsvSchema &schema, Diagnostic &diagnostic)
{
  impl_.reset(new Impl(input_, options_));
  Impl &impl = *impl_;
  diagnostic_ = {};
  impl.file_size = input_.size();
  if (impl.file_size < 12) {
    diagnostic.message = "The Parquet file is too small to be valid";
    diagnostic_ = diagnostic;
    return Status::INVALID;
  }
  char magic[4] = {0};
  if (impl.read_exact(0, magic, 4) != Status::OK) {
    diagnostic = impl.diagnostic;
    diagnostic_ = diagnostic;
    return Status::IO_ERROR;
  }
  if (memcmp(magic, "PAR1", 4) != 0) {
    diagnostic.message = "The file does not start with the Parquet magic bytes";
    diagnostic_ = diagnostic;
    return Status::INVALID;
  }
  uint32_t footer_length = 0;
  if (impl.read_exact(impl.file_size - 8, magic, 4) != Status::OK) {
    diagnostic = impl.diagnostic;
    diagnostic_ = diagnostic;
    return Status::IO_ERROR;
  }
  memcpy(&footer_length, magic, sizeof(footer_length));
  char footer_magic[4] = {0};
  if (impl.read_exact(impl.file_size - 4, footer_magic, 4) != Status::OK) {
    diagnostic = impl.diagnostic;
    diagnostic_ = diagnostic;
    return Status::IO_ERROR;
  }
  if (memcmp(footer_magic, "PAR1", 4) != 0) {
    diagnostic.message = "The Parquet footer magic bytes are missing";
    diagnostic_ = diagnostic;
    return Status::INVALID;
  }
  if (footer_length == 0 || footer_length > options_.max_footer_bytes
      || footer_length + 12 > impl.file_size) {
    diagnostic.message = "The Parquet footer size is out of range";
    diagnostic_ = diagnostic;
    return Status::INVALID;
  }
  std::vector<uint8_t> footer(footer_length);
  if (impl.read_exact(impl.file_size - 8 - footer_length,
                      reinterpret_cast<char *>(footer.data()), footer_length) != Status::OK) {
    diagnostic = impl.diagnostic;
    diagnostic_ = diagnostic;
    return Status::IO_ERROR;
  }
  std::vector<SchemaElement> schema_elements;
  std::vector<RowGroup> row_groups;
  int64_t num_rows = 0;
  CompactReader reader(footer.data(), footer.size());
  if (!parse_file_metadata(reader, schema_elements, row_groups, num_rows)) {
    diagnostic.message = "The Parquet footer metadata is malformed";
    diagnostic_ = diagnostic;
    return Status::INVALID;
  }
  if (schema_elements.empty()) {
    diagnostic.message = "The Parquet file has no schema";
    diagnostic_ = diagnostic;
    return Status::INVALID;
  }
  if (schema_elements.size() > options_.max_schema_nodes) {
    diagnostic.message = "The Parquet schema has too many nodes";
    diagnostic_ = diagnostic;
    return Status::LIMIT;
  }
  const SchemaElement &root = schema_elements[0];
  if (root.num_children <= 0 || schema_elements.size() != static_cast<size_t>(root.num_children) + 1) {
    diagnostic.message = "Only flat Parquet schemas are supported";
    diagnostic_ = diagnostic;
    return Status::INVALID;
  }
  if (static_cast<uint64_t>(root.num_children) > options_.max_columns) {
    diagnostic.message = "The Parquet file has too many columns";
    diagnostic_ = diagnostic;
    return Status::LIMIT;
  }
  impl.columns.reserve(static_cast<size_t>(root.num_children));
  for (int i = 0; i < root.num_children; ++i) {
    const SchemaElement &element = schema_elements[1 + i];
    if (element.num_children > 0) {
      diagnostic.message = "Only flat Parquet schemas are supported";
      diagnostic_ = diagnostic;
      return Status::INVALID;
    }
    if (element.repetition_type == REPETITION_REPEATED) {
      diagnostic.message = "Repeated Parquet columns are not supported";
      diagnostic_ = diagnostic;
      return Status::INVALID;
    }
    if (element.repetition_type != REPETITION_REQUIRED
        && element.repetition_type != REPETITION_OPTIONAL) {
      diagnostic.message = "The Parquet column repetition type is unknown";
      diagnostic_ = diagnostic;
      return Status::INVALID;
    }
    if (element.name.empty() || element.name.size() > options_.max_name_bytes) {
      diagnostic.message = "A Parquet column name is empty or too long";
      diagnostic_ = diagnostic;
      return Status::INVALID;
    }
    if (element.converted_type == CONVERTED_DECIMAL) {
      diagnostic.message = "Decimal Parquet columns are not supported";
      diagnostic_ = diagnostic;
      return Status::INVALID;
    }
    Impl::ColumnSchema column;
    column.name = element.name;
    column.physical = element.type;
    column.converted = element.converted_type;
    column.logical = element.logical_type;
    column.logical_time_unit = element.logical_time_unit;
    column.max_definition_level = element.repetition_type == REPETITION_OPTIONAL ? 1 : 0;
    switch (element.type) {
      case PT_BOOLEAN:
        column.kind = ValueKind::BOOLEAN;
        column.type = Type::BOOLEAN;
        break;
      case PT_INT32:
        if (element.converted_type == CONVERTED_DATE || element.logical_type == LOGICAL_DATE) {
          column.kind = ValueKind::DATE;
          column.type = Type::DATE;
        } else {
          column.kind = ValueKind::INT32;
          column.type = Type::BIGINT;
        }
        break;
      case PT_INT64:
        if (element.converted_type == CONVERTED_TIMESTAMP_MILLIS
            || element.converted_type == CONVERTED_TIMESTAMP_MICROS
            || element.logical_type == LOGICAL_TIMESTAMP) {
          column.kind = ValueKind::TIMESTAMP;
          column.type = Type::DATETIME;
        } else {
          column.kind = ValueKind::INT64;
          column.type = Type::BIGINT;
        }
        break;
      case PT_FLOAT:
        column.kind = ValueKind::FLOAT;
        column.type = Type::DOUBLE;
        break;
      case PT_DOUBLE:
        column.kind = ValueKind::DOUBLE;
        column.type = Type::DOUBLE;
        break;
      case PT_BYTE_ARRAY:
        column.kind = ValueKind::BYTE_ARRAY;
        column.type = Type::VARCHAR;
        break;
      default:
        diagnostic.message = std::string("The Parquet column type ")
            + physical_type_name(element.type) + " is not supported";
        diagnostic_ = diagnostic;
        return Status::INVALID;
    }
    impl.columns.push_back(std::move(column));
  }
  if (row_groups.size() > options_.max_row_groups) {
    diagnostic.message = "The Parquet file has too many row groups";
    diagnostic_ = diagnostic;
    return Status::LIMIT;
  }
  int64_t group_rows = 0;
  for (const RowGroup &group : row_groups) {
    if (group.num_rows < 0) {
      diagnostic.message = "A Parquet row group has a negative row count";
      diagnostic_ = diagnostic;
      return Status::INVALID;
    }
    group_rows += group.num_rows;
    if (group_rows > static_cast<int64_t>(options_.max_rows)) {
      diagnostic.message = "The Parquet file has too many rows";
      diagnostic_ = diagnostic;
      return Status::LIMIT;
    }
  }
  impl.row_groups = std::move(row_groups);

  columns_.clear();
  columns_.reserve(impl.columns.size());
  schema.names.clear();
  schema.types.clear();
  schema.nullable.clear();
  for (const Impl::ColumnSchema &column : impl.columns) {
    ParquetColumnInfo info;
    info.name = column.name;
    info.physical_type = physical_type_name(column.physical);
    info.logical_type = logical_type_name(column.converted, column.logical);
    info.type = column.type;
    info.optional = column.max_definition_level > 0;
    columns_.push_back(std::move(info));
    schema.names.push_back(column.name);
    schema.types.push_back(column.type);
    schema.nullable.push_back(info.optional);
  }
  schema.row_count = static_cast<uint64_t>(group_rows);
  diagnostic = diagnostic_;
  return Status::OK;
}

Status ParquetReader::rewind(Diagnostic &diagnostic)
{
  if (!impl_) {
    diagnostic.message = "The Parquet reader is not prepared";
    return Status::INVALID;
  }
  impl_->group_index = 0;
  impl_->rows_read = 0;
  impl_->states.clear();
  if (impl_->row_groups.empty()) { return Status::OK; }
  const Status status = impl_->start_row_group();
  diagnostic = impl_->diagnostic;
  return status;
}

Status ParquetReader::next(std::vector<Field> &row)
{
  row.clear();
  if (!impl_) {
    diagnostic_.message = "The Parquet reader is not prepared";
    return Status::INVALID;
  }
  Impl &impl = *impl_;
  for (;;) {
    if (impl.group_index >= impl.row_groups.size()) {
      diagnostic_ = impl.diagnostic;
      return Status::END;
    }
    const RowGroup &group = impl.row_groups[impl.group_index];
    if (impl.rows_read >= group.num_rows) {
      ++impl.group_index;
      if (impl.group_index >= impl.row_groups.size()) {
        diagnostic_ = impl.diagnostic;
        return Status::END;
      }
      const Status status = impl.start_row_group();
      if (status != Status::OK) {
        diagnostic_ = impl.diagnostic;
        return status;
      }
      continue;
    }
    for (size_t i = 0; i < impl.states.size(); ++i) {
      Impl::ColumnState &state = impl.states[i];
      while (state.pending_position >= state.pending.size()) {
        if (state.values_left <= 0) {
          impl.diagnostic.message = "A Parquet column chunk ended before its row group";
          diagnostic_ = impl.diagnostic;
          return Status::INVALID;
        }
        const Status status = impl.read_next_page(state);
        if (status != Status::OK) {
          diagnostic_ = impl.diagnostic;
          return status;
        }
      }
    }
    row.reserve(impl.states.size());
    for (Impl::ColumnState &state : impl.states) {
      row.push_back(state.pending[state.pending_position++]);
    }
    ++impl.rows_read;
    return Status::OK;
  }
}
} // namespace oceanbase::sql::file_scan
