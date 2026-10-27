/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace oceanbase::sql::file_scan
{
enum class Status { OK, END, INVALID, LIMIT, IO_ERROR };

struct Diagnostic
{
  uint64_t record = 0;
  uint64_t line = 1;
  std::string message;
};

struct CsvOptions
{
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string null_text;
  uint64_t max_record_bytes = 8 * 1024 * 1024;
  uint64_t max_columns = 1024;
};

struct Field
{
  std::string text;
  bool is_null = false;
};

// Keeps only one record in memory. The owner supplies the secured file stream
// and must revalidate its file identity before each executor batch.
class CsvReader
{
public:
  CsvReader(std::istream &input, const CsvOptions &options);
  Status next(std::vector<Field> &row);
  const Diagnostic &diagnostic() const { return diagnostic_; }

private:
  int read_byte();
  Status fail(Status status, const char *message);
  Status finish_field(std::vector<Field> &row, std::string &text, bool quoted);
  std::istream &input_;
  CsvOptions options_;
  Diagnostic diagnostic_;
  std::string prefix_;
  size_t prefix_position_ = 0;
  bool initialized_ = false;
  bool failed_ = false;
};

enum class Type { NULL_VALUE, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
Type infer_type(const Field &field);
Type widen(Type left, Type right);
Type result_type(Type accumulated);

struct CsvSchema
{
  std::vector<std::string> names;
  std::vector<Type> types;
  std::vector<bool> nullable;
  uint64_t row_count = 0;
};

// Full inference retains only column metadata and one record, never file data.
// The caller reopens or rewinds a validated file for the later execution pass.
Status infer_csv_schema(std::istream &input, const CsvOptions &options,
                        CsvSchema &schema, Diagnostic &diagnostic);

// Produce deterministic case-insensitively unique headers without shadowing
// a later literal name (e.g. id,id,id__2).
std::vector<std::string> column_names(const std::vector<Field> &header);
} // namespace oceanbase::sql::file_scan
