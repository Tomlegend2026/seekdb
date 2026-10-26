/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_json.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace oceanbase::sql::file_scan;

TEST(FileScanJson, PreservesJsonScalarTypesAndNumericLexemes)
{
  std::istringstream input(R"({"i":9223372036854775807,"overflow":9223372036854775808,"n":1.25e2,"b":true,"s":"123","empty":"","null":null,"date":"2024-02-29","dt":"2024-02-29 23:59:59"})");
  JsonReader reader(input, {});
  std::vector<JsonField> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  ASSERT_EQ(9U, row.size());
  EXPECT_EQ(Type::BIGINT, row[0].type);
  EXPECT_EQ(Type::VARCHAR, row[1].type);
  EXPECT_EQ("9223372036854775808", row[1].value.text);
  EXPECT_EQ(Type::DOUBLE, row[2].type);
  EXPECT_EQ("1.25e2", row[2].value.text);
  EXPECT_EQ(Type::BOOLEAN, row[3].type);
  EXPECT_EQ(Type::VARCHAR, row[4].type);
  EXPECT_EQ(Type::VARCHAR, row[5].type);
  EXPECT_FALSE(row[5].value.is_null);
  EXPECT_TRUE(row[6].value.is_null);
  EXPECT_EQ(Type::DATE, row[7].type);
  EXPECT_EQ(Type::DATETIME, row[8].type);
  EXPECT_EQ(Status::END, reader.next(row));
}

TEST(FileScanJson, UnionAndMissingFieldsAreInferredAcrossTheEntireFile)
{
  std::istringstream input("{\"id\":1,\"score\":null}\n{}\n{\"id\":2,\"score\":1.5,\"late\":true}\n");
  CsvSchema schema;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, infer_json_schema(input, {}, schema, diagnostic));
  EXPECT_EQ((std::vector<std::string>{"id", "score", "late"}), schema.names);
  EXPECT_EQ((std::vector<Type>{Type::BIGINT, Type::DOUBLE, Type::BOOLEAN}), schema.types);
  EXPECT_EQ(3U, schema.row_count);
  std::istringstream scan("{\"id\":3}");
  JsonReader reader(scan, {});
  std::vector<JsonField> object;
  std::vector<Field> row;
  ASSERT_EQ(Status::OK, reader.next(object));
  ASSERT_EQ(Status::OK, project_json_row(object, schema, row));
  ASSERT_EQ(3U, row.size());
  EXPECT_EQ("3", row[0].text);
  EXPECT_TRUE(row[1].is_null);
  EXPECT_TRUE(row[2].is_null);
}

TEST(FileScanJson, WideningAndAllNullFieldsDoNotDependOnRowOrder)
{
  for (const std::string text : {
      "{\"a\":1,\"b\":null}\n{\"a\":1.5}\n{\"a\":\"str\"}",
      "{\"a\":\"str\",\"b\":null}\n{\"a\":1.5}\n{\"a\":1}"}) {
    std::istringstream input(text);
    CsvSchema schema;
    Diagnostic diagnostic;
    ASSERT_EQ(Status::OK, infer_json_schema(input, {}, schema, diagnostic));
    EXPECT_EQ((std::vector<Type>{Type::VARCHAR, Type::VARCHAR}), schema.types);
  }
}

TEST(FileScanJson, HandlesBomBlankLinesCrlfUnicodeAndEscapedNewlines)
{
  std::istringstream input("\xef\xbb\xbf \r\n\n{\"name\":\"\\u4f60\\ud83d\\ude00\\n\"}\r\n");
  JsonReader reader(input, {});
  std::vector<JsonField> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  ASSERT_EQ(1U, row.size());
  EXPECT_EQ("\xe4\xbd\xa0\xf0\x9f\x98\x80\n", row[0].value.text);
  EXPECT_EQ(3U, reader.diagnostic().line);
  EXPECT_EQ(1U, reader.diagnostic().record);
  EXPECT_EQ(Status::END, reader.next(row));
}

TEST(FileScanJson, RejectsNestedValuesScalarRootsDuplicateAndAmbiguousKeys)
{
  for (const auto *text : {"[]", "1", "null", "{\"a\":[]}", "{\"a\":{}}",
      "{\"a\":1,\"a\":2}", "{\"a\":1,\"A\":2}", "{\"\":1}",
      "{\"a\\u0000b\":1}", "{\"a\":1} {\"b\":2}", "{\"a\":NaN}", "{\"a\":01}"}) {
    std::istringstream input(text);
    JsonReader reader(input, {});
    std::vector<JsonField> row;
    EXPECT_EQ(Status::INVALID, reader.next(row)) << text;
    EXPECT_TRUE(row.empty());
  }
}

TEST(FileScanJson, RejectsInvalidUtf8AndUnpairedSurrogates)
{
  for (const std::string &text : {std::string("{\"a\":\"\xc0\xaf\"}"), std::string("{\"a\":\"\\ud800\"}")}) {
    std::istringstream input(text);
    JsonReader reader(input, {});
    std::vector<JsonField> row;
    EXPECT_EQ(Status::INVALID, reader.next(row));
  }
}

TEST(FileScanJson, SchemaLimitsApplyToUnionNotJustIndividualObjects)
{
  std::istringstream input("{\"a\":1}\n{\"b\":2}\n{\"c\":3}");
  JsonOptions options;
  options.max_columns = 2;
  CsvSchema schema;
  schema.names = {"unchanged"};
  Diagnostic diagnostic;
  EXPECT_EQ(Status::LIMIT, infer_json_schema(input, options, schema, diagnostic));
  EXPECT_EQ((std::vector<std::string>{"unchanged"}), schema.names);
  EXPECT_EQ(3U, diagnostic.line);
}

TEST(FileScanJson, BadLateRecordDoesNotPublishPartialSchema)
{
  std::istringstream input("{\"a\":1}\n\n{\"a\":}");
  CsvSchema schema;
  schema.names = {"unchanged"};
  Diagnostic diagnostic;
  EXPECT_EQ(Status::INVALID, infer_json_schema(input, {}, schema, diagnostic));
  EXPECT_EQ((std::vector<std::string>{"unchanged"}), schema.names);
  EXPECT_EQ(3U, diagnostic.line);
  EXPECT_EQ(2U, diagnostic.record);
}

TEST(FileScanJson, BoundsRecordSizeAndReportsIoErrors)
{
  std::istringstream input("{\"a\":1}");
  JsonOptions options;
  options.max_record_bytes = 3;
  JsonReader reader(input, options);
  std::vector<JsonField> row;
  EXPECT_EQ(Status::LIMIT, reader.next(row));
  std::istringstream broken("{}");
  broken.setstate(std::ios::badbit);
  JsonReader bad(broken, {});
  EXPECT_EQ(Status::IO_ERROR, bad.next(row));
}

TEST(FileScanJson, EmptyInputAndObjectsDoNotCreateZeroColumnTables)
{
  for (const auto *text : {"", "\n  \r\n", "{}\n{}"}) {
    std::istringstream input(text);
    CsvSchema schema;
    Diagnostic diagnostic;
    EXPECT_EQ(Status::INVALID, infer_json_schema(input, {}, schema, diagnostic));
  }
}
