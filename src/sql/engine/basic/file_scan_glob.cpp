/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_glob.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <cstdio>

namespace oceanbase::sql::file_scan
{
namespace
{
// Same bounds as the secure file layer: a path Linux can open and one NAME_MAX
// component.
constexpr size_t MAX_PATH_BYTES = 4096;
constexpr size_t MAX_COMPONENT_BYTES = 255;
// Directories opened per call, so a broad wildcard (e.g. "/*/*/*" on "/")
// cannot explore the filesystem unbounded.
constexpr size_t MAX_DIRECTORIES = 65536;

Status fail(Status status, Diagnostic &diagnostic, const char *message)
{
  diagnostic.message = message;
  return status;
}

bool has_wildcard(const std::string &component)
{
  return component.find_first_of("*?[\\") != std::string::npos;
}

// Pattern components are brace-free shell globs. An unterminated '[' or a
// trailing '\' is expanded by no shell (bash leaves it literal), so fnmatch's
// bracket/escape reading would invent matches; such a pattern is refused.
bool well_formed(const std::string &component)
{
  for (size_t i = 0; i < component.size(); ++i) {
    const char c = component[i];
    if (c == '\\') {
      if (i + 1 == component.size()) { return false; }
      ++i;
    } else if (c == '[') {
      size_t end = i + 1;
      if (end < component.size() && (component[end] == '!' || component[end] == '^')) { ++end; }
      if (end < component.size() && component[end] == ']') { ++end; }
      while (end < component.size() && component[end] != ']') { ++end; }
      if (end == component.size()) { return false; }
      i = end;
    }
  }
  return true;
}

// Normalizes a path into components like the secure file layer: repeated '/'
// and '.' are dropped and a trailing '/' is ignored, so the root may be "a//./"
// and the pattern "/sub/*.txt". NUL or a '..' component is rejected, as is an
// over-long path or component.
bool split(const std::string &path, std::vector<std::string> &parts)
{
  parts.clear();
  if (path.empty() || path.size() >= MAX_PATH_BYTES || path.find('\0') != std::string::npos) {
    return false;
  }
  for (size_t begin = 0; begin < path.size();) {
    size_t end = path.find('/', begin);
    if (end == std::string::npos) { end = path.size(); }
    const std::string part = path.substr(begin, end - begin);
    if (part == ".." || part.size() > MAX_COMPONENT_BYTES) { parts.clear(); return false; }
    if (!part.empty() && part != ".") { parts.push_back(part); }
    begin = end + 1;
  }
  return true;
}

// "/a/b" for the first `count` components; no components is "/".
std::string absolute_name(const std::vector<std::string> &parts, size_t count)
{
  std::string result;
  for (size_t i = 0; i < count; ++i) { result += "/" + parts[i]; }
  return result.empty() ? "/" : result;
}

bool same_identity(const struct stat &left, const struct stat &right)
{
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

// True when `directory` holds a non-symbolic entry named `name`, whatever its
// type. Used to explain a failed literal lookup: a symbolic link or other
// non-directory is refused (IO_ERROR), a missing name matches nothing.
bool exists_without_link(int fd, const std::string &name)
{
  struct stat identity {};
  return ::fstatat(fd, name.c_str(), &identity, AT_SYMLINK_NOFOLLOW) == 0;
}

// Owning descriptor so no error path can leak an fd.
class Fd
{
public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() { reset(); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd &operator=(Fd &&other) noexcept
  {
    if (this != &other) {
      reset(other.fd_);
      other.fd_ = -1;
    }
    return *this;
  }
  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void reset(int fd = -1)
  {
    if (fd_ >= 0) { ::close(fd_); }
    fd_ = fd;
  }
private:
  int fd_ = -1;
};

// An open directory of the walk with its absolute path and absolute components.
struct Directory
{
  Fd fd;
  std::string path;             // e.g. "/tmp/root/sub"
  std::vector<std::string> components; // e.g. {"tmp", "root", "sub"}
  struct stat identity {};
};

struct Entry
{
  std::string name;
  struct stat identity {};
};

// fnmatch(3) for one path component with FNM_PERIOD, so a leading '.' needs an
// explicit dot. A literal component is compared byte for byte and is never
// interpreted by fnmatch.
bool matches(const std::string &component, const std::string &name, bool literal)
{
  return literal ? component == name : ::fnmatch(component.c_str(), name.c_str(), FNM_PERIOD) == 0;
}

// Lists the entries of `fd` that match `component`, are not "." or ".." and
// are, without following links, a regular file or (when `directories`) a real
// directory. Symbolic links, FIFOs and devices are never listed. At most
// `limit` matching entries are collected: the entry that would exceed the limit
// is rejected before it is appended and LIMIT is returned. Sorted by name.
Status list(int fd, const std::string &component, bool literal, bool directories, size_t limit,
            std::vector<Entry> &entries, Diagnostic &diagnostic)
{
  entries.clear();
  const int dup_fd = ::dup(fd);
  if (dup_fd < 0) { return fail(Status::IO_ERROR, diagnostic, "Cannot read glob directory"); }
  DIR *dir = ::fdopendir(dup_fd);
  if (nullptr == dir) {
    ::close(dup_fd);
    return fail(Status::IO_ERROR, diagnostic, "Cannot read glob directory");
  }
  Status status = Status::OK;
  while (true) {
    errno = 0;
    const struct dirent *item = ::readdir(dir);
    if (nullptr == item) {
      if (errno != 0) { status = fail(Status::IO_ERROR, diagnostic, "Cannot read glob directory"); }
      break;
    }
    const std::string name = item->d_name;
    if (name == "." || name == ".." || !matches(component, name, literal)) { continue; }
    Entry entry;
    entry.name = name;
    if (::fstatat(fd, name.c_str(), &entry.identity, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) { continue; } // vanished between readdir and stat
      status = fail(Status::IO_ERROR, diagnostic, "Cannot stat glob entry");
      break;
    }
    if (directories ? !S_ISDIR(entry.identity.st_mode) : !S_ISREG(entry.identity.st_mode)) { continue; }
    if (entries.size() >= limit) {
      status = fail(Status::LIMIT, diagnostic, "Glob result exceeds the resource limit");
      break;
    }
    entries.push_back(std::move(entry));
  }
  ::closedir(dir);
  if (status != Status::OK) { return status; }
  std::sort(entries.begin(), entries.end(),
      [](const Entry &left, const Entry &right) { return left.name < right.name; });
  return Status::OK;
}

// Opens `name` in `parent` as a real directory. O_NOFOLLOW refuses a symbolic
// link (ELOOP) and O_NONBLOCK keeps a racing FIFO from blocking open. A missing
// name is INVALID; a symbolic link or a non-directory is IO_ERROR.
Status open_directory(int parent, const Directory &base, const std::string &name, Directory &child,
                      Diagnostic &diagnostic)
{
  child = Directory{};
  child.fd.reset(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  const int error = errno;
  if (!child.fd.valid()) {
    // O_NOFOLLOW may report a symbolic link as ELOOP or ENOTDIR depending on the
    // kernel, so identify the link itself rather than trusting errno.
    struct stat link_stat {};
    const bool is_link = ::fstatat(parent, name.c_str(), &link_stat, AT_SYMLINK_NOFOLLOW) == 0
                         && S_ISLNK(link_stat.st_mode);
    if (is_link) {
      return fail(Status::IO_ERROR, diagnostic, "Symbolic link in glob path");
    }
    if (error == ENOENT || error == ENOTDIR) {
      return fail(Status::INVALID, diagnostic,
          error == ENOTDIR ? "Glob path is not a directory" : "Glob parent directory does not exist");
    }
    return fail(Status::IO_ERROR, diagnostic, "Cannot open glob directory");
  }
  if (::fstat(child.fd.get(), &child.identity) != 0 || !S_ISDIR(child.identity.st_mode)) {
    return fail(Status::IO_ERROR, diagnostic, "Glob path is not a real directory");
  }
  child.path = base.path + "/" + name;
  child.components = base.components;
  child.components.push_back(name);
  return Status::OK;
}

// Opens `components` from '/' one component at a time without following any
// symbolic link.
Status resolve(const std::vector<std::string> &components, Directory &result, Diagnostic &diagnostic)
{
  result = Directory{};
  result.path = "/";
  result.fd.reset(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!result.fd.valid() || ::fstat(result.fd.get(), &result.identity) != 0) {
    return fail(Status::IO_ERROR, diagnostic, "Cannot open filesystem root");
  }
  for (const auto &component : components) {
    Directory child;
    const Status status = open_directory(result.fd.get(), result, component, child, diagnostic);
    if (status != Status::OK) { return status; }
    result = std::move(child);
  }
  return Status::OK;
}

// Describes a literal directory that has no candidate under a wildcard-expanded
// parent: a symbolic link or other non-directory there is refused (IO_ERROR),
// while a missing name leaves the pattern simply unmatched.
Status prune_or_refuse(const Directory &parent, const std::string &name, Diagnostic &diagnostic)
{
  return exists_without_link(parent.fd.get(), name)
      ? fail(Status::IO_ERROR, diagnostic, "Glob path is a symbolic link or not a directory")
      : Status::OK;
}

// Walks the pattern below an already opened anchor. `levels` starts at the first
// wildcard (or at the leaf when every parent is literal).
class Walker
{
public:
  Walker(const std::vector<std::string> &anchor_components, const std::vector<std::string> &levels,
         uint64_t max_files, std::vector<std::string> &out, Diagnostic &diagnostic)
      : anchor_components_(anchor_components), levels_(levels), max_files_(max_files), out_(out),
        diagnostic_(diagnostic)
  {
  }

  Status run(const Directory &anchor)
  {
    if (levels_.empty()) {
      return fail(Status::INVALID, diagnostic_, "Glob pattern names no file inside secure_file_priv");
    }
    Status status = level(anchor, 0);
    if (status != Status::OK) { return status; }
    // The anchor and every directory opened below it must still resolve from
    // '/' to the same inode, without symlinks.
    Directory again;
    status = resolve(anchor_components_, again, diagnostic_);
    if (status != Status::OK || !same_identity(again.identity, anchor.identity)) {
      return fail(Status::IO_ERROR, diagnostic_, "Glob directory changed during discovery");
    }
    for (const auto &record : opened_) {
      Directory current;
      status = resolve(record.components, current, diagnostic_);
      if (status != Status::OK || !same_identity(current.identity, record.identity)) {
        return fail(Status::IO_ERROR, diagnostic_, "Glob directory changed during discovery");
      }
    }
    std::sort(out_.begin(), out_.end());
    out_.erase(std::unique(out_.begin(), out_.end()), out_.end());
    return Status::OK;
  }

private:
  struct Opened
  {
    std::vector<std::string> components;
    struct stat identity {};
  };

  Status level(const Directory &directory, size_t index)
  {
    const std::string &component = levels_[index];
    const bool literal = !has_wildcard(component);
    const bool leaf = index + 1 == levels_.size();
    // `directory` stays open while its entries are read: its identity is
    // re-checked after the recursion below, and closing it (implicitly through
    // the caller's `child`) before the stream is fully consumed would let the
    // kernel reuse the descriptor number.
    std::vector<Entry> entries;
    const size_t limit = leaf ? (max_files_ - out_.size()) : (MAX_DIRECTORIES - opened_count_);
    const Status listed = list(directory.fd.get(), component, literal, !leaf, limit, entries, diagnostic_);
    if (listed != Status::OK) { return listed; }
    if (entries.empty() && !literal && !leaf) {
      // An unmatched literal parent below a wildcard: a link or non-directory
      // there is refused, a missing name simply matches nothing.
      return prune_or_refuse(directory, component, diagnostic_);
    }
    for (const auto &entry : entries) {
      if (leaf) {
        if (out_.size() >= max_files_) {
          return fail(Status::LIMIT, diagnostic_, "Glob match count exceeds the file limit");
        }
        const std::string file = directory.path + "/" + entry.name;
        if (file.size() >= MAX_PATH_BYTES) {
          return fail(Status::LIMIT, diagnostic_, "Glob path exceeds the length limit");
        }
        out_.push_back(file);
        continue;
      }
      if (++opened_count_ > MAX_DIRECTORIES) {
        return fail(Status::LIMIT, diagnostic_, "Glob explored too many directories");
      }
      Directory child;
      const Status opened = open_directory(directory.fd.get(), directory, entry.name, child, diagnostic_);
      if (opened != Status::OK) {
        // Listed a moment ago. A wildcard skips a vanished or swapped entry; a
        // literal parent reports it.
        if (!literal) { return opened; }
        continue;
      }
      Opened record;
      record.components = child.components;
      record.identity = child.identity;
      opened_.push_back(record);
      const Status status = level(child, index + 1);
      if (status != Status::OK) { return status; }
      // The child was closed by `level`; the parent must be exactly as observed.
      struct stat after {};
      if (::fstat(directory.fd.get(), &after) != 0 || !same_identity(after, directory.identity)) {
        return fail(Status::IO_ERROR, diagnostic_, "Glob directory changed during discovery");
      }
    }
    return Status::OK;
  }

  const std::vector<std::string> &anchor_components_;
  const std::vector<std::string> &levels_;
  uint64_t max_files_;
  std::vector<std::string> &out_;
  Diagnostic &diagnostic_;
  std::vector<Opened> opened_;
  size_t opened_count_ = 1;
};
} // namespace

Status discover_files(const FilePolicy &policy, const std::string &pattern,
                      std::vector<std::string> &files, Diagnostic &diagnostic,
                      uint64_t max_files)
{
  diagnostic = {};
  if (!policy.enabled) { return fail(Status::INVALID, diagnostic, "File SQL is disabled"); }
  if (max_files == 0) { return fail(Status::LIMIT, diagnostic, "File limit must be positive"); }
  if (policy.secure_root.empty() || policy.secure_root[0] != '/') {
    return fail(Status::INVALID, diagnostic, "An absolute secure_file_priv directory is required");
  }
  std::vector<std::string> root;
  if (!split(policy.secure_root, root)) {
    return fail(Status::INVALID, diagnostic, "An absolute secure_file_priv directory is required");
  }
  std::vector<std::string> written;
  if (!split(pattern, written)) {
    return fail(Status::INVALID, diagnostic, "A safe glob pattern without '..' is required");
  }
  for (const auto &component : written) {
    if (!well_formed(component)) {
      return fail(Status::INVALID, diagnostic, "Glob pattern has an unterminated escape or bracket");
    }
  }
  // A pattern is absolute exactly when its first byte is '/'; any other
  // pattern is relative and anchored under the root. An absolute pattern must
  // lie inside the root component-wise (never a string prefix).
  const bool rooted = !pattern.empty() && pattern[0] == '/';
  std::vector<std::string> parts = written;
  if (!rooted) { parts.insert(parts.begin(), root.begin(), root.end()); }
  if (parts.size() <= root.size() || !std::equal(root.begin(), root.end(), parts.begin())) {
    return fail(Status::INVALID, diagnostic, "Glob pattern names no file inside secure_file_priv");
  }
  if (absolute_name(parts, parts.size()).size() >= MAX_PATH_BYTES) {
    return fail(Status::LIMIT, diagnostic, "Glob path exceeds the length limit");
  }
  // The anchor is every literal parent (the root, then each literal component up
  // to the first wildcard); literal levels below it are walked one by one.
  size_t anchor = parts.size() - 1;
  for (size_t i = root.size(); i + 1 < parts.size(); ++i) {
    if (has_wildcard(parts[i])) { anchor = i; break; }
  }
  Directory start;
  Status status = resolve(root, start, diagnostic);
  if (status != Status::OK) { return status; }
  const struct stat root_identity = start.identity;
  for (size_t i = root.size(); i < anchor; ++i) {
    Directory child;
    status = open_directory(start.fd.get(), start, parts[i], child, diagnostic);
    if (status != Status::OK) { return status; }
    start = std::move(child);
  }
  std::vector<std::string> anchor_components(parts.begin(), parts.begin() + static_cast<std::ptrdiff_t>(anchor));
  const std::vector<std::string> levels(parts.begin() + static_cast<std::ptrdiff_t>(anchor), parts.end());
  start.components = anchor_components;
  start.path = absolute_name(anchor_components, anchor_components.size());
  std::vector<std::string> result;
  Walker walker(anchor_components, levels, max_files, result, diagnostic);
  status = walker.run(start);
  if (status != Status::OK) { return status; }
  // The policy root must still be the directory first opened for it.
  Directory root_now;
  if (resolve(root, root_now, diagnostic) != Status::OK || !same_identity(root_now.identity, root_identity)) {
    return fail(Status::IO_ERROR, diagnostic, "secure_file_priv changed during discovery");
  }
  files = std::move(result);
  return Status::OK;
}
} // namespace oceanbase::sql::file_scan
