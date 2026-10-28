/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <sstream>
#include "sql/engine/basic/file_scan_utils.h"

using namespace oceanbase::sql::file_scan;

class GeneratedCsvBuffer : public std::streambuf
{
public:
  explicit GeneratedCsvBuffer(uint64_t records) : remaining_(records) {}
protected:
  int_type underflow() override
  {
    if (remaining_ == 0) { return traits_type::eof(); }
    --remaining_;
    setg(record_, record_, record_ + sizeof(record_) - 1);
    return traits_type::to_int_type(*gptr());
  }
private:
  uint64_t remaining_;
  char record_[5] = "1,x\n";
};

TEST(FileSqlLocalCsv, scans_non_seekable_input_without_retaining_old_records)
{
  GeneratedCsvBuffer buffer(100000);
  std::istream input(&buffer);
  CsvReader reader(input, {});
  std::vector<Field> row;
  for (uint64_t i = 0; i < 100000; ++i) {
    ASSERT_EQ(Status::OK, reader.next(row));
    ASSERT_EQ(2u, row.size());
    ASSERT_EQ("1", row[0].text);
    ASSERT_EQ("x", row[1].text);
  }
  EXPECT_EQ(Status::END, reader.next(row));
}

TEST(FileSqlLocalCsv, rejects_invalid_utf8_and_preserves_four_byte_characters)
{
  for (const char *data : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8", "\x80"}) {
    std::istringstream input(data);
    CsvReader reader(input, {});
    std::vector<Field> row;
    EXPECT_EQ(Status::INVALID, reader.next(row));
  }
  std::istringstream input("\xf0\x9f\x98\x80");
  CsvReader reader(input, {});
  std::vector<Field> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  ASSERT_EQ(1u, row.size());
  EXPECT_EQ("\xf0\x9f\x98\x80", row[0].text);
}

TEST(FileSqlLocalCsv, bom_quotes_multiline_and_crlf)
{
  std::istringstream input("\xef\xbb\xbf\"名,称\",note,empty\r\n"
      "\"数据库\",\"line1\r\nline2 \"\"quoted\"\"\",\"\"\r\n");
  CsvReader reader(input, {});
  std::vector<Field> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  ASSERT_EQ(3u, row.size());
  EXPECT_EQ("名,称", row[0].text);
  ASSERT_EQ(Status::OK, reader.next(row));
  ASSERT_EQ(3u, row.size());
  EXPECT_EQ("数据库", row[0].text);
  EXPECT_EQ("line1\r\nline2 \"quoted\"", row[1].text);
  EXPECT_EQ("", row[2].text);
  EXPECT_FALSE(row[2].is_null);
  EXPECT_EQ(Status::END, reader.next(row));
}

TEST(FileSqlLocalCsv, null_is_distinct_from_quoted_text_and_trailing_field)
{
  std::istringstream input("NULL,\"NULL\",,\"\",\nlast,");
  CsvOptions options;
  options.null_text = "NULL";
  CsvReader reader(input, options);
  std::vector<Field> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  ASSERT_EQ(5u, row.size());
  EXPECT_TRUE(row[0].is_null);
  EXPECT_FALSE(row[1].is_null);
  EXPECT_FALSE(row[2].is_null);
  EXPECT_FALSE(row[3].is_null);
  ASSERT_EQ(Status::OK, reader.next(row));
  ASSERT_EQ(2u, row.size());
  EXPECT_EQ("last", row[0].text);
  EXPECT_EQ("", row[1].text);
  EXPECT_EQ(Status::END, reader.next(row));
}

TEST(FileSqlLocalCsv, configurable_delimiter_quote_and_escape)
{
  std::istringstream input("'a;b';'it\\'s';'a\\\\b'\n");
  CsvOptions options;
  options.delimiter = ';';
  options.quote = '\'';
  options.escape = '\\';
  CsvReader reader(input, options);
  std::vector<Field> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  ASSERT_EQ(3u, row.size());
  EXPECT_EQ("a;b", row[0].text);
  EXPECT_EQ("it's", row[1].text);
  EXPECT_EQ("a\\b", row[2].text);
}

TEST(FileSqlLocalCsv, invalid_records_stop_with_location)
{
  for (const std::string data : {"good,row\n\"unclosed", "good,row\na\"b,c",
      "good,row\n\"x\"junk,y", "good,row\nbare\rnext"}) {
    std::istringstream input(data);
    CsvReader reader(input, {});
    std::vector<Field> row;
    ASSERT_EQ(Status::OK, reader.next(row));
    EXPECT_EQ(Status::INVALID, reader.next(row)) << data;
    EXPECT_EQ(2u, reader.diagnostic().record);
    EXPECT_GE(reader.diagnostic().line, 2u);
    EXPECT_FALSE(reader.diagnostic().message.empty());
    EXPECT_EQ(Status::INVALID, reader.next(row));
  }
}

TEST(FileSqlLocalCsv, limits_include_delimiters_quotes_and_newlines)
{
  CsvOptions options;
  options.max_record_bytes = 5;
  std::istringstream input("\"ab\"\r\n");
  CsvReader reader(input, options);
  std::vector<Field> row;
  EXPECT_EQ(Status::LIMIT, reader.next(row));
  options.max_columns = 2;
  std::istringstream columns(",,");
  CsvReader wide(columns, options);
  EXPECT_EQ(Status::LIMIT, wide.next(row));
}

TEST(FileSqlLocalCsv, empty_short_and_non_bom_utf8_inputs)
{
  for (const std::string data : {"", "a", "ab", "abc", "\xef\xbc\xa1"}) {
    std::istringstream input(data);
    CsvReader reader(input, {});
    std::vector<Field> row;
    if (data.empty()) {
      EXPECT_EQ(Status::END, reader.next(row));
    } else {
      ASSERT_EQ(Status::OK, reader.next(row));
      ASSERT_EQ(1u, row.size());
      EXPECT_EQ(data, row[0].text);
      EXPECT_EQ(Status::END, reader.next(row));
    }
  }
}

TEST(FileSqlLocalCsv, input_io_failure_and_nul_are_not_eof)
{
  std::istringstream failed("data");
  failed.setstate(std::ios::badbit);
  CsvReader reader(failed, {});
  std::vector<Field> row;
  EXPECT_EQ(Status::IO_ERROR, reader.next(row));
  std::istringstream input(std::string("a\0b", 3));
  CsvReader nul(input, {});
  EXPECT_EQ(Status::INVALID, nul.next(row));
}

TEST(FileSqlLocalSchema, exact_numeric_boundaries)
{
  for (const char *text : {"0", "+42", "-9223372036854775808", "9223372036854775807"}) {
    EXPECT_EQ(Type::BIGINT, infer_type(Field{text, false})) << text;
  }
  for (const char *text : {"1.5", "-1e2", "+1.2e-3"}) {
    EXPECT_EQ(Type::DOUBLE, infer_type(Field{text, false})) << text;
  }
  for (const char *text : {"9223372036854775808", "-9223372036854775809", "nan", "inf",
      "1e309", "1x", " 1", "1 ", "+-1", "++1", ""}) {
    EXPECT_EQ(Type::VARCHAR, infer_type(Field{text, false})) << text;
  }
}

TEST(FileSqlLocalSchema, dates_require_real_calendar_and_time_values)
{
  EXPECT_EQ(Type::DATE, infer_type(Field{"2024-02-29", false}));
  EXPECT_EQ(Type::DATE, infer_type(Field{"2000-02-29", false}));
  for (const char *text : {"1900-02-29", "2026-02-29", "2026-04-31", "0000-01-01",
      "2026-00-10", "2026-13-01", "2026-01-00"}) {
    EXPECT_EQ(Type::VARCHAR, infer_type(Field{text, false})) << text;
  }
  for (const char *text : {"2026-10-26 23:59:59", "2026-10-26T00:00:00.123456"}) {
    EXPECT_EQ(Type::DATETIME, infer_type(Field{text, false})) << text;
  }
  for (const char *text : {"2026-10-26 24:00:00", "2026-10-26 12:60:00",
      "2026-10-26 12:00:60", "2026-10-26 12:00:00.", "2026-10-26 12:00:00.1234567"}) {
    EXPECT_EQ(Type::VARCHAR, infer_type(Field{text, false})) << text;
  }
}

TEST(FileSqlLocalSchema, widening_is_independent_of_record_order)
{
  const Type types[] = {Type::NULL_VALUE, Type::BIGINT, Type::DOUBLE, Type::BOOLEAN,
      Type::VARCHAR, Type::DATE, Type::DATETIME};
  for (Type left : types) {
    for (Type middle : types) {
      EXPECT_EQ(widen(left, middle), widen(middle, left));
      for (Type right : types) {
        EXPECT_EQ(widen(widen(left, middle), right), widen(left, widen(middle, right)));
      }
    }
  }
  EXPECT_EQ(Type::VARCHAR, result_type(Type::NULL_VALUE));
  EXPECT_EQ(Type::NULL_VALUE, infer_type(Field{"ignored", true}));
  EXPECT_EQ(Type::BOOLEAN, infer_type(Field{"TrUe", false}));
}

TEST(FileSqlLocalSchema, duplicate_headers_do_not_shadow_literal_suffixes)
{
  const std::vector<Field> header = {{"id"}, {"ID"}, {"id__2"}, {""}, {"column4"}};
  EXPECT_EQ((std::vector<std::string>{"id", "ID__3", "id__2", "column4__2", "column4"}),
      column_names(header));
}

TEST(FileSqlLocalSchema, inference_reads_late_rows_and_widens_all_columns)
{
  std::istringstream input("n,day,empty,flag\n1,2024-02-29,,true\n"
      "2.5,2024-03-01 01:02:03,,FALSE\n");
  CsvSchema schema;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, infer_csv_schema(input, {}, schema, diagnostic));
  EXPECT_EQ(2u, schema.row_count);
  EXPECT_EQ((std::vector<Type>{Type::DOUBLE, Type::DATETIME, Type::VARCHAR, Type::BOOLEAN}), schema.types);
  EXPECT_EQ((std::vector<std::string>{"n", "day", "empty", "flag"}), schema.names);
  EXPECT_EQ((std::vector<bool>{false, false, true, false}), schema.nullable);
}

TEST(FileSqlLocalSchema, nullability_tracks_late_nulls_and_quoted_empty_values)
{
  std::istringstream input("id,text,late\n1,\"\",3\n2,\"\",\n");
  CsvSchema schema;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, infer_csv_schema(input, {}, schema, diagnostic));
  EXPECT_EQ((std::vector<bool>{false, false, true}), schema.nullable);
}

TEST(FileSqlLocalSchema, headerless_inference_includes_first_record)
{
  std::istringstream input("text,2\n1,3\n");
  CsvOptions options;
  options.header = false;
  CsvSchema schema;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, infer_csv_schema(input, options, schema, diagnostic));
  EXPECT_EQ(2u, schema.row_count);
  EXPECT_EQ((std::vector<Type>{Type::VARCHAR, Type::BIGINT}), schema.types);
  EXPECT_EQ((std::vector<std::string>{"column1", "column2"}), schema.names);
}

TEST(FileSqlLocalSchema, width_errors_do_not_publish_partial_schema)
{
  std::istringstream input("a,b\n1,2\n3\n");
  CsvSchema schema;
  Diagnostic diagnostic;
  EXPECT_EQ(Status::INVALID, infer_csv_schema(input, {}, schema, diagnostic));
  EXPECT_EQ(3U, diagnostic.line);
  EXPECT_TRUE(schema.names.empty());
  EXPECT_EQ(3u, diagnostic.record);
  EXPECT_NE(std::string::npos, diagnostic.message.find("width"));
}

TEST(FileSqlLocalSchema, empty_input_differs_from_header_only_input)
{
  std::istringstream empty;
  CsvSchema schema;
  Diagnostic diagnostic;
  EXPECT_EQ(Status::INVALID, infer_csv_schema(empty, {}, schema, diagnostic));
  std::istringstream header("id,name\n");
  ASSERT_EQ(Status::OK, infer_csv_schema(header, {}, schema, diagnostic));
  EXPECT_EQ(0u, schema.row_count);
  EXPECT_EQ((std::vector<Type>{Type::VARCHAR, Type::VARCHAR}), schema.types);
  EXPECT_EQ((std::vector<bool>{true, true}), schema.nullable);
}
