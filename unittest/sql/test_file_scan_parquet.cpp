/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_cursor.h"
#include "sql/engine/basic/file_scan_parquet.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace oceanbase::sql::file_scan;
namespace fs = std::filesystem;

namespace
{
std::string decode_base64(const std::string &text)
{
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int value = 0;
  int bits = -8;
  for (char character : text) {
    if (character == '=') { break; }
    const size_t index = alphabet.find(character);
    if (index == std::string::npos) { continue; }
    value = (value << 6) + static_cast<int>(index);
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((value >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

// The official acceptance fixture, tools/deploy/mysql_test/test_suite/file_sql/
// data/simple.parquet.b64, decoded from base64.
const char *OFFICIAL_PARQUET =
    "UEFSMRUGFTwVQFwVDhUAFQ4VABUEFQARHDYAKAQGAAAAGAQAAAAAAAAAA38cbAAAAAABAAAAAgAA"
    "AAMAAAAEAAAABQAAAAYAAAAmlAEcFQIZJQYAGRgCYzEVAhYOFogBFowBJgg8NgAoBAYAAAAYBAAA"
    "AAAAAAAVBBksSARyb290FQIAFQIlAhgCYzEAFg4ZHBkcJpQBHBUCGSUGABkYAmMxFQIWDhaIARaM"
    "ASYIPDYAKAQGAAAAGAQAAAAAAAAAFowBFg4AGRwYDEFSUk9XOnNjaGVtYRi8AS8vLy8vNFFBQUFB"
    "UUFBQUFBQUFLQUE0QURBQUxBQVFBQ2dBQUFCUUFBQUFBQUFBQkJBQUtBQXdBQUFBSUFBUUFDZ0FB"
    "QUFnQUFBQUlBQUFBQUFBQUFBRUFBQUFVQUFBQUVBQVVBQkFBRGdBUEFBUUFBQUFJQUJBQUFBQVlB"
    "QUFBSUFBQUFBQUFBUUljQUFBQUNBQU1BQVFBQ3dBSUFBQUFJQUFBQUFBQUFBRUFBQUFBQWdBQUFH"
    "TXhBQUE9ABgsQXJyb3cyIC0gTmF0aXZlIFJ1c3QgaW1wbGVtZW50YXRpb24gb2YgQXJyb3cATgEA"
    "AFBBUjE=";

// Fixtures written by .runtime/parquet_write.py, which exercises the dictionary
// encoding, several row groups, the BOOLEAN/DOUBLE/DATE types and the padded
// bit-packed definition levels.
const char *DICTIONARY_PARQUET =
    "UEFSMRUEFRAVEEwVBBUAAAABAAAAAgAAABUAFSIVIiwVChUQFQYVBgAABgAAAAQBAgAEAQECAAQB"
    "AgAVAhksSARyb290FQIAFQIlAhgCaWQAFgIZHBkcJjIcFQIZJQYQGRgCaWQVABYKFm4WbiYyJggA"
    "ABZuFgoAAD4AAABQQVIx";
const char *ROW_GROUP_PARQUET =
    "UEFSMRUAFSAVICwVBBUAFQYVBgAAAQAAAAAAAAACAAAAAAAAABUAFRoVGiwVBBUAFQYVBgAABAAA"
    "AAIBAgABAAAAYRUAFRAVECwVAhUAFQYVBgAAAwAAAAAAAAAVABUWFRYsFQIVABUGFQYAAAIAAAAC"
    "AQEAAABjFQIZPEgEcm9vdBUEABUEJQAYAmlkABUMJQIYBG5hbWUAFggZLBksJggcFQQZJQYAGRgC"
    "aWQVABYEFkIWQiYIAAAmShwVDBklBgAZGARuYW1lFQAWBBY8FjwmSgAAFn4WBAAZLCaGARwVBBkl"
    "BgAZGAJpZBUAFgIWMhYyJoYBAAAmuAEcFQwZJQYAGRgEbmFtZRUAFgIWOBY4JrgBAAAWahYCAACk"
    "AAAAUEFSMQ==";
const char *TYPE_PARQUET =
    "UEFSMRUAFQIVAiwVBhUAFQYVBgAABRUAFTAVMCwVBhUAFQYVBgAAAAAAAAAA+D8AAAAAAAACQAAA"
    "AAAAAAhAFQAVIBUgLBUGFQAVBhUGAAAEAAAABAECAAAAAAABAAAAFQIZTEgEcm9vdBUGABUAJQAY"
    "BGZsYWcAFQolABgFcmF0aW8AFQIlAhgDZGF5JQwAFgYZHBk8JggcFQAZJQYAGRgEZmxhZxUAFgYW"
    "JBYkJggAACYsHBUKGSUGABkYBXJhdGlvFQAWBhZSFlImLAAAJn4cFQIZJQYAGRgDZGF5FQAWBhZC"
    "FkImfgAAFrgBFgYAAJEAAABQQVIx";
const char *BIT_PACKED_PARQUET =
    "UEFSMRUAFSwVLCwVDhUAFQYVBgAAAgAAAANVAQAAAAMAAAAFAAAABwAAABUCGSxIBHJvb3QVAgAV"
    "AiUCGAJpZAAWAhkcGRwmCBwVAhklBgAZGAJpZBUAFg4WThZOJggAABZOFg4AADwAAABQQVIx";

class ParquetReaderTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    char pattern[] = "/tmp/seekdb-parquet-XXXXXX";
    const char *directory = ::mkdtemp(pattern);
    ASSERT_NE(nullptr, directory);
    base = directory;
    fs::create_directories(base / "allowed");
    policy.enabled = true;
    policy.secure_root = (base / "allowed").string();
  }
  void TearDown() override { if (!base.empty()) { fs::remove_all(base); } }

  void write(const std::string &name, const std::string &payload)
  {
    std::ofstream output(base / "allowed" / name, std::ios::binary | std::ios::trunc);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ASSERT_TRUE(output.good());
  }

  // Runs the reader over a fixture and collects every row as text.
  Status collect(const std::string &name, const std::string &payload,
                 std::vector<std::vector<Field>> &rows, CsvSchema &schema)
  {
    write(name, payload);
    SecureFile file;
    Status status = file.open(policy, name);
    if (status != Status::OK) { return status; }
    ParquetReader reader(file, ParquetOptions{});
    Diagnostic diagnostic;
    status = reader.prepare(schema, diagnostic);
    if (status != Status::OK) { return status; }
    status = reader.rewind(diagnostic);
    if (status != Status::OK) { return status; }
    std::vector<Field> row;
    for (;;) {
      status = reader.next(row);
      if (status != Status::OK) { break; }
      rows.push_back(row);
    }
    return status;
  }

  fs::path base;
  FilePolicy policy;
};
} // namespace

TEST_F(ParquetReaderTest, ReadsTheOfficialFixture)
{
  std::vector<std::vector<Field>> rows;
  CsvSchema schema;
  ASSERT_EQ(Status::END, collect("simple.parquet", decode_base64(OFFICIAL_PARQUET), rows, schema));
  ASSERT_EQ(1U, schema.names.size());
  EXPECT_EQ("c1", schema.names[0]);
  EXPECT_EQ(Type::BIGINT, schema.types[0]);
  EXPECT_TRUE(schema.nullable[0]);
  ASSERT_EQ(7U, rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    ASSERT_EQ(1U, rows[i].size());
    EXPECT_FALSE(rows[i][0].is_null);
    EXPECT_EQ(std::to_string(i), rows[i][0].text);
  }
}

TEST_F(ParquetReaderTest, ReadsDictionaryEncodedPages)
{
  std::vector<std::vector<Field>> rows;
  CsvSchema schema;
  ASSERT_EQ(Status::END, collect("dictionary.parquet", decode_base64(DICTIONARY_PARQUET), rows, schema));
  ASSERT_EQ(5U, rows.size());
  EXPECT_EQ("1", rows[0][0].text);
  EXPECT_EQ("2", rows[1][0].text);
  EXPECT_TRUE(rows[2][0].is_null);
  EXPECT_EQ("2", rows[3][0].text);
  EXPECT_EQ("1", rows[4][0].text);
}

TEST_F(ParquetReaderTest, ReadsSeveralRowGroups)
{
  std::vector<std::vector<Field>> rows;
  CsvSchema schema;
  ASSERT_EQ(Status::END, collect("rowgroups.parquet", decode_base64(ROW_GROUP_PARQUET), rows, schema));
  ASSERT_EQ(2U, schema.names.size());
  EXPECT_EQ("id", schema.names[0]);
  EXPECT_EQ("name", schema.names[1]);
  EXPECT_EQ(Type::BIGINT, schema.types[0]);
  EXPECT_EQ(Type::VARCHAR, schema.types[1]);
  EXPECT_FALSE(schema.nullable[0]);
  EXPECT_TRUE(schema.nullable[1]);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ("1", rows[0][0].text);
  EXPECT_EQ("a", rows[0][1].text);
  EXPECT_EQ("2", rows[1][0].text);
  EXPECT_TRUE(rows[1][1].is_null);
  EXPECT_EQ("3", rows[2][0].text);
  EXPECT_EQ("c", rows[2][1].text);
}

TEST_F(ParquetReaderTest, ReadsBooleanDoubleAndDateColumns)
{
  std::vector<std::vector<Field>> rows;
  CsvSchema schema;
  ASSERT_EQ(Status::END, collect("types.parquet", decode_base64(TYPE_PARQUET), rows, schema));
  ASSERT_EQ(3U, schema.names.size());
  EXPECT_EQ(Type::BOOLEAN, schema.types[0]);
  EXPECT_EQ(Type::DOUBLE, schema.types[1]);
  EXPECT_EQ(Type::DATE, schema.types[2]);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ("true", rows[0][0].text);
  EXPECT_EQ("false", rows[1][0].text);
  EXPECT_EQ("1.5", rows[0][1].text);
  EXPECT_EQ("2.25", rows[1][1].text);
  EXPECT_EQ("1970-01-01", rows[0][2].text);
  EXPECT_EQ("1970-01-02", rows[1][2].text);
  EXPECT_TRUE(rows[2][2].is_null);
}

TEST_F(ParquetReaderTest, ReadsPaddedBitPackedDefinitionLevels)
{
  std::vector<std::vector<Field>> rows;
  CsvSchema schema;
  ASSERT_EQ(Status::END, collect("bitpacked.parquet", decode_base64(BIT_PACKED_PARQUET), rows, schema));
  ASSERT_EQ(7U, rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i % 2 == 0) {
      EXPECT_FALSE(rows[i][0].is_null);
      EXPECT_EQ(std::to_string(i + 1), rows[i][0].text);
    } else {
      EXPECT_TRUE(rows[i][0].is_null);
    }
  }
}

TEST_F(ParquetReaderTest, RejectsMalformedFiles)
{
  write("empty.parquet", "");
  SecureFile file;
  ASSERT_EQ(Status::OK, file.open(policy, "empty.parquet"));
  ParquetReader reader(file, ParquetOptions{});
  CsvSchema schema;
  Diagnostic diagnostic;
  EXPECT_EQ(Status::INVALID, reader.prepare(schema, diagnostic));

  write("text.parquet", "this is not a parquet file at all");
  ASSERT_EQ(Status::OK, file.open(policy, "text.parquet"));
  ParquetReader other(file, ParquetOptions{});
  EXPECT_EQ(Status::INVALID, other.prepare(schema, diagnostic));

  write("truncated.parquet", decode_base64(OFFICIAL_PARQUET).substr(0, 200));
  ASSERT_EQ(Status::OK, file.open(policy, "truncated.parquet"));
  ParquetReader third(file, ParquetOptions{});
  EXPECT_EQ(Status::INVALID, third.prepare(schema, diagnostic));
}

TEST_F(ParquetReaderTest, RewindsToTheFirstRow)
{
  write("simple.parquet", decode_base64(OFFICIAL_PARQUET));
  SecureFile file;
  ASSERT_EQ(Status::OK, file.open(policy, "simple.parquet"));
  ParquetReader reader(file, ParquetOptions{});
  CsvSchema schema;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, reader.prepare(schema, diagnostic));
  ASSERT_EQ(Status::OK, reader.rewind(diagnostic));
  std::vector<Field> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ("0", row[0].text);
  ASSERT_EQ(Status::OK, reader.rewind(diagnostic));
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ("0", row[0].text);
}
