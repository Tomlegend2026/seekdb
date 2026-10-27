/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_native.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "lib/oblog/ob_warning_buffer.h"

namespace oceanbase::sql::file_scan
{
using namespace common;
int native_status(Status status)
{
  switch (status) {
    case Status::OK: return OB_SUCCESS;
    case Status::END: return OB_ITER_END;
    case Status::LIMIT: return OB_SIZE_OVERFLOW;
    case Status::IO_ERROR: return OB_IO_ERROR;
    default: return OB_INVALID_ARGUMENT;
  }
}

int diagnostic_error(Status status, const Diagnostic &diagnostic)
{
  const int result = native_status(status);
  if (result != OB_SUCCESS && result != OB_ITER_END) {
    const std::string message = "FileSQL record " + std::to_string(diagnostic.record)
        + ", line " + std::to_string(diagnostic.line) + ": " + diagnostic.message;
    if (auto *warnings = ob_get_tsi_warning_buffer(); warnings != nullptr) {
      warnings->set_error(message.c_str(), result);
    }
  }
  return result;
}

int session_policy(ObSQLSessionInfo &session, FilePolicy &policy)
{
  int ret = OB_SUCCESS;
  ObString root;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, policy.enabled))) {
  } else if (!policy.enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL while enable_file_sql is OFF");
  } else if ((session.get_user_priv_set() & OB_PRIV_FILE) == 0) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (OB_FAIL(session.get_secure_file_priv(root))) {
  } else {
    if (root.empty()) { ret = OB_ERR_NO_PRIVILEGE; }
    else { policy.secure_root.assign(root.ptr(), root.length()); }
  }
  return ret;
}

ObObjType native_type(Type type)
{
  switch (type) {
    case Type::BIGINT: return ObIntType;
    case Type::DOUBLE: return ObDoubleType;
    case Type::BOOLEAN: return ObTinyIntType;
    case Type::DATE: return ObDateType;
    case Type::DATETIME: return ObDateTimeType;
    default: return ObVarcharType;
  }
}

std::vector<std::string> query_metadata(const QueryOptions &options, const std::string &fingerprint)
{
  return {options.glob ? "glob" : options.json ? "jsonl" : "csv", options.path, options.csv.header ? "1" : "0",
      std::string(1, options.csv.delimiter), std::string(1, options.csv.quote),
      std::string(1, options.csv.escape), options.csv.null_text, fingerprint};
}

int query_from_metadata(const ObIArray<ObString> &metadata, QueryOptions &options)
{
  if (metadata.count() != 8) { return OB_INVALID_ARGUMENT; }
  const auto value = [&metadata](int index) {
    return std::string(metadata.at(index).ptr(), metadata.at(index).length());
  };
  if ((value(0) != "csv" && value(0) != "jsonl" && value(0) != "glob") || (value(2) != "0" && value(2) != "1")
      || metadata.at(3).length() != 1 || metadata.at(4).length() != 1 || metadata.at(5).length() != 1) {
    return OB_INVALID_ARGUMENT;
  }
  options.json = value(0) == "jsonl";
  options.glob = value(0) == "glob";
  options.path = value(1);
  options.csv.header = value(2) == "1";
  options.csv.delimiter = metadata.at(3).ptr()[0];
  options.csv.quote = metadata.at(4).ptr()[0];
  options.csv.escape = metadata.at(5).ptr()[0];
  options.csv.null_text = value(6);
  return OB_SUCCESS;
}
} // namespace oceanbase::sql::file_scan
