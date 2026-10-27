/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "file_scan_utils.h"
#include <array>
#include <streambuf>
#include <sys/stat.h>

namespace oceanbase::sql::file_scan
{
struct FilePolicy
{
  bool enabled = false;
  std::string secure_root;
  uint64_t max_file_bytes = 1024ULL * 1024 * 1024;
};

// Read-only, descriptor-backed stream. Every path component is opened without
// following symlinks. Revalidation compares the open descriptor AND current path
// against the original identity before rewinds and each buffered read.
class SecureFile : public RandomAccessInput
{
public:
  SecureFile();
  ~SecureFile();
  SecureFile(const SecureFile &) = delete;
  SecureFile &operator=(const SecureFile &) = delete;
  Status open(const FilePolicy &policy, const std::string &path);
  Status validate();
  Status rewind();
  std::string fingerprint() const;
  void close();
  std::istream &stream() { return stream_; }
  const Diagnostic &diagnostic() const { return diagnostic_; }

  // RandomAccessInput: the descriptor is read without disturbing the
  // sequential stream position.
  uint64_t size() const override { return identity_.st_size < 0 ? 0 : static_cast<uint64_t>(identity_.st_size); }
  Status read_at(uint64_t offset, char *buffer, uint64_t buffer_size,
                 Diagnostic &diagnostic) override;

private:
  class Buffer : public std::streambuf
  {
  public:
    explicit Buffer(SecureFile &owner) : owner_(owner) { reset(); }
    void reset() { setg(bytes_.data(), bytes_.data(), bytes_.data()); }
  protected:
    int_type underflow() override;
  private:
    SecureFile &owner_;
    std::array<char, 64 * 1024> bytes_;
  };
  Status fail(Status status, const char *message);
  int descriptor_ = -1;
  int changes_ = -1;
  struct stat identity_ {};
  std::string absolute_path_;
  uint64_t max_bytes_ = 0;
  uint64_t content_hash_ = 14695981039346656037ULL;
  Status status_ = Status::OK;
  Diagnostic diagnostic_;
  Buffer buffer_;
  std::istream stream_;
};
} // namespace oceanbase::sql::file_scan
