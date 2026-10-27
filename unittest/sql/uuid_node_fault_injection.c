/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
/* Linux-only preload fixture. Never load this library in a production server. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static _Thread_local int enumeration_seen;
static _Thread_local int entropy_fd = -1;
static _Thread_local int read_calls;

static const char *fault_mode(void)
{
  const char *mode = getenv("SEEKDB_UUID_TEST_MODE");
  return mode == NULL ? "deny" : mode;
}

int ioctl(int fd, unsigned long request, ...)
{
  va_list args;
  va_start(args, request);
  void *argument = va_arg(args, void *);
  va_end(args);
  if (request == SIOCGIFCONF && strcmp(fault_mode(), "normal") != 0) {
    static const char marker[] = "UUID_TEST: forced interface discovery\n";
    (void)write(STDERR_FILENO, marker, sizeof(marker) - 1);
    enumeration_seen = 1;
    if (strcmp(fault_mode(), "empty") == 0) {
      ((struct ifconf *)argument)->ifc_len = 0;
      return 0;
    }
    errno = EPERM;
    return -1;
  }
  int (*real_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
  return real_ioctl(fd, request, argument);
}

static int open_source(const char *path, int flags, mode_t permissions, const char *symbol)
{
  const int is_entropy = enumeration_seen && strcmp(path, "/dev/urandom") == 0;
  if (is_entropy && strcmp(fault_mode(), "entropy-denied") == 0) {
    errno = EACCES;
    return -1;
  }
  int (*real_open)(const char *, int, ...) = dlsym(RTLD_NEXT, symbol);
  const int fd = real_open(path, flags, permissions);
  if (is_entropy && fd >= 0) {
    entropy_fd = fd;
    read_calls = 0;
  }
  return fd;
}

#define DEFINE_OPEN(name) \
  int name(const char *path, int flags, ...) \
  { \
    mode_t permissions = 0; \
    if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) { \
      va_list args; \
      va_start(args, flags); \
      permissions = va_arg(args, mode_t); \
      va_end(args); \
    } \
    return open_source(path, flags, permissions, #name); \
  }

DEFINE_OPEN(open)
DEFINE_OPEN(open64)

ssize_t read(int fd, void *buffer, size_t length)
{
  if (fd == entropy_fd) {
    if (strcmp(fault_mode(), "entropy-eof") == 0) { return 0; }
    if (strcmp(fault_mode(), "entropy-partial") == 0) {
      if (read_calls++ == 0) { errno = EINTR; return -1; }
      if (length > 2) { length = 2; }
    }
  }
  ssize_t (*real_read)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
  return real_read(fd, buffer, length);
}

int close(int fd)
{
  if (fd == entropy_fd) { entropy_fd = -1; }
  int (*real_close)(int) = dlsym(RTLD_NEXT, "close");
  return real_close(fd);
}
