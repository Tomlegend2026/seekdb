/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_json.h"

#include <rapidjson/reader.h>
#include <rapidjson/memorystream.h>
#include <unordered_map>
#include <unordered_set>

namespace oceanbase::sql::file_scan
{
namespace
{
std::string folded(std::string value)
{
  for (char &byte : value) { if (byte >= 'A' && byte <= 'Z') { byte += 'a' - 'A'; } }
  return value;
}

class FlatObjectHandler : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, FlatObjectHandler>
{
public:
  FlatObjectHandler(std::vector<JsonField> &row, uint64_t max_columns)
      : row_(row), max_columns_(max_columns) {}
  bool StartObject() { if (started_) { return false; } started_ = true; return true; }
  bool EndObject(rapidjson::SizeType) { complete_ = true; return !has_key_; }
  bool StartArray() { return false; }
  bool Key(const char *text, rapidjson::SizeType size, bool)
  {
    std::string key(text, size);
    if (key.empty() || key.size() > 256 || key.find('\0') != std::string::npos || has_key_ || row_.size() >= max_columns_
        || !keys_.insert(folded(key)).second) { return false; }
    name_ = std::move(key);
    has_key_ = true;
    return true;
  }
  bool Null() { return append({"", true}, Type::NULL_VALUE); }
  bool Bool(bool value) { return append({value ? "true" : "false", false}, Type::BOOLEAN); }
  bool RawNumber(const char *text, rapidjson::SizeType size, bool)
  {
    Field field{std::string(text, size), false};
    const Type type = infer_type(field);
    return append(std::move(field), type);
  }
  bool String(const char *text, rapidjson::SizeType size, bool)
  {
    Field field{std::string(text, size), false};
    const Type inferred = infer_type(field);
    return append(std::move(field), inferred == Type::DATE || inferred == Type::DATETIME ? inferred : Type::VARCHAR);
  }
  bool complete() const { return started_ && complete_; }
private:
  bool append(Field field, Type type)
  {
    if (!started_ || complete_ || !has_key_) { return false; }
    row_.push_back({std::move(name_), std::move(field), type});
    has_key_ = false;
    return true;
  }
  std::vector<JsonField> &row_;
  uint64_t max_columns_;
  std::string name_;
  std::unordered_set<std::string> keys_;
  bool started_ = false;
  bool has_key_ = false;
  bool complete_ = false;
};
} // namespace

Status JsonReader::fail(Status status, const char *message)
{
  failed_ = true;
  diagnostic_.message = message;
  return status;
}

Status JsonReader::next(std::vector<JsonField> &row)
{
  row.clear();
  if (failed_) { return Status::INVALID; }
  std::string line;
  while (true) {
    diagnostic_.line = next_line_;
    line.clear();
    bool end = false;
    while (true) {
      const int byte = input_.get();
      if (byte == std::char_traits<char>::eof()) {
        if (input_.bad() || (input_.fail() && !input_.eof())) {
          return fail(Status::IO_ERROR, "Cannot read JSONL input");
        }
        end = true;
        break;
      }
      if (byte == '\n') { ++next_line_; break; }
      if (line.size() >= options_.max_record_bytes) { return fail(Status::LIMIT, "JSONL record byte limit exceeded"); }
      line.push_back(static_cast<char>(byte));
    }
    if (diagnostic_.line == 1 && line.compare(0, 3, "\xef\xbb\xbf") == 0) { line.erase(0, 3); }
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
      if (end) { return Status::END; }
      continue;
    }
    ++diagnostic_.record;
    rapidjson::MemoryStream stream(line.data(), line.size());
    rapidjson::Reader reader;
    std::vector<JsonField> candidate;
    FlatObjectHandler handler(candidate, options_.max_columns);
    constexpr unsigned flags = rapidjson::kParseNumbersAsStringsFlag | rapidjson::kParseValidateEncodingFlag;
    if (line.find('\0') != std::string::npos || !reader.Parse<flags>(stream, handler) || !handler.complete()) {
      return fail(Status::INVALID, "JSONL requires a valid flat object with unique nonempty column names");
    }
    row = std::move(candidate);
    return Status::OK;
  }
}

Status infer_json_schema(std::istream &input, const JsonOptions &options,
                         CsvSchema &schema, Diagnostic &diagnostic)
{
  JsonReader reader(input, options);
  CsvSchema candidate;
  std::unordered_map<std::string, size_t> indices;
  std::vector<JsonField> row;
  std::vector<uint64_t> nonnull_counts;
  Status status;
  while ((status = reader.next(row)) == Status::OK) {
    ++candidate.row_count;
    for (const auto &field : row) {
      const std::string key = folded(field.name);
      auto position = indices.find(key);
      if (position == indices.end()) {
        if (candidate.names.size() >= options.max_columns) {
          diagnostic = reader.diagnostic();
          diagnostic.message = "JSONL schema column limit exceeded";
          return Status::LIMIT;
        }
        position = indices.emplace(key, candidate.names.size()).first;
        candidate.names.push_back(field.name);
        candidate.types.push_back(Type::NULL_VALUE);
        nonnull_counts.push_back(0);
      }
      Type &type = candidate.types[position->second];
      type = widen(type, field.type);
      if (!field.value.is_null) { ++nonnull_counts[position->second]; }
    }
  }
  if (status != Status::END) { diagnostic = reader.diagnostic(); return status; }
  if (candidate.names.empty()) {
    diagnostic = reader.diagnostic();
    diagnostic.message = "Cannot infer columns from JSONL without any fields";
    return Status::INVALID;
  }
  for (Type &type : candidate.types) { type = result_type(type); }
  for (const auto count : nonnull_counts) {
    candidate.nullable.push_back(count < candidate.row_count);
  }
  schema = std::move(candidate);
  diagnostic = {};
  return Status::OK;
}

Status project_json_row(const std::vector<JsonField> &object, const CsvSchema &schema,
                         std::vector<Field> &row)
{
  std::unordered_map<std::string, size_t> indices;
  for (size_t i = 0; i < schema.names.size(); ++i) { indices.emplace(folded(schema.names[i]), i); }
  std::vector<Field> candidate(schema.names.size(), {"", true});
  for (const auto &field : object) {
    const auto found = indices.find(folded(field.name));
    if (found == indices.end()) { return Status::INVALID; }
    candidate[found->second] = field.value;
  }
  row = std::move(candidate);
  return Status::OK;
}
} // namespace oceanbase::sql::file_scan
