/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_glob.h"
#include <gtest/gtest.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace oceanbase::sql::file_scan;
namespace fs = std::filesystem;

namespace
{
// Counts open descriptors: "." plus ".." plus the one opendir itself holds.
int open_descriptor_count()
{
  DIR *dir = ::opendir("/proc/self/fd");
  if (nullptr == dir) { return -1; }
  int count = -3;
  while (::readdir(dir) != nullptr) { ++count; }
  ::closedir(dir);
  return count;
}
} // namespace

class FileScanGlobTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    char pattern[] = "/tmp/seekdb-file-glob-XXXXXX";
    const char *directory = ::mkdtemp(pattern);
    ASSERT_NE(nullptr, directory);
    base = directory;
    fs::create_directories(base / "allowed/sub");
    fs::create_directories(base / "allowed_empty");
    fs::create_directories(base / "allowed_other");
    policy.enabled = true;
    policy.secure_root = (base / "allowed").string();
    write("allowed/b.csv", "b\n");
    write("allowed/a.txt", "a\n");
    write("allowed/c.txt", "c\n");
    write("allowed/.hidden.txt", "h\n");
    write("allowed/sub/d.txt", "d\n");
    write("allowed_other/outside.csv", "secret\n");
  }
  void TearDown() override { if (!base.empty()) { fs::remove_all(base); } }
  void write(const std::string &name, const std::string &text) const
  {
    std::ofstream output(base / name, std::ios::binary | std::ios::trunc);
    output << text;
    ASSERT_TRUE(output.good());
  }
  // Absolute paths of `names` below base/allowed, sorted like discover_files.
  std::vector<std::string> expect(const std::vector<std::string> &names) const
  {
    std::vector<std::string> result;
    for (const auto &name : names) { result.push_back((base / "allowed" / name).string()); }
    std::sort(result.begin(), result.end());
    return result;
  }
  // Absolute glob for the sibling directory, which lies outside secure_root.
  std::string sibling_glob() const { return (base / "allowed_other/*.csv").string(); }
  fs::path base;
  FilePolicy policy;
};

// A disabled, empty, relative or '..'-containing secure_root fails closed and
// never publishes partial output. "/" is a valid root (covered below).
TEST_F(FileScanGlobTest, DisabledPolicyAndInvalidRootFailClosed)
{
  std::vector<std::string> files{"sentinel"};
  Diagnostic diagnostic;
  FilePolicy defaults;
  EXPECT_EQ(Status::INVALID, discover_files(defaults, "*.txt", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{"sentinel"}, files);
  defaults.enabled = true;
  diagnostic = {};
  EXPECT_EQ(Status::INVALID, discover_files(defaults, "*.txt", files, diagnostic));
  EXPECT_FALSE(diagnostic.message.empty());
  const std::string root = (base / "allowed").string();
  defaults.secure_root = "relative/root";
  EXPECT_EQ(Status::INVALID, discover_files(defaults, root + "/*.txt", files, diagnostic));
  EXPECT_EQ(Status::INVALID, discover_files(defaults, "*.txt", files, diagnostic));
  // A relative pattern is anchored under secure_root, never under the CWD. The
  // result for a rejected absolute pattern is only checked after a call that
  // is expected to publish its result, which resets `files` in place.
  EXPECT_EQ(Status::INVALID, discover_files(policy, "../*.txt", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{"sentinel"}, files);
  EXPECT_EQ(Status::INVALID, discover_files(policy, "sub/../../*.txt", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{"sentinel"}, files);
  FilePolicy slash = policy;
  slash.secure_root = "/";
  EXPECT_EQ(Status::INVALID, discover_files(slash, "../*.txt", files, diagnostic));
  defaults.secure_root = "/tmp/../" + root.substr(1);
  EXPECT_EQ(Status::INVALID, discover_files(defaults, root + "/*.txt", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{"sentinel"}, files);
}

// The shell never expands a wildcard inside a quoted argument, and never a
// leading '/', so a glob input is an explicit pattern. Patterns are normalized
// component-wise like the secure file layer, but '..', NUL, an empty pattern
// and sibling-prefix paths are refused before anything outside secure_root is
// looked at, and every refusal leaves `files` untouched.
TEST_F(FileScanGlobTest, RejectsTraversalNulAndSiblingPrefixes)
{
  std::vector<std::string> files{"keep"};
  const std::string root = (base / "allowed").string();
  const std::string sibling = (base / "allowed_other").string();
  // A rejection loop must not read `files` in the same EXPECT as the call: the
  // gtest arguments evaluate right to left, so the call would run first.
  const std::vector<std::string> bad = {
    "../allowed_other/*.csv",
    "sub/../../allowed_other/*.csv",
    (base / "allowed/../allowed_other/*.csv").string(),
    (base / "allowed/./../allowed_other/*.csv").string(),
    sibling + "/*.csv",
    root + "2/*.csv",
    std::string("*") + '\0' + ".txt",
    "",
    "/",
  };
  for (const auto &pattern : bad) {
    Diagnostic current;
    const Status status = discover_files(policy, pattern, files, current);
    EXPECT_EQ(Status::INVALID, status) << pattern;
    EXPECT_FALSE(current.message.empty()) << pattern;
    EXPECT_EQ(std::vector<std::string>{"keep"}, files) << pattern;
  }
  // The configured root and its trailing-slash spelling name no file.
  for (const auto &pattern : std::vector<std::string>{root, root + "/"}) {
    Diagnostic current;
    const Status status = discover_files(policy, pattern, files, current);
    EXPECT_EQ(Status::INVALID, status) << pattern;
    EXPECT_EQ(std::vector<std::string>{"keep"}, files) << pattern;
  }
  // "su*/d.txt" stays inside secure_root: 'su*' matches only its own "sub" (a
  // real directory); the final literal leaf is then the file "sub/d.txt".
  Diagnostic current;
  ASSERT_EQ(Status::OK, discover_files(policy, "su*/d.txt", files, current));
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "su*/x*.txt", files, current));
  EXPECT_TRUE(files.empty());
}

TEST_F(FileScanGlobTest, NormalizesRedundantSlashesAndDots)
{
  std::vector<std::string> files;
  Diagnostic diagnostic;
  const std::string root = (base / "allowed").string();
  // Redundant separators and '.' are normalized in the root as well, including
  // an absolute pattern spelling the root with them (a trailing '/' is ignored).
  Diagnostic absolute;
  ASSERT_EQ(Status::OK, discover_files(policy, root + "//./sub/*.txt", files, absolute))
      << absolute.message;
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "./sub/./*.txt", files, diagnostic)) << diagnostic.message;
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  // A leading '/' makes the pattern absolute: "/./sub" is "/sub", which lies
  // outside the "/tmp/.../allowed" root and is refused, never anchored.
  EXPECT_EQ(Status::INVALID, discover_files(policy, "/./sub//*.txt", files, diagnostic));
  EXPECT_FALSE(diagnostic.message.empty());
}

TEST_F(FileScanGlobTest, HiddenFilesNeedAnExplicitDot)
{
  std::vector<std::string> files;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "*.txt", files, diagnostic));
  EXPECT_EQ(expect({"a.txt", "c.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, ".*.txt", files, diagnostic));
  EXPECT_EQ(expect({".hidden.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/.hidden.txt", files, diagnostic));
  EXPECT_TRUE(files.empty());
}

// A single '*' never crosses '/', so the listing stays at the root.
TEST_F(FileScanGlobTest, LiteralAndWildcardSelectionIsSorted)
{
  std::vector<std::string> files;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "a.txt", files, diagnostic));
  EXPECT_EQ(expect({"a.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "*.csv", files, diagnostic));
  EXPECT_EQ(expect({"b.csv"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/d.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "[ac].t?t", files, diagnostic));
  EXPECT_EQ(expect({"a.txt", "c.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "[!a]*", files, diagnostic));
  // 'b.csv' has no leading 'a', so it matches `[!a]*`.
  EXPECT_EQ(expect({"b.csv", "c.txt"}), files);
  write("allowed/z.txt", "z\n");
  write("allowed/d.txt", "d\n");
  // 'sub' is a directory, so it is never a match for '*' (regular files only).
  ASSERT_EQ(Status::OK, discover_files(policy, "*", files, diagnostic));
  EXPECT_EQ(expect({"a.txt", "b.csv", "c.txt", "d.txt", "z.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/*.txt", files, diagnostic));
  ASSERT_EQ(1U, files.size());
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  // A non-wildcard component is literal, so a prefix of a directory name never
  // reaches it: "su" is a missing literal parent, which is INVALID.
  EXPECT_EQ(Status::INVALID, discover_files(policy, "su/d.txt", files, diagnostic));
  EXPECT_FALSE(diagnostic.message.empty());
}

// Wildcard directory components expand to every matching real directory.
TEST_F(FileScanGlobTest, WildcardParentComponentsAreSupported)
{
  fs::create_directories(base / "allowed/one/inner");
  fs::create_directories(base / "allowed/two");
  write("allowed/one/x.csv", "x\n");
  write("allowed/two/y.csv", "y\n");
  write("allowed/one/inner/z.csv", "z\n");
  write("allowed/one/skip.json", "{}\n");
  std::vector<std::string> files;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "*/*.csv", files, diagnostic));
  EXPECT_EQ(expect({"one/x.csv", "two/y.csv"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "on?/*.csv", files, diagnostic));
  EXPECT_EQ(expect({"one/x.csv"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "*/*/*.csv", files, diagnostic));
  EXPECT_EQ(expect({"one/inner/z.csv"}), files);
  // A leading '/' makes the pattern absolute and outside the root, so it is
  // refused (never anchored under secure_root).
  EXPECT_EQ(Status::INVALID, discover_files(policy, "/[ot]*/*.csv", files, diagnostic));
  // A directory symlink is never followed by a wildcard expansion.
  fs::create_directory_symlink(base / "allowed_other", base / "allowed/linked");
  ASSERT_EQ(Status::OK, discover_files(policy, "*/*.csv", files, diagnostic));
  EXPECT_EQ(expect({"one/x.csv", "two/y.csv"}), files);
  // A wildcard directory that exists but holds no match yields no file, and so
  // does a parent wildcard without any matching directory.
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/*.csv", files, diagnostic));
  EXPECT_TRUE(files.empty());
  // A literal final component is simply absent: OK with an empty list.
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/nothing.csv", files, diagnostic));
  EXPECT_TRUE(files.empty());
  EXPECT_EQ(Status::INVALID, discover_files(policy, "sub/nothing.csv/*.csv", files, diagnostic));
  // A final component below a wildcard directory is pruned with it: no match.
  ASSERT_EQ(Status::OK, discover_files(policy, "s*/x.csv", files, diagnostic));
  EXPECT_TRUE(files.empty());
  // A leading '/' makes the pattern absolute and outside the root, so it is
  // refused; "s*/x.csv" (no leading '/') filters the root's directories.
  EXPECT_EQ(Status::INVALID, discover_files(policy, "/s*/x.csv", files, diagnostic));
  EXPECT_EQ(Status::INVALID, discover_files(policy, "/[ot]*/*.csv", files, diagnostic));
  // A prefix of secure_root is outside it, even when every wildcard level
  // exists (the pattern is rejected before any directory below it is read).
  EXPECT_EQ(Status::INVALID, discover_files(policy, base.string() + "/*/*.csv", files, diagnostic));
  // An absolute pattern outside secure_root is rejected (never listed); a
  // relative pattern always anchors under it, so './' prefixes are harmless.
  EXPECT_EQ(Status::INVALID, discover_files(policy, sibling_glob(), files, diagnostic));
  ASSERT_EQ(Status::OK, discover_files(policy, "./sub/*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/d.txt"}), files);
}

TEST_F(FileScanGlobTest, RelativeAndAbsolutePatternsAreEquivalent)
{
  std::vector<std::string> relative;
  std::vector<std::string> absolute;
  Diagnostic diagnostic;
  for (const std::string &leaf : std::vector<std::string>{
           "*.txt", "*", "sub/*.txt", "sub/*", "a.txt", "sub/d.txt", "./sub/*.txt"}) {
    ASSERT_EQ(Status::OK, discover_files(policy, leaf, relative, diagnostic)) << leaf;
    // The absolute form is built by normalizing the root and leaf joined with
    // '/', exactly like the secure file layer accepts "a//b" and "a/./b".
    ASSERT_EQ(Status::OK,
        discover_files(policy, (base / "allowed").string() + "/" + leaf, absolute, diagnostic)) << leaf;
    EXPECT_EQ(relative, absolute) << leaf;
    for (const auto &file : relative) { EXPECT_EQ('/', file[0]) << leaf; }
  }
  // Directly at the root (parent == secure_root) enumeration must succeed; a
  // leading '/' is absolute and outside the root, so it is refused.
  ASSERT_EQ(Status::OK, discover_files(policy, "*.txt", relative, diagnostic));
  EXPECT_EQ(expect({"a.txt", "c.txt"}), relative);
  EXPECT_EQ(Status::INVALID, discover_files(policy, "/sub/*.txt", relative, diagnostic));
}

TEST_F(FileScanGlobTest, AcceptsAbsolutePatternsOnlyInsideRoot)
{
  std::vector<std::string> files;
  Diagnostic diagnostic;
  const std::string root = (base / "allowed").string();
  ASSERT_EQ(Status::OK, discover_files(policy, root + "/sub/*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  EXPECT_EQ(Status::INVALID, discover_files(policy, root + "/", files, diagnostic));
  // A sibling that only shares the root's string prefix is outside the root.
  EXPECT_EQ(Status::INVALID, discover_files(policy, root + "2/*.csv", files, diagnostic));
  // A trailing slash on the configured root is normalized by components.
  policy.secure_root = root + "/";
  ASSERT_EQ(Status::OK, discover_files(policy, "*.txt", files, diagnostic));
  EXPECT_EQ(expect({"a.txt", "c.txt"}), files);
  EXPECT_EQ(Status::INVALID, discover_files(policy, root, files, diagnostic));
  policy.secure_root = root + "//./";
  ASSERT_EQ(Status::OK, discover_files(policy, "*.txt", files, diagnostic));
  EXPECT_EQ(expect({"a.txt", "c.txt"}), files);
}

TEST_F(FileScanGlobTest, SkipsSymlinksDirectoriesAndSpecialFiles)
{
  fs::create_symlink(base / "allowed/a.txt", base / "allowed/link.txt");
  fs::create_directory_symlink(base / "allowed/sub", base / "allowed/linkdir");
  ASSERT_EQ(0, ::mkfifo((base / "allowed/pipe.txt").c_str(), 0600));
  std::vector<std::string> files;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "*.txt", files, diagnostic));
  EXPECT_EQ(expect({"a.txt", "c.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "link.txt", files, diagnostic));
  EXPECT_TRUE(files.empty());
  // A symbolic-link parent is never followed (IO_ERROR): the literal resolution
  // reports the symbolic link even when a valid target exists.
  files = {"keep"};
  EXPECT_EQ(Status::IO_ERROR, discover_files(policy, "linkdir/d.txt", files, diagnostic));
  EXPECT_FALSE(diagnostic.message.empty());
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
  Diagnostic current;
  // A missing leaf under a never-followed symbolic-link parent is still refused:
  // the link is reported, not silently treated as "no match".
  EXPECT_EQ(Status::IO_ERROR, discover_files(policy, "linkdir/missing.txt", files, current));
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
  // A wildcard never takes a symlink: it is skipped though the target is a file.
  ASSERT_EQ(Status::OK, discover_files(policy, "li*.txt", files, diagnostic));
  EXPECT_TRUE(files.empty());
  ASSERT_EQ(Status::OK, discover_files(policy, "*dir", files, diagnostic));
  EXPECT_TRUE(files.empty());
  ASSERT_EQ(Status::OK, discover_files(policy, "*.txt", files, diagnostic));
  EXPECT_EQ(expect({"a.txt", "c.txt"}), files);
}

TEST_F(FileScanGlobTest, RejectsSymlinkedRootAndRootParent)
{
  fs::create_directory_symlink(base / "allowed", base / "rootlink");
  std::vector<std::string> files{"keep"};
  Diagnostic diagnostic;
  policy.secure_root = (base / "rootlink").string();
  EXPECT_EQ(Status::IO_ERROR, discover_files(policy, "*.txt", files, diagnostic));
  EXPECT_FALSE(diagnostic.message.empty());
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
  fs::create_directory_symlink(base, base / "topparent");
  policy.secure_root = (base / "topparent/allowed").string();
  EXPECT_EQ(Status::IO_ERROR, discover_files(policy, "*.txt", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
}

// The default fixture has a.txt and c.txt at the root; ".hidden.txt" is
// excluded because '*' needs an explicit leading '.'.
TEST_F(FileScanGlobTest, EnforcesFileCountLimit)
{
  write("allowed/d.txt", "d\n");
  std::vector<std::string> files{"keep"};
  Diagnostic diagnostic;
  EXPECT_EQ(Status::LIMIT, discover_files(policy, "*", files, diagnostic, 0));
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
  // One fewer than the three matches fails closed without output; a larger
  // limit publishes only the regular-file matches.
  EXPECT_EQ(Status::LIMIT, discover_files(policy, "*.txt", files, diagnostic, 2));
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
  ASSERT_EQ(Status::OK, discover_files(policy, "*.txt", files, diagnostic, 3));
  EXPECT_EQ(expect({"a.txt", "c.txt", "d.txt"}), files);
  // The limit also applies across wildcard-expanded parent directories: three
  // files live below subdirectories (sub/d.txt plus the two new ones).
  fs::create_directories(base / "allowed/a_dir");
  fs::create_directories(base / "allowed/b_dir");
  write("allowed/a_dir/x.txt", "x\n");
  write("allowed/b_dir/y.txt", "y\n");
  EXPECT_EQ(Status::LIMIT, discover_files(policy, "*/*.txt", files, diagnostic, 1));
  EXPECT_EQ(expect({"a.txt", "c.txt", "d.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "*/*.txt", files, diagnostic, 3));
  EXPECT_EQ(expect({"a_dir/x.txt", "b_dir/y.txt", "sub/d.txt"}), files);
}

TEST_F(FileScanGlobTest, MissingOrNonDirectoryParentsFailWithoutOutput)
{
  std::vector<std::string> files{"keep"};
  Diagnostic diagnostic;
  EXPECT_EQ(Status::INVALID, discover_files(policy, "missing/*.txt", files, diagnostic));
  // "a.txt" exists but is a file, not a parent directory: the same refusal the
  // secure file layer applies to a non-directory path component.
  EXPECT_EQ(Status::INVALID, discover_files(policy, "a.txt/*.txt", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
  EXPECT_FALSE(diagnostic.message.empty());
  // The configured root as a regular file is not a directory.
  FilePolicy file_root = policy;
  file_root.secure_root = (base / "allowed/a.txt").string();
  EXPECT_EQ(Status::INVALID, discover_files(file_root, "*", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
  // A literal leaf below a file or a link is refused the same way.
  EXPECT_EQ(Status::INVALID, discover_files(policy, "a.txt/x.txt", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
}

TEST_F(FileScanGlobTest, MissingRootFailsWithoutOutput)
{
  write("allowed/a2.txt", "a2\n");
  std::vector<std::string> files{"keep"};
  Diagnostic diagnostic;
  FilePolicy other = policy;
  other.secure_root = (base / "absent").string();
  EXPECT_EQ(Status::INVALID, discover_files(other, "*.txt", files, diagnostic));
  EXPECT_FALSE(diagnostic.message.empty());
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
}

TEST_F(FileScanGlobTest, NoMatchSucceedsWithEmptyResult)
{
  std::vector<std::string> files{"keep"};
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "*.nothing", files, diagnostic));
  EXPECT_TRUE(files.empty());
  FilePolicy empty_root = policy;
  empty_root.secure_root = (base / "allowed_empty").string();
  ASSERT_EQ(Status::OK, discover_files(empty_root, "*", files, diagnostic));
  EXPECT_TRUE(files.empty());
}

// fnmatch(3) with FNM_PERIOD: '[...]' is a character class, '?' is any single
// character, and '\' escapes the next character.
TEST_F(FileScanGlobTest, FnmatchEscapesHiddenNamesAndDirectories)
{
  write("allowed/a[b].txt", "x\n");
  write("allowed/a\\b.txt", "x\n");
  write("allowed/sub/.deep.txt", "x\n");
  std::vector<std::string> files;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "a\\[b\\].txt", files, diagnostic));
  EXPECT_EQ(expect({"a[b].txt"}), files);
  // '\\\\' is one literal backslash (two escapes inside a C++ literal).
  ASSERT_EQ(Status::OK, discover_files(policy, "a\\\\b.txt", files, diagnostic));
  EXPECT_EQ(expect({"a\\b.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "a[[]b].txt", files, diagnostic));
  EXPECT_EQ(expect({"a[b].txt"}), files);
  // '?' matches exactly one byte, so 'a?b.txt' matches a\b.txt ('?' consumes the
  // backslash); it cannot match a[b].txt ('?' would leave ']' vs '.').
  ASSERT_EQ(Status::OK, discover_files(policy, "a?b.txt", files, diagnostic));
  EXPECT_EQ(expect({"a\\b.txt"}), files);
  // FNM_PERIOD: '*.txt' skips '.deep.txt' but still matches 'd.txt'.
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/.*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/.deep.txt"}), files);
  // '**' is not recursive: it stays in one component and only names files.
  ASSERT_EQ(Status::OK, discover_files(policy, "**", files, diagnostic));
  EXPECT_EQ(expect({"a[b].txt", "a\\b.txt", "a.txt", "b.csv", "c.txt"}), files);
  // A trailing '**' is not recursive either: it names 'sub/d.txt' directly.
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/**", files, diagnostic));
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  // '**' as a parent matches the one directory 'sub', then '*.txt' names d.txt.
  ASSERT_EQ(Status::OK, discover_files(policy, "**/*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/d.txt"}), files);
  // A '[' or '\' with nothing closing it names nothing in the shell, so it is
  // refused instead of being read as an unterminated fnmatch bracket/escape.
  EXPECT_EQ(Status::INVALID, discover_files(policy, "a[b.txt", files, diagnostic));
  EXPECT_EQ(Status::INVALID, discover_files(policy, "a\\", files, diagnostic));
}

TEST_F(FileScanGlobTest, NestedParentDirectoriesAreOpenedWithoutSymlinks)
{
  fs::create_directories(base / "allowed/sub/deep/deeper");
  write("allowed/sub/deep/deeper/x.txt", "x\n");
  write("allowed/sub/deep/y.txt", "y\n");
  std::vector<std::string> files;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/deep/deeper/*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/deep/deeper/x.txt"}), files);
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/deep/*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/deep/y.txt"}), files);
  const std::string root = (base / "allowed").string();
  ASSERT_EQ(Status::OK, discover_files(policy, root + "/sub/deep/deeper/*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/deep/deeper/x.txt"}), files);
  // A symlinked intermediate parent is refused and leaves no output.
  Diagnostic current;
  fs::create_directory_symlink(base / "allowed/sub/deep", base / "allowed/sub/deeplink");
  files = {"keep"};
  EXPECT_EQ(Status::IO_ERROR,
      discover_files(policy, "sub/deeplink/deeper/*.txt", files, current));
  EXPECT_FALSE(current.message.empty());
  EXPECT_EQ(std::vector<std::string>{"keep"}, files);
  // The same parent reached through a wildcard is skipped, not followed.
  ASSERT_EQ(Status::OK, discover_files(policy, "sub/deep*/*.txt", files, diagnostic));
  EXPECT_EQ(expect({"sub/deep/y.txt"}), files);
}

// "/" is an absolute security root (consistent with the secure file layer).
TEST_F(FileScanGlobTest, SlashSecureRootIsValid)
{
  fs::create_directories(base / "rootfiles");
  write("rootfiles/r.txt", "r\n");
  FilePolicy slash = policy;
  slash.secure_root = "/";
  std::vector<std::string> files{"keep"};
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(slash, "/tmp/*.this-is-not-there", files, diagnostic));
  EXPECT_TRUE(files.empty());
  const std::string expected = fs::weakly_canonical(base / "rootfiles/r.txt").string();
  ASSERT_EQ(Status::OK, discover_files(slash, expected, files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{expected}, files);
  EXPECT_EQ(Status::INVALID, discover_files(slash, expected + "/../r.txt", files, diagnostic));
  EXPECT_EQ(Status::INVALID, discover_files(slash, "/", files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{expected}, files);
  // A relative pattern is anchored at "/" and still returns absolute paths.
  const std::string relative = expected.substr(1);
  ASSERT_EQ(Status::OK, discover_files(slash, relative, files, diagnostic));
  EXPECT_EQ(std::vector<std::string>{expected}, files);
}

// max_files counts regular files only: directories, symlinks and FIFOs that
// match '*.txt' are skipped, so the limit is never consumed by them.
TEST_F(FileScanGlobTest, CountsOnlyRegularFilesAgainstLimit)
{
  fs::create_directories(base / "allowed/limit");
  write("allowed/limit/a.txt", "a\n");
  write("allowed/limit/b.txt", "b\n");
  fs::create_directories(base / "allowed/limit/dir.txt");
  fs::create_directory_symlink(base / "allowed/limit", base / "allowed/limit/link.txt");
  fs::create_symlink(base / "allowed/limit/a.txt", base / "allowed/limit/alias.txt");
  ASSERT_EQ(0, ::mkfifo((base / "allowed/limit/fifo.txt").c_str(), 0600));
  std::vector<std::string> files{"keep"};
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "limit/*.txt", files, diagnostic, 2));
  EXPECT_EQ(expect({"limit/a.txt", "limit/b.txt"}), files);
  EXPECT_EQ(Status::LIMIT, discover_files(policy, "limit/*.txt", files, diagnostic, 1));
  EXPECT_EQ(expect({"limit/a.txt", "limit/b.txt"}), files);
  EXPECT_FALSE(diagnostic.message.empty());
  // Only the two regular files count even when FIFOs/symlinks come first.
  ASSERT_EQ(Status::OK, discover_files(policy, "limit/*", files, diagnostic, 2));
  EXPECT_EQ(expect({"limit/a.txt", "limit/b.txt"}), files);
}

TEST_F(FileScanGlobTest, ErrorsDoNotLeakDescriptors)
{
  fs::create_directories(base / "allowed/sub/deep");
  write("allowed/sub/deep/x.txt", "x\n");
  fs::create_directory_symlink(base / "allowed/sub", base / "allowed/linkdir");
  FilePolicy slash = policy;
  slash.secure_root = "/";
  std::vector<std::string> files;
  Diagnostic diagnostic;
  ASSERT_EQ(Status::OK, discover_files(policy, "*.txt", files, diagnostic));
  const int before = open_descriptor_count();
  ASSERT_GE(before, 0);
  for (int i = 0; i < 20; ++i) {
    std::vector<std::string> scratch;
    Diagnostic current;
    EXPECT_EQ(Status::OK, discover_files(policy, "*", scratch, current));
    EXPECT_EQ(Status::OK, discover_files(policy, "sub/deep/*.txt", scratch, current));
    EXPECT_EQ(Status::OK, discover_files(policy, "*/*.txt", scratch, current));
    EXPECT_EQ(Status::OK, discover_files(policy, "missing*/*.txt", scratch, current));
    EXPECT_EQ(Status::OK, discover_files(policy, "sub/*/*.txt", scratch, current));
    EXPECT_EQ(Status::OK, discover_files(slash, "/tmp/*.none", scratch, current));
    EXPECT_EQ(Status::LIMIT, discover_files(policy, "*", scratch, current, 1));
    EXPECT_EQ(Status::INVALID, discover_files(policy, "missing/*", scratch, current));
    EXPECT_EQ(Status::INVALID, discover_files(policy, "../*", scratch, current));
    EXPECT_EQ(Status::IO_ERROR, discover_files(policy, "linkdir/*", scratch, current));
    // Refusals after the root has been opened also must not leak descriptors:
    // a missing root, a symlinked root, and an out-of-root absolute pattern.
    FilePolicy missing_root = policy;
    missing_root.secure_root = (base / "missing_root").string();
    EXPECT_EQ(Status::INVALID, discover_files(missing_root, "*", scratch, current));
    FilePolicy root_link = policy;
    root_link.secure_root = (base / "allowed/linkdir").string();
    EXPECT_EQ(Status::IO_ERROR, discover_files(root_link, "*", scratch, current));
    EXPECT_EQ(Status::INVALID, discover_files(policy, sibling_glob(), scratch, current));
    EXPECT_EQ(Status::OK, discover_files(policy, "sub/deep/*.txt", scratch, current));
  }
  EXPECT_EQ(before, open_descriptor_count());
}
