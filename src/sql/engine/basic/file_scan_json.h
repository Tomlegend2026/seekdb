/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "file_scan_utils.h"

namespace oceanbase::sql::file_scan
{
struct JsonOptions
{
  uint64_t max_record_bytes = 8 * 1024 * 1024;
  uint64_t max_columns = 1024;
};
struct JsonField
{
  std::string name;
  Field value;
  Type type = Type::NULL_VALUE;
};

// Flat newline-delimited objects only. JSON strings retain string semantics
// except valid ISO dates/times; JSON numeric tokens retain their original text.
class JsonReader
{
public:
  JsonReader(std::istream &input, const JsonOptions &options) : input_(input), options_(options) {}
  Status next(std::vector<JsonField> &row);
  const Diagnostic &diagnostic() const { return diagnostic_; }
private:
  Status fail(Status status, const char *message);
  std::istream &input_;
  JsonOptions options_;
  Diagnostic diagnostic_;
  bool failed_ = false;
  uint64_t next_line_ = 1;
};

Status infer_json_schema(std::istream &input, const JsonOptions &options,
                         CsvSchema &schema, Diagnostic &diagnostic);
Status project_json_row(const std::vector<JsonField> &object, const CsvSchema &schema,
                         std::vector<Field> &row);
} // namespace oceanbase::sql::file_scan
