/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_file.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace oceanbase::sql::file_scan
{
namespace
{
bool split_path(const std::string &path, std::vector<std::string> &parts)
{
  if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos) { return false; }
  for (size_t begin = 0; begin < path.size();) {
    size_t end = path.find('/', begin);
    if (end == std::string::npos) { end = path.size(); }
    const auto part = path.substr(begin, end - begin);
    if (part == ".." || part.size() > 255) { return false; }
    if (!part.empty() && part != ".") { parts.push_back(part); }
    begin = end + 1;
  }
  return true;
}

std::string absolute_name(const std::vector<std::string> &parts)
{
  std::string result;
  for (const auto &part : parts) { result += "/" + part; }
  return result.empty() ? "/" : result;
}

// Opening one component at a time avoids realpath/open TOCTOU races and
// O_NONBLOCK ensures an attacker-created FIFO cannot hang the SQL worker.
int open_regular(const std::string &absolute)
{
  std::vector<std::string> parts;
  if (absolute.empty() || absolute[0] != '/' || !split_path(absolute, parts) || parts.empty()) {
    return -1;
  }
  int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  for (size_t i = 0; current >= 0 && i < parts.size(); ++i) {
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
        | (i + 1 < parts.size() ? O_DIRECTORY : 0);
    const int next = ::openat(current, parts[i].c_str(), flags);
    ::close(current);
    current = next;
  }
  return current;
}

bool same_file(const struct stat &left, const struct stat &right)
{
  return S_ISREG(right.st_mode) && left.st_dev == right.st_dev && left.st_ino == right.st_ino
      && left.st_size == right.st_size && left.st_nlink == right.st_nlink
      && left.st_mtim.tv_sec == right.st_mtim.tv_sec && left.st_mtim.tv_nsec == right.st_mtim.tv_nsec
      && left.st_ctim.tv_sec == right.st_ctim.tv_sec && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}
} // namespace

SecureFile::SecureFile() : buffer_(*this), stream_(&buffer_) {}
SecureFile::~SecureFile() { close(); }

void SecureFile::close()
{
  if (descriptor_ >= 0) { ::close(descriptor_); }
  if (changes_ >= 0) { ::close(changes_); }
  descriptor_ = -1;
  changes_ = -1;
  buffer_.reset();
  stream_.clear();
  absolute_path_.clear();
}

Status SecureFile::fail(Status status, const char *message)
{
  diagnostic_.message = message;
  status_ = status;
  return status;
}

Status SecureFile::open(const FilePolicy &policy, const std::string &path)
{
  close();
  status_ = Status::OK;
  diagnostic_ = {};
  if (!policy.enabled) { return fail(Status::INVALID, "File SQL is disabled"); }
  std::vector<std::string> root;
  std::vector<std::string> file;
  if (policy.secure_root.empty() || policy.secure_root[0] != '/'
      || !split_path(policy.secure_root, root) || !split_path(path, file)) {
    return fail(Status::INVALID, "An absolute secure_file_priv root and valid path are required");
  }
  if (path[0] != '/') {
    file.insert(file.begin(), root.begin(), root.end());
  } else {
    if (file.size() <= root.size()) { return fail(Status::INVALID, "File path is outside secure_file_priv"); }
    for (size_t i = 0; i < root.size(); ++i) {
      if (file[i] != root[i]) { return fail(Status::INVALID, "File path is outside secure_file_priv"); }
    }
  }
  absolute_path_ = absolute_name(file);
  max_bytes_ = policy.max_file_bytes;
  descriptor_ = open_regular(absolute_path_);
  // Watch the descriptor's inode, not a path that could already have changed.
  // Install before taking the baseline stat to cover same-size writes within a
  // filesystem timestamp tick; event queue overflow is also a hard failure.
  changes_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  const auto descriptor_path = "/proc/self/fd/" + std::to_string(descriptor_);
  if (descriptor_ < 0 || changes_ < 0
      || ::inotify_add_watch(changes_, descriptor_path.c_str(),
          IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT) < 0
      || ::fstat(descriptor_, &identity_) != 0 || !S_ISREG(identity_.st_mode)) {
    close();
    return fail(Status::IO_ERROR, "Cannot open a regular file without following symlinks");
  }
  if (identity_.st_size < 0 || static_cast<uint64_t>(identity_.st_size) > max_bytes_) {
    close();
    return fail(Status::LIMIT, "File byte limit exceeded");
  }
  return validate();
}

Status SecureFile::validate()
{
  if (status_ != Status::OK) { return status_; }
  alignas(struct inotify_event) char event_buffer[4096];
  ssize_t changes = -1;
  do { changes = ::read(changes_, event_buffer, sizeof(event_buffer)); } while (changes < 0 && errno == EINTR);
  if (changes > 0) { return fail(Status::IO_ERROR, "File changed during schema inference or scan"); }
  if (changes == 0 || (changes < 0 && errno != EAGAIN)) {
    return fail(Status::IO_ERROR, "Cannot monitor input file changes");
  }
  struct stat descriptor_identity {};
  struct stat path_identity {};
  if (descriptor_ < 0 || ::fstat(descriptor_, &descriptor_identity) != 0) {
    return fail(Status::IO_ERROR, "File descriptor is not available");
  }
  const int current = open_regular(absolute_path_);
  const bool path_ok = current >= 0 && ::fstat(current, &path_identity) == 0;
  if (current >= 0) { ::close(current); }
  if (!path_ok || !same_file(identity_, descriptor_identity) || !same_file(identity_, path_identity)) {
    return fail(Status::IO_ERROR, "File changed during schema inference or scan");
  }
  return Status::OK;
}

Status SecureFile::rewind()
{
  if (validate() != Status::OK) { return status_; }
  if (::lseek(descriptor_, 0, SEEK_SET) < 0) { return fail(Status::IO_ERROR, "Cannot rewind input file"); }
  buffer_.reset();
  stream_.clear();
  return Status::OK;
}

SecureFile::Buffer::int_type SecureFile::Buffer::underflow()
{
  if (gptr() < egptr()) { return traits_type::to_int_type(*gptr()); }
  if (owner_.validate() != Status::OK) { throw std::ios_base::failure(owner_.diagnostic_.message); }
  ssize_t count = -1;
  do {
    count = ::read(owner_.descriptor_, bytes_.data(), bytes_.size());
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    owner_.fail(Status::IO_ERROR, "Cannot read input file");
    throw std::ios_base::failure(owner_.diagnostic_.message);
  }
  // Detect writes racing with read before making any newly read bytes visible.
  if (owner_.validate() != Status::OK) { throw std::ios_base::failure(owner_.diagnostic_.message); }
  if (count == 0) { return traits_type::eof(); }
  setg(bytes_.data(), bytes_.data(), bytes_.data() + count);
  return traits_type::to_int_type(*gptr());
}
} // namespace oceanbase::sql::file_scan
