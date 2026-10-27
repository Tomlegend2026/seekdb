/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_cursor.h"
#include "file_scan_glob.h"

namespace oceanbase::sql::file_scan
{
Status FileCursor::prepare(const FilePolicy &policy, const QueryOptions &options)
{
  csv_.reset();
  json_.reset();
  parquet_.reset();
  file_.close();
  prepared_ = false;
  fingerprint_.clear();
  options_ = options;
  policy_ = policy;
  files_.clear();
  file_position_ = 0;
  if (options.glob) {
    const auto status = discover_files(policy, options.path, files_, diagnostic_);
    if (status != Status::OK) { return status; }
    schema_.names = {"file"};
    schema_.types = {Type::VARCHAR};
    schema_.nullable = {false};
    schema_.row_count = files_.size();
    fingerprint_ = "glob:";
    for (const auto &file : files_) { fingerprint_ += std::to_string(file.size()) + ":" + file; }
    prepared_ = true;
    return Status::OK;
  }
  auto status = file_.open(policy, options.path);
  if (status != Status::OK) { diagnostic_ = file_.diagnostic(); return status; }
  if (options.parquet) {
    parquet_ = std::make_unique<ParquetReader>(file_, ParquetOptions{});
    status = parquet_->prepare(schema_, diagnostic_);
    if (status != Status::OK) { return status; }
    if ((status = file_.validate()) != Status::OK) { diagnostic_ = file_.diagnostic(); return status; }
    fingerprint_ = file_.fingerprint();
    prepared_ = true;
    return rewind();
  }
  status = options.json ? infer_json_schema(file_.stream(), {}, schema_, diagnostic_)
                        : infer_csv_schema(file_.stream(), options.csv, schema_, diagnostic_);
  if (status != Status::OK) { return status; }
  if ((status = file_.validate()) != Status::OK) { diagnostic_ = file_.diagnostic(); return status; }
  fingerprint_ = file_.fingerprint();
  prepared_ = true;
  return rewind();
}

Status FileCursor::rewind()
{
  if (!prepared_) { diagnostic_.message = "File cursor has no valid schema"; return Status::INVALID; }
  if (options_.glob) {
    std::vector<std::string> current;
    const auto status = discover_files(policy_, options_.path, current, diagnostic_);
    if (status != Status::OK) { prepared_ = false; return status; }
    if (current != files_) {
      prepared_ = false;
      diagnostic_.message = "Glob listing changed before rescan";
      return Status::IO_ERROR;
    }
    file_position_ = 0;
    return Status::OK;
  }
  if (options_.parquet) {
    const auto status = parquet_->rewind(diagnostic_);
    if (status != Status::OK) { return status; }
    return Status::OK;
  }
  auto status = file_.rewind();
  if (status != Status::OK) { diagnostic_ = file_.diagnostic(); return status; }
  csv_.reset();
  json_.reset();
  if (options_.json) {
    json_ = std::make_unique<JsonReader>(file_.stream(), JsonOptions{});
  } else {
    csv_ = std::make_unique<CsvReader>(file_.stream(), options_.csv);
    if (options_.csv.header) {
      std::vector<Field> header;
      status = csv_->next(header);
      if (status != Status::OK) { diagnostic_ = csv_->diagnostic(); return status; }
    }
  }
  diagnostic_ = {};
  return Status::OK;
}

Status FileCursor::next(std::vector<Field> &row)
{
  if (!prepared_) { row.clear(); diagnostic_.message = "File cursor has no valid schema"; return Status::INVALID; }
  if (options_.glob) {
    row.clear();
    if (file_position_ >= files_.size()) { return Status::END; }
    row.push_back({files_[file_position_++], false});
    return Status::OK;
  }
  auto status = file_.validate();
  if (status != Status::OK) { row.clear(); diagnostic_ = file_.diagnostic(); return status; }
  if (parquet_) {
    status = parquet_->next(row);
    diagnostic_ = parquet_->diagnostic();
  } else if (json_) {
    std::vector<JsonField> object;
    status = json_->next(object);
    if (status == Status::OK) { status = project_json_row(object, schema_, row); }
    diagnostic_ = json_->diagnostic();
  } else if (csv_) {
    status = csv_->next(row);
    diagnostic_ = csv_->diagnostic();
  } else {
    diagnostic_.message = "File cursor is not prepared";
    return Status::INVALID;
  }
  if (status == Status::OK && row.size() != schema_.names.size()) {
    diagnostic_.message = "File row width differs from its inferred schema";
    status = Status::INVALID;
  }
  if (status == Status::OK || status == Status::END) {
    const auto validated = file_.validate();
    if (validated != Status::OK) { status = validated; diagnostic_ = file_.diagnostic(); }
  }
  if (status != Status::OK) { row.clear(); }
  return status;
}
} // namespace oceanbase::sql::file_scan
