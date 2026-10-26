/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_cursor.h"

namespace oceanbase::sql::file_scan
{
Status FileCursor::prepare(const FilePolicy &policy, const QueryOptions &options)
{
  csv_.reset();
  json_.reset();
  prepared_ = false;
  fingerprint_.clear();
  options_ = options;
  auto status = file_.open(policy, options.path);
  if (status != Status::OK) { diagnostic_ = file_.diagnostic(); return status; }
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
  auto status = file_.validate();
  if (status != Status::OK) { row.clear(); diagnostic_ = file_.diagnostic(); return status; }
  if (json_) {
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
