/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "file_scan_utils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase::sql::file_scan
{
struct ParquetOptions
{
  uint64_t max_footer_bytes = 16 * 1024 * 1024;
  uint64_t max_metadata_bytes = 32 * 1024 * 1024;
  uint64_t max_row_groups = 65536;
  uint64_t max_columns = 1024;
  uint64_t max_name_bytes = 256;
  uint64_t max_schema_nodes = 16384;
  uint64_t max_thrift_nesting = 16;
  uint64_t max_values_per_page = 16 * 1024 * 1024;
  uint64_t max_page_bytes = 32 * 1024 * 1024;
  uint64_t max_dictionary_values = 16 * 1024 * 1024;
  uint64_t max_dictionary_bytes = 128 * 1024 * 1024;
  uint64_t max_string_bytes = 4 * 1024 * 1024;
  uint64_t max_row_values = 4 * 1024 * 1024;
  uint64_t max_rows = 1ULL << 40;
};

struct ParquetColumnInfo
{
  std::string name;
  std::string physical_type;
  std::string logical_type;
  Type type = Type::VARCHAR;
  bool optional = false;
};

// Standalone, seekable reader over a Parquet file.
//
// prepare() parses and validates the footer, the flat schema and every column
// chunk metadata without touching data pages; next() streams one logical row
// at a time and decodes only the pages it needs; rewind() restarts the scan.
//
// Supported: flat schemas of required or optional BOOLEAN, INT32, INT64,
// FLOAT, DOUBLE and BYTE_ARRAY columns, PLAIN and dictionary
// (PLAIN_DICTIONARY / RLE_DICTIONARY) encodings, RLE definition levels,
// DATA_PAGE and DATA_PAGE_V2 pages, and the UNCOMPRESSED and SNAPPY codecs.
// DATE is reported as DATE, TIMESTAMP as DATETIME and every integer as
// BIGINT. Everything else - nested or repeated fields, INT96,
// FIXED_LEN_BYTE_ARRAY, decimals, other codecs or encodings, and malformed
// files - fails explicitly with a Diagnostic message; nothing is guessed.
class ParquetReader
{
public:
  ParquetReader(RandomAccessInput &input, const ParquetOptions &options);
  ~ParquetReader();
  ParquetReader(const ParquetReader &) = delete;
  ParquetReader &operator=(const ParquetReader &) = delete;

  Status prepare(CsvSchema &schema, Diagnostic &diagnostic);
  Status next(std::vector<Field> &row);
  Status rewind(Diagnostic &diagnostic);
  const Diagnostic &diagnostic() const { return diagnostic_; }
  const std::vector<ParquetColumnInfo> &columns() const { return columns_; }

private:
  struct Impl;
  RandomAccessInput &input_;
  ParquetOptions options_;
  std::unique_ptr<Impl> impl_;
  std::vector<ParquetColumnInfo> columns_;
  Diagnostic diagnostic_;
};
} // namespace oceanbase::sql::file_scan
