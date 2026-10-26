/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "file_scan_cursor.h"
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_object.h"

namespace oceanbase::sql { class ObSQLSessionInfo; }
namespace oceanbase::sql::file_scan
{
int native_status(Status status);
int diagnostic_error(Status status, const Diagnostic &diagnostic);
int session_policy(ObSQLSessionInfo &session, FilePolicy &policy);
common::ObObjType native_type(Type type);
std::vector<std::string> query_metadata(const QueryOptions &options, const std::string &fingerprint);
int query_from_metadata(const common::ObIArray<common::ObString> &metadata, QueryOptions &options);
} // namespace oceanbase::sql::file_scan
