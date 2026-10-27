/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "file_scan_file.h"
#include "file_scan_json.h"
#include <memory>

namespace oceanbase::sql::file_scan
{
struct QueryOptions
{
  std::string path;
  bool json = false;
  bool glob = false;
  CsvOptions csv;
};

class FileCursor
{
public:
  Status prepare(const FilePolicy &policy, const QueryOptions &options);
  Status rewind();
  Status next(std::vector<Field> &row);
  const CsvSchema &schema() const { return schema_; }
  const std::string &fingerprint() const { return fingerprint_; }
  const Diagnostic &diagnostic() const { return diagnostic_; }
private:
  SecureFile file_;
  QueryOptions options_;
  FilePolicy policy_;
  std::vector<std::string> files_;
  size_t file_position_ = 0;
  CsvSchema schema_;
  std::string fingerprint_;
  std::unique_ptr<CsvReader> csv_;
  std::unique_ptr<JsonReader> json_;
  Diagnostic diagnostic_;
  bool prepared_ = false;
};
} // namespace oceanbase::sql::file_scan
