/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_file.h"
#include "sql/engine/basic/file_scan_cursor.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace oceanbase::sql::file_scan;
namespace fs = std::filesystem;

class SecureFileTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    char pattern[] = "/tmp/seekdb-file-security-XXXXXX";
    const char *directory = ::mkdtemp(pattern);
    ASSERT_NE(nullptr, directory);
    base = directory;
    fs::create_directories(base / "allowed/sub");
    fs::create_directory(base / "allowed_other");
    policy.enabled = true;
    policy.secure_root = (base / "allowed").string();
    write("allowed/sub/data.csv", "id,name\n1,alice\n");
    write("allowed_other/outside.csv", "secret\n");
  }
  void TearDown() override { if (!base.empty()) { fs::remove_all(base); } }
  void write(const std::string &name, const std::string &text)
  {
    std::ofstream output(base / name, std::ios::binary | std::ios::trunc);
    output << text;
    ASSERT_TRUE(output.good());
  }
  fs::path base;
  FilePolicy policy;
};

TEST_F(SecureFileTest, DisabledAndMissingRootFailClosed)
{
  SecureFile file;
  FilePolicy defaults;
  EXPECT_EQ(Status::INVALID, file.open(defaults, "sub/data.csv"));
  defaults.enabled = true;
  EXPECT_EQ(Status::INVALID, file.open(defaults, "sub/data.csv"));
  defaults.secure_root = "relative";
  EXPECT_EQ(Status::INVALID, file.open(defaults, "sub/data.csv"));
}

TEST_F(SecureFileTest, StreamsInfersAndRewindsTheSameDescriptor)
{
  SecureFile file;
  ASSERT_EQ(Status::OK, file.open(policy, "sub/data.csv"));
  CsvSchema schema;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, infer_csv_schema(file.stream(), {}, schema, diagnostic));
  EXPECT_EQ(1U, schema.row_count);
  EXPECT_EQ(Type::BIGINT, schema.types[0]);
  ASSERT_EQ(Status::OK, file.rewind());
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(file.stream(), line)));
  EXPECT_EQ("id,name", line);
  EXPECT_EQ(Status::OK, file.open(policy, (base / "allowed/sub/data.csv").string()));
}

TEST_F(SecureFileTest, RejectsTraversalNulAndSiblingPrefix)
{
  SecureFile file;
  for (const auto &path : {"../allowed_other/outside.csv", "sub/../../allowed_other/outside.csv", ""}) {
    EXPECT_EQ(Status::INVALID, file.open(policy, path));
  }
  EXPECT_EQ(Status::INVALID, file.open(policy, std::string("sub/data.csv\0hidden", 19)));
  EXPECT_EQ(Status::INVALID, file.open(policy, (base / "allowed_other/outside.csv").string()));
  EXPECT_EQ(Status::INVALID, file.open(policy, policy.secure_root));
}

TEST_F(SecureFileTest, RejectsAllSymlinkComponentsIncludingRoot)
{
  fs::create_symlink(base / "allowed_other/outside.csv", base / "allowed/link.csv");
  fs::create_directory_symlink(base / "allowed_other", base / "allowed/linkdir");
  fs::create_directory_symlink(base / "allowed", base / "rootlink");
  SecureFile file;
  EXPECT_EQ(Status::IO_ERROR, file.open(policy, "link.csv"));
  EXPECT_EQ(Status::IO_ERROR, file.open(policy, "linkdir/outside.csv"));
  policy.secure_root = (base / "rootlink").string();
  EXPECT_EQ(Status::IO_ERROR, file.open(policy, "sub/data.csv"));
}

TEST_F(SecureFileTest, RejectsDirectoriesFifosMissingFilesAndSizeLimit)
{
  SecureFile file;
  ASSERT_EQ(0, ::mkfifo((base / "allowed/fifo").c_str(), 0600));
  EXPECT_EQ(Status::IO_ERROR, file.open(policy, "fifo"));
  EXPECT_EQ(Status::IO_ERROR, file.open(policy, "sub"));
  EXPECT_EQ(Status::IO_ERROR, file.open(policy, "missing.csv"));
  policy.max_file_bytes = 1;
  EXPECT_EQ(Status::LIMIT, file.open(policy, "sub/data.csv"));
}

TEST_F(SecureFileTest, DetectsAppendAndNeverSilentlyRewindsChangedFile)
{
  SecureFile file;
  ASSERT_EQ(Status::OK, file.open(policy, "sub/data.csv"));
  std::ofstream(base / "allowed/sub/data.csv", std::ios::app) << "2,bob\n";
  EXPECT_EQ(Status::IO_ERROR, file.validate());
  EXPECT_EQ(Status::IO_ERROR, file.rewind());
}

TEST_F(SecureFileTest, DetectsSameSizeRewriteReplacementAndDeletion)
{
  SecureFile file;
  ASSERT_EQ(Status::OK, file.open(policy, "sub/data.csv"));
  write("allowed/sub/data.csv", "id,name\n2,bobby\n");
  EXPECT_EQ(Status::IO_ERROR, file.validate());
  ASSERT_EQ(Status::OK, file.open(policy, "sub/data.csv"));
  fs::rename(base / "allowed/sub/data.csv", base / "allowed/sub/old.csv");
  write("allowed/sub/data.csv", "id,name\n2,bobby\n");
  EXPECT_EQ(Status::IO_ERROR, file.validate());
  ASSERT_EQ(Status::OK, file.open(policy, "sub/data.csv"));
  fs::remove(base / "allowed/sub/data.csv");
  EXPECT_EQ(Status::IO_ERROR, file.validate());
}

TEST_F(SecureFileTest, DetectsAncestorReplacementAndSymlinkSwap)
{
  SecureFile file;
  ASSERT_EQ(Status::OK, file.open(policy, "sub/data.csv"));
  fs::rename(base / "allowed/sub", base / "moved");
  fs::create_directory_symlink(base / "moved", base / "allowed/sub");
  EXPECT_EQ(Status::IO_ERROR, file.validate());
}

TEST_F(SecureFileTest, StreamReportsChangeAsIoFailureInsteadOfEnd)
{
  SecureFile file;
  ASSERT_EQ(Status::OK, file.open(policy, "sub/data.csv"));
  fs::remove(base / "allowed/sub/data.csv");
  CsvReader reader(file.stream(), {});
  std::vector<Field> row;
  EXPECT_EQ(Status::IO_ERROR, reader.next(row));
  EXPECT_TRUE(file.stream().bad());
}

TEST_F(SecureFileTest, RevalidatesBeforeReadingAnotherBuffer)
{
  write("allowed/sub/data.csv", std::string(128 * 1024, 'x'));
  SecureFile file;
  ASSERT_EQ(Status::OK, file.open(policy, "sub/data.csv"));
  EXPECT_EQ('x', file.stream().get());
  write("allowed/sub/data.csv", std::string(128 * 1024, 'y'));
  file.stream().ignore(64 * 1024);
  EXPECT_TRUE(file.stream().bad());
  EXPECT_EQ(Status::IO_ERROR, file.validate());
}

TEST_F(SecureFileTest, CursorRescanPreservesFullSchemaAndSkipsHeader)
{
  write("allowed/sub/data.csv", "id,value\n1,7\n2,3.5\n");
  QueryOptions options;
  options.path = "sub/data.csv";
  FileCursor cursor;
  ASSERT_EQ(Status::OK, cursor.prepare(policy, options));
  EXPECT_EQ((std::vector<Type>{Type::BIGINT, Type::DOUBLE}), cursor.schema().types);
  EXPECT_EQ(2U, cursor.schema().row_count);
  const auto fingerprint = cursor.fingerprint();
  ASSERT_FALSE(fingerprint.empty());
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<Field> row;
    ASSERT_EQ(Status::OK, cursor.next(row));
    ASSERT_EQ(2U, row.size());
    EXPECT_EQ("1", row[0].text);
    ASSERT_EQ(Status::OK, cursor.next(row));
    EXPECT_EQ("3.5", row[1].text);
    EXPECT_EQ(Status::END, cursor.next(row));
    EXPECT_TRUE(row.empty());
    ASSERT_EQ(Status::OK, cursor.rewind());
    EXPECT_EQ(fingerprint, cursor.fingerprint());
  }
}

TEST_F(SecureFileTest, CursorJsonProjectsUnionAndMissingFieldsAcrossRescans)
{
  write("allowed/sub/data.jsonl", "{\"id\":1}\n{\"id\":2,\"late\":true}\n");
  QueryOptions options;
  options.path = "sub/data.jsonl";
  options.json = true;
  FileCursor cursor;
  ASSERT_EQ(Status::OK, cursor.prepare(policy, options));
  ASSERT_EQ(2U, cursor.schema().names.size());
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<Field> row;
    ASSERT_EQ(Status::OK, cursor.next(row));
    EXPECT_TRUE(row[1].is_null);
    ASSERT_EQ(Status::OK, cursor.next(row));
    EXPECT_EQ("true", row[1].text);
    ASSERT_EQ(Status::END, cursor.next(row));
    ASSERT_EQ(Status::OK, cursor.rewind());
  }
}

TEST_F(SecureFileTest, CursorDetectsChangeBetweenInferenceAndFirstOutput)
{
  QueryOptions options;
  options.path = "sub/data.csv";
  FileCursor cursor;
  ASSERT_EQ(Status::OK, cursor.prepare(policy, options));
  write("allowed/sub/data.csv", "id,name\n2,bobby\n");
  std::vector<Field> row;
  EXPECT_EQ(Status::IO_ERROR, cursor.next(row));
  EXPECT_TRUE(row.empty());
  EXPECT_EQ(Status::IO_ERROR, cursor.rewind());
}

TEST_F(SecureFileTest, FailedPrepareCannotReviveAnOldSchema)
{
  QueryOptions options;
  options.path = "sub/data.csv";
  FileCursor cursor;
  ASSERT_EQ(Status::OK, cursor.prepare(policy, options));
  write("allowed/sub/data.csv", "id,name\n1,bad,extra\n");
  EXPECT_EQ(Status::INVALID, cursor.prepare(policy, options));
  EXPECT_EQ(Status::INVALID, cursor.rewind());
  std::vector<Field> row;
  EXPECT_EQ(Status::INVALID, cursor.next(row));
}

TEST_F(SecureFileTest, GlobCursorReturnsPathsAndRejectsChangedRescans)
{
  QueryOptions options;
  options.path = "sub/*.csv";
  options.glob = true;
  FileCursor cursor;
  ASSERT_EQ(Status::OK, cursor.prepare(policy, options));
  EXPECT_EQ((std::vector<std::string>{"file"}), cursor.schema().names);
  EXPECT_EQ((std::vector<Type>{Type::VARCHAR}), cursor.schema().types);
  std::vector<Field> row;
  ASSERT_EQ(Status::OK, cursor.next(row));
  ASSERT_EQ(1U, row.size());
  EXPECT_EQ((base / "allowed/sub/data.csv").string(), row[0].text);
  ASSERT_EQ(Status::END, cursor.next(row));
  ASSERT_EQ(Status::OK, cursor.rewind());
  write("allowed/sub/new.csv", "id\n2\n");
  EXPECT_EQ(Status::IO_ERROR, cursor.rewind());
  EXPECT_EQ(Status::INVALID, cursor.next(row));
}
