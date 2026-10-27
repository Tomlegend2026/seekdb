/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "file_scan_file.h"
#include "file_scan_utils.h"
#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase::sql::file_scan
{
// Expands glob('pattern') to the regular files under FilePolicy::secure_root.
//   * A disabled policy or an empty, relative or '..'-containing root is
//     INVALID; "/" is a valid root. A relative pattern is anchored under the
//     root, and an absolute pattern must lie inside it (component-wise).
//   * Patterns are normalized like the secure file layer: redundant '/', '.'
//     and a trailing '/' are dropped. A NUL byte or a '..' component is
//     INVALID, as is a pattern that names no file below the root.
//   * Each component is matched with POSIX fnmatch(3) + FNM_PERIOD: a leading
//     '.' needs an explicit dot, '\' escapes, '[...]' is a class, and '*'/'?'
//     never cross '/'. "**" is an ordinary one-component pattern (not recursive).
//     Non-wildcard components are matched literally (no fnmatch interpretation).
//   * Only regular files are returned, sorted and unique. Symbolic links
//     (files, parents and the root itself), directories, FIFOs and devices are
//     never followed: a wildcard skips them, and a literal parent that is a
//     symbolic link is IO_ERROR. A missing or non-directory literal parent is
//     INVALID. No match is OK with an empty list.
//   * Both match count (max_files) and directory exploration
//     (kMaxDirectoriesPerCall) are bounded: exceeding either is LIMIT.
//   * Paths are resolved one component at a time from '/' with O_NOFOLLOW. The
//     parents that produced the result are re-opened and compared by identity
//     before publishing, so a swapped or symlinked ancestor yields IO_ERROR.
//   * On any non-OK status `files` is left untouched; nothing is published
//     partially and no descriptor outlives the call.
Status discover_files(const FilePolicy &policy, const std::string &pattern,
                      std::vector<std::string> &files, Diagnostic &diagnostic,
                      uint64_t max_files = 1024);
} // namespace oceanbase::sql::file_scan
