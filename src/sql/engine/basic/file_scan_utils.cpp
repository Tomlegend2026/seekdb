/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_utils.h"

#include <charconv>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace oceanbase::sql::file_scan
{
namespace
{
bool valid_utf8(const std::string &text)
{
  for (size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) { continue; }
    const size_t continuation = first >= 0xc2 && first <= 0xdf ? 1
        : first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
    if (continuation == 0 || text.size() - i < continuation) { return false; }
    const auto second = static_cast<unsigned char>(text[i]);
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0)
        || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) {
      return false;
    }
    for (size_t end = i + continuation; i < end; ++i) {
      const auto byte = static_cast<unsigned char>(text[i]);
      if (byte < 0x80 || byte > 0xbf) { return false; }
    }
  }
  return true;
}

std::string ascii_lower(std::string text)
{
  for (char &value : text) {
    if (value >= 'A' && value <= 'Z') {
      value += 'a' - 'A';
    }
  }
  return text;
}

bool digits(const std::string &text, size_t begin, size_t count)
{
  if (begin + count > text.size()) { return false; }
  for (size_t i = begin; i < begin + count; ++i) {
    if (text[i] < '0' || text[i] > '9') { return false; }
  }
  return true;
}

int decimal(const std::string &text, size_t begin, size_t count)
{
  int result = 0;
  for (size_t i = begin; i < begin + count; ++i) {
    result = result * 10 + text[i] - '0';
  }
  return result;
}

bool valid_date(const std::string &text)
{
  if (text.size() < 10 || text[4] != '-' || text[7] != '-'
      || !digits(text, 0, 4) || !digits(text, 5, 2) || !digits(text, 8, 2)) {
    return false;
  }
  const int year = decimal(text, 0, 4);
  const int month = decimal(text, 5, 2);
  const int day = decimal(text, 8, 2);
  const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return year >= 1 && month >= 1 && month <= 12 && day >= 1
      && day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

bool valid_datetime(const std::string &text)
{
  if (text.size() < 19 || !valid_date(text) || (text[10] != ' ' && text[10] != 'T')
      || text[13] != ':' || text[16] != ':' || !digits(text, 11, 2)
      || !digits(text, 14, 2) || !digits(text, 17, 2)
      || decimal(text, 11, 2) > 23 || decimal(text, 14, 2) > 59
      || decimal(text, 17, 2) > 59) {
    return false;
  }
  return text.size() == 19 || (text.size() >= 21 && text.size() <= 26
      && text[19] == '.' && digits(text, 20, text.size() - 20));
}
} // namespace

CsvReader::CsvReader(std::istream &input, const CsvOptions &options)
  : input_(input), options_(options)
{}

int CsvReader::read_byte()
{
  if (!initialized_) {
    initialized_ = true;
    // Do not require a seekable input just to skip a UTF-8 BOM.
    for (int i = 0; i < 3; ++i) {
      const int byte = input_.get();
      if (byte == std::char_traits<char>::eof()) { break; }
      prefix_.push_back(static_cast<char>(byte));
    }
    if (prefix_ == "\xef\xbb\xbf") { prefix_.clear(); }
  }
  return prefix_position_ < prefix_.size()
      ? static_cast<unsigned char>(prefix_[prefix_position_++]) : input_.get();
}

Status CsvReader::fail(Status status, const char *message)
{
  failed_ = true;
  diagnostic_.message = message;
  return status;
}

Status CsvReader::finish_field(std::vector<Field> &row, std::string &text, bool quoted)
{
  if (!valid_utf8(text)) { return fail(Status::INVALID, "Invalid UTF-8 in CSV field"); }
  if (row.size() >= options_.max_columns) {
    return fail(Status::LIMIT, "CSV column limit exceeded");
  }
  const bool is_null = !quoted && text == options_.null_text;
  row.push_back(Field{std::move(text), is_null});
  text.clear();
  return Status::OK;
}

Status CsvReader::next(std::vector<Field> &row)
{
  row.clear();
  if (failed_) { return Status::INVALID; }
  if (options_.max_record_bytes == 0 || options_.max_columns == 0
      || options_.delimiter == options_.quote || options_.delimiter == '\r'
      || options_.delimiter == '\n' || options_.delimiter == '\0'
      || options_.quote == '\r' || options_.quote == '\n' || options_.quote == '\0'
      || options_.escape == '\r' || options_.escape == '\n' || options_.escape == '\0') {
    return fail(Status::INVALID, "Invalid CSV options");
  }
  enum class State { START, UNQUOTED, QUOTED, AFTER_QUOTE, ESCAPED };
  State state = State::START;
  std::string text;
  bool quoted = false;
  bool any = false;
  uint64_t bytes = 0;
  ++diagnostic_.record;
  while (true) {
    const int byte = read_byte();
    if (byte == std::char_traits<char>::eof()) {
      if (input_.bad() || (!input_.eof() && input_.fail())) {
        return fail(Status::IO_ERROR, "CSV input read failed");
      }
      if (state == State::QUOTED || state == State::ESCAPED) {
        return fail(Status::INVALID, "Unterminated quoted CSV field");
      }
      return !any ? Status::END : finish_field(row, text, quoted);
    }
    any = true;
    if (++bytes > options_.max_record_bytes) {
      return fail(Status::LIMIT, "CSV record byte limit exceeded");
    }
    const char value = static_cast<char>(byte);
    if (value == '\0') { return fail(Status::INVALID, "NUL byte in CSV input"); }
    if (value == '\n') { ++diagnostic_.line; }
    if (state == State::ESCAPED) {
      if (value != options_.quote && value != options_.escape) {
        return fail(Status::INVALID, "Invalid CSV escape sequence");
      }
      text.push_back(value);
      state = State::QUOTED;
    } else if (state == State::QUOTED) {
      if (value == options_.quote) {
        state = State::AFTER_QUOTE;
      } else if (value == options_.escape) {
        state = State::ESCAPED;
      } else {
        text.push_back(value);
      }
    } else if (state == State::AFTER_QUOTE && value == options_.quote
        && options_.escape == options_.quote) {
      text.push_back(value);
      state = State::QUOTED;
    } else if (value == options_.delimiter) {
      const Status status = finish_field(row, text, quoted);
      if (status != Status::OK) { return status; }
      state = State::START;
      quoted = false;
    } else if (value == '\n' || value == '\r') {
      if (value == '\r') {
        const int next = read_byte();
        if (next != '\n') {
          return fail(Status::INVALID, "Bare CR outside a quoted CSV field");
        }
        ++diagnostic_.line;
        if (++bytes > options_.max_record_bytes) {
          return fail(Status::LIMIT, "CSV record byte limit exceeded");
        }
      }
      return finish_field(row, text, quoted);
    } else if (state == State::AFTER_QUOTE) {
      return fail(Status::INVALID, "Unexpected character after closing CSV quote");
    } else if (value == options_.quote) {
      if (state != State::START) {
        return fail(Status::INVALID, "Quote inside an unquoted CSV field");
      }
      state = State::QUOTED;
      quoted = true;
    } else {
      text.push_back(value);
      state = State::UNQUOTED;
    }
  }
}

Type infer_type(const Field &field)
{
  if (field.is_null) { return Type::NULL_VALUE; }
  const std::string &text = field.text;
  if (text.empty()) { return Type::VARCHAR; }
  if (ascii_lower(text) == "true" || ascii_lower(text) == "false") { return Type::BOOLEAN; }
  if (text.size() == 10 && valid_date(text)) { return Type::DATE; }
  if (valid_datetime(text)) { return Type::DATETIME; }
  const char *begin = text.data();
  const char *end = begin + text.size();
  if (*begin == '+') { ++begin; }
  if (begin == end || *begin == '+' || (text[0] == '+' && *begin == '-')) { return Type::VARCHAR; }
  int64_t integer = 0;
  const auto integral = std::from_chars(begin, end, integer);
  if (integral.ec == std::errc{} && integral.ptr == end) { return Type::BIGINT; }
  // Preserve out-of-range integer text rather than silently rounding to double.
  const size_t sign = text[0] == '-' || text[0] == '+' ? 1 : 0;
  if (digits(text, sign, text.size() - sign)) { return Type::VARCHAR; }
  double number = 0;
  const auto floating = std::from_chars(begin, end, number, std::chars_format::general);
  return floating.ec == std::errc{} && floating.ptr == end && std::isfinite(number)
      ? Type::DOUBLE : Type::VARCHAR;
}

Type widen(Type left, Type right)
{
  if (left == Type::NULL_VALUE) { return right; }
  if (right == Type::NULL_VALUE || left == right) { return left; }
  if ((left == Type::BIGINT && right == Type::DOUBLE)
      || (right == Type::BIGINT && left == Type::DOUBLE)) { return Type::DOUBLE; }
  if ((left == Type::DATE && right == Type::DATETIME)
      || (right == Type::DATE && left == Type::DATETIME)) { return Type::DATETIME; }
  return Type::VARCHAR;
}

Type result_type(Type accumulated)
{
  return accumulated == Type::NULL_VALUE ? Type::VARCHAR : accumulated;
}

Status infer_csv_schema(std::istream &input, const CsvOptions &options,
                        CsvSchema &schema, Diagnostic &diagnostic)
{
  CsvReader reader(input, options);
  CsvSchema candidate;
  std::vector<Field> row;
  Status status = reader.next(row);
  if (status == Status::END) {
    diagnostic = reader.diagnostic();
    diagnostic.message = "Cannot infer columns from an empty CSV file";
    return Status::INVALID;
  } else if (status != Status::OK) {
    diagnostic = reader.diagnostic();
    return status;
  }
  if (options.header) {
    candidate.names = column_names(row);
  } else {
    for (size_t i = 0; i < row.size(); ++i) {
      candidate.names.push_back("column" + std::to_string(i + 1));
    }
  }
  candidate.types.resize(row.size(), Type::NULL_VALUE);
  if (options.header) { status = reader.next(row); }
  while (status == Status::OK) {
    if (row.size() != candidate.names.size()) {
      diagnostic = reader.diagnostic();
      diagnostic.message = "CSV record width differs from inferred column count";
      return Status::INVALID;
    }
    for (size_t i = 0; i < row.size(); ++i) {
      candidate.types[i] = widen(candidate.types[i], infer_type(row[i]));
    }
    ++candidate.row_count;
    status = reader.next(row);
  }
  if (status != Status::END) {
    diagnostic = reader.diagnostic();
    return status;
  }
  for (Type &type : candidate.types) { type = result_type(type); }
  schema = std::move(candidate);
  diagnostic = {};
  return Status::OK;
}

std::vector<std::string> column_names(const std::vector<Field> &header)
{
  std::unordered_set<std::string> reserved;
  std::unordered_set<std::string> used;
  for (const auto &field : header) {
    if (!field.text.empty()) { reserved.insert(ascii_lower(field.text)); }
  }
  std::vector<std::string> names;
  for (size_t i = 0; i < header.size(); ++i) {
    const std::string base = header[i].text.empty() ? "column" + std::to_string(i + 1) : header[i].text;
    std::string candidate = base;
    uint64_t suffix = 2;
    if (used.count(ascii_lower(candidate))
        || (header[i].text.empty() && reserved.count(ascii_lower(candidate)))) {
      do {
        candidate = base + "__" + std::to_string(suffix++);
      } while (reserved.count(ascii_lower(candidate)) || used.count(ascii_lower(candidate)));
    }
    used.insert(ascii_lower(candidate));
    names.push_back(std::move(candidate));
  }
  return names;
}
} // namespace oceanbase::sql::file_scan
