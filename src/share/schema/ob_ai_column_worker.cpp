/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SHARE_SCHEMA
#include "share/schema/ob_ai_column_worker.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/time/ob_time_utility.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace share
{
namespace schema
{

ObAiColumnWorker::ObAiColumnWorker()
    : stopped_(true), sql_proxy_(NULL)
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
  stop();
}

ObAiColumnWorker &ObAiColumnWorker::get_instance()
{
  static ObAiColumnWorker instance;
  return instance;
}

int ObAiColumnWorker::init(ObMySQLProxy &proxy)
{
  int ret = OB_SUCCESS;
  if (!stopped_) { ret = OB_INIT_TWICE; }
  else { sql_proxy_ = &proxy; }
  return ret;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!stopped_) {
    LOG_WARN("worker already started");
    ret = OB_INIT_TWICE;
  } else if (nullptr == sql_proxy_) {
    ret = OB_NOT_INIT;
  } else {
    stopped_ = false;
    if (OB_FAIL(lib::Threads::start())) {
      LOG_WARN("failed to start thread", K(ret));
      stopped_ = true;
    } else {
      LOG_INFO("AI column worker started");
    }
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  if (!stopped_) {
    stopped_ = true;
    lib::Threads::stop();
    lib::Threads::wait();
    LOG_INFO("AI column worker stopped");
  }
}

void ObAiColumnWorker::run1()
{
  int ret = OB_SUCCESS;
  while (!stopped_) {
    ObSEArray<ObAiColumnTask, 16> tasks;
    if (OB_FAIL(scan_pending_tasks(tasks))) {
      LOG_WARN("failed to scan pending tasks", K(ret));
    } else {
      for (int64_t i = 0; i < tasks.count() && !stopped_; ++i) {
        if (OB_FAIL(process_task(tasks.at(i)))) {
          LOG_WARN("failed to process task", K(ret), "task_id", tasks.at(i).task_id_);
        }
      }
    }
    usleep(WORKER_INTERVAL_US);
  }
}

int ObAiColumnWorker::scan_pending_tasks(ObIArray<ObAiColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  tasks.reset();

  if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("sql proxy not initialized", K(ret));
  } else {
    int64_t now_us = ObTimeUtility::current_time();
    ObSqlString sql;
    if (OB_FAIL(sql.assign_fmt(
        "SELECT task_id, table_id, column_id, request_id, rowkey_predicate, "
        "state, retry_count, CAST(UNIX_TIMESTAMP(eligible_ts)*1000000 AS SIGNED) AS eligible_ts, definition_version "
        "FROM oceanbase.__all_ai_column_task "
        "WHERE state = %d AND (eligible_ts IS NULL OR eligible_ts <= CURRENT_TIMESTAMP(6)) "
        "ORDER BY eligible_ts ASC LIMIT 100",
        static_cast<int>(AI_TASK_PENDING)))) {
      LOG_WARN("failed to assign sql", K(ret));
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        if (OB_FAIL(sql_proxy_->read(res, sql.ptr()))) {
          LOG_WARN("failed to execute sql", K(ret), K(sql));
        } else {
          sqlclient::ObMySQLResult &result = *res.get_result();
          while (OB_SUCC(ret) && OB_SUCC(ret = result.next())) {
            ObAiColumnTask task;
            EXTRACT_INT_FIELD_MYSQL(result, "task_id", task.task_id_, int64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "table_id", task.table_id_, uint64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "column_id", task.column_id_, uint64_t);
            EXTRACT_VARCHAR_FIELD_MYSQL(result, "request_id", task.request_id_);
            EXTRACT_VARCHAR_FIELD_MYSQL(result, "rowkey_predicate", task.rowkey_predicate_);
            EXTRACT_INT_FIELD_MYSQL(result, "state", task.state_, int);
            EXTRACT_INT_FIELD_MYSQL(result, "retry_count", task.retry_count_, int64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "eligible_ts", task.eligible_ts_, int64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "definition_version", task.definition_version_, int64_t);
            if (OB_FAIL(ret)) {
              LOG_WARN("failed to extract field", K(ret));
            } else if (OB_FAIL(tasks.push_back(task))) {
              LOG_WARN("failed to push back task", K(ret));
            }
          }
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::load_ai_column_schema(uint64_t table_id, uint64_t column_id,
                                             ObAiColumnSchema &ai_col)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("sql proxy not initialized", K(ret));
  } else {
    ObSqlString query_sql;
    if (OB_FAIL(query_sql.assign_fmt(
        "SELECT table_id, column_id, column_name, ai_model_name, ai_func_type, "
        "normalized_expr, max_retries, sync_mode, definition_version, "
        "state_column_id, request_column_id, result_spec_column_id, "
        "result_type, result_dimension "
        "FROM oceanbase.__all_ai_column "
        "WHERE table_id = %lu AND column_id = %lu",
        table_id, column_id))) {
      LOG_WARN("failed to assign query sql", K(ret));
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        if (OB_FAIL(sql_proxy_->read(res, query_sql.ptr()))) {
          LOG_WARN("failed to query ai column schema", K(ret), K(query_sql));
        } else {
          sqlclient::ObMySQLResult &result = *res.get_result();
          if (OB_SUCC(result.next())) {
            uint64_t tid, cid;
            int64_t func_type, retries, version, state_col, req_col, res_spec_col, res_type, res_dim;
            ObString col_name, model_name, norm_expr, sync_mode;

            EXTRACT_INT_FIELD_MYSQL(result, "table_id", tid, uint64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "column_id", cid, uint64_t);
            EXTRACT_VARCHAR_FIELD_MYSQL(result, "column_name", col_name);
            EXTRACT_VARCHAR_FIELD_MYSQL(result, "ai_model_name", model_name);
            EXTRACT_INT_FIELD_MYSQL(result, "ai_func_type", func_type, int64_t);
            EXTRACT_VARCHAR_FIELD_MYSQL(result, "normalized_expr", norm_expr);
            EXTRACT_INT_FIELD_MYSQL(result, "max_retries", retries, int64_t);
            EXTRACT_VARCHAR_FIELD_MYSQL(result, "sync_mode", sync_mode);
            EXTRACT_INT_FIELD_MYSQL(result, "definition_version", version, int64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "state_column_id", state_col, uint64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "request_column_id", req_col, uint64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "result_spec_column_id", res_spec_col, uint64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "result_type", res_type, int64_t);
            EXTRACT_INT_FIELD_MYSQL(result, "result_dimension", res_dim, int64_t);

            if (OB_SUCC(ret)) {
              ai_col.set_table_id(tid);
              ai_col.set_column_id(cid);
              if (OB_FAIL(ai_col.set_column_name(col_name))) {
                LOG_WARN("failed to set column name", K(ret));
              } else if (OB_FAIL(ai_col.set_ai_model_name(model_name))) {
                LOG_WARN("failed to set ai model name", K(ret));
              } else {
                ai_col.set_ai_func_type(func_type);
                if (OB_FAIL(ai_col.set_normalized_expr(norm_expr))) {
                  LOG_WARN("failed to set normalized expr", K(ret));
                } else {
                  ai_col.set_max_retries(retries);
                  if (OB_FAIL(ai_col.set_sync_mode(sync_mode))) {
                    LOG_WARN("failed to set sync mode", K(ret));
                  } else {
                    ai_col.set_definition_version(version);
                    ai_col.set_state_column_id(state_col);
                    ai_col.set_request_column_id(req_col);
                    ai_col.set_result_spec_column_id(res_spec_col);
                    ai_col.set_result_type(res_type);
                    ai_col.set_result_dimension(res_dim);
                  }
                }
              }
            }
          } else {
            ret = OB_ENTRY_NOT_EXIST;
            LOG_WARN("ai column not found", K(table_id), K(column_id));
          }
        }
      }
    }
  }
  return ret;
}

namespace {
int append_ai_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (name.empty()) { return OB_INVALID_ARGUMENT; }
  OZ(sql.append("`"));
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { OZ(sql.append("``")); }
    else { OZ(sql.append(name.ptr() + i, 1)); }
  }
  OZ(sql.append("`"));
  return ret;
}
}

int ObAiColumnWorker::load_table_info(uint64_t table_id, ObString &database_name,
                                     ObString &table_name, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObISQLClient::ReadResult res;
  ObString raw_database, raw_table;
  if (nullptr == sql_proxy_) { return OB_NOT_INIT; }
  OZ(sql.append_fmt("SELECT d.database_name,t.table_name FROM oceanbase.__all_table t "
                    "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
                    "WHERE t.table_id=%lu", table_id));
  OZ(sql_proxy_->read(res, sql.ptr()));
  if (OB_SUCC(ret) && nullptr == res.get_result()) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = res.get_result()->next(); }
  if (ret == OB_ITER_END) { ret = OB_ENTRY_NOT_EXIST; }
  OZ(res.get_result()->get_varchar("database_name", raw_database));
  OZ(res.get_result()->get_varchar("table_name", raw_table));
  OZ(ob_write_string(allocator, raw_database, database_name));
  OZ(ob_write_string(allocator, raw_table, table_name));
  return ret;
}
int ObAiColumnWorker::load_column_name(uint64_t table_id, uint64_t column_id,
                                      ObString &name, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (nullptr == sql_proxy_) { return OB_NOT_INIT; }
  ObSqlString sql;
  ObISQLClient::ReadResult result;
  ObString value;
  OZ(sql.append_fmt("SELECT column_name FROM oceanbase.__all_column WHERE table_id=%lu AND column_id=%lu",
                    table_id, column_id));
  OZ(sql_proxy_->read(result, sql.ptr()));
  if (OB_SUCC(ret) && nullptr == result.get_result()) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = result.get_result()->next(); }
  if (ret == OB_ITER_END) { ret = OB_ENTRY_NOT_EXIST; }
  OZ(result.get_result()->get_varchar(static_cast<int64_t>(0), value));
  OZ(ob_write_string(allocator, value, name));
  return ret;
}

namespace {
int append_ai_literal(ObSqlString &sql, const ObString &value, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (value.length() > 16 * 1024 * 1024) { return OB_SIZE_OVERFLOW; }
  const int64_t size = static_cast<int64_t>(value.length()) * 2;
  char *hex = static_cast<char *>(allocator.alloc(size + 1));
  if (nullptr == hex) { return OB_ALLOCATE_MEMORY_FAILED; }
  const char *digits = "0123456789abcdef";
  for (int64_t i = 0; i < value.length(); ++i) {
    const unsigned char byte = static_cast<unsigned char>(value.ptr()[i]);
    hex[2 * i] = digits[byte >> 4];
    hex[2 * i + 1] = digits[byte & 15];
  }
  OZ(sql.append("CONVERT(X'"));
  OZ(sql.append(hex, size));
  OZ(sql.append("' USING utf8mb4)"));
  return ret;
}
}
int ObAiColumnWorker::process_task(const ObAiColumnTask &task)
{
  int ret = OB_SUCCESS;

  // 1. Update state to RUNNING
  if (OB_FAIL(update_task_state(task.task_id_, AI_TASK_RUNNING))) {
    LOG_WARN("failed to update task state to RUNNING", K(ret));
  } else {
    // 2. Load AI column schema from catalog
    ObAiColumnSchema ai_col;
    if (OB_FAIL(load_ai_column_schema(task.table_id_, task.column_id_, ai_col))) {
      LOG_WARN("failed to load ai column schema", K(ret),
               K(task.table_id_), K(task.column_id_));
      // Mark task as failed if we can't load schema
      if (OB_FAIL(update_task_state(task.task_id_, AI_TASK_FAILED))) {
        LOG_WARN("failed to update task state to FAILED", K(ret));
      }
    } else {
      // 3. Call AI model
      ObArenaAllocator result_allocator;
      ObString result;
      int call_ret = call_ai_model(ai_col, task.rowkey_predicate_, result, result_allocator);

      // 4. Update result or handle error
      if (OB_SUCCESS == call_ret) {
        if (OB_FAIL(update_result_column(task, result))) {
          LOG_WARN("failed to update result column", K(ret));
          if (OB_FAIL(update_task_state(task.task_id_, AI_TASK_FAILED))) {
            LOG_WARN("failed to update task state to FAILED", K(ret));
          }
        } else {
          if (OB_FAIL(update_task_state(task.task_id_, AI_TASK_COMPLETED, result))) {
            LOG_WARN("failed to update task state to COMPLETED", K(ret));
          }
        }
      } else {
        LOG_WARN("failed to call AI model", K(call_ret));
        if (task.retry_count_ < ai_col.get_max_retries()) {
          int64_t next_retry_time = calculate_next_retry_time(task.retry_count_);
          ObSqlString update_sql;
          if (OB_FAIL(update_sql.assign_fmt(
              "UPDATE oceanbase.__all_ai_column_task "
              "SET state = %d, retry_count = retry_count + 1, eligible_ts = FROM_UNIXTIME(%ld / 1000000.0) "
              "WHERE task_id = %ld",
              static_cast<int>(AI_TASK_PENDING),
              next_retry_time,
              task.task_id_))) {
            LOG_WARN("failed to assign update sql", K(ret));
          } else if (OB_ISNULL(sql_proxy_)) {
            ret = OB_NOT_INIT;
          } else {
            int64_t affected_rows = 0;
            if (OB_FAIL(sql_proxy_->write(update_sql.ptr(), affected_rows))) {
              LOG_WARN("failed to update task for retry", K(ret));
            }
          }
        } else {
          if (OB_FAIL(update_task_state(task.task_id_, AI_TASK_FAILED))) {
            LOG_WARN("failed to update task state to FAILED", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::call_ai_model(const ObAiColumnSchema &ai_col,
                                   const ObString &rowkey_predicate,
                                   ObString &result, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  result.reset();
  if (nullptr == sql_proxy_) { return OB_NOT_INIT; }
  if (rowkey_predicate.empty() || ai_col.get_normalized_expr().empty()) {
    return OB_INVALID_ARGUMENT;
  }
  ObString database_name, table_name, value;
  ObSqlString sql;
  ObISQLClient::ReadResult response;
  OZ(load_table_info(ai_col.get_table_id(), database_name, table_name, allocator));
  // The kernel's existing AI functions own endpoint selection, request formatting,
  // HTTP errors and response decoding. Evaluate the stored SQL against the row.
  OZ(sql.append_fmt("SELECT CAST((%.*s) AS CHAR) FROM ",
                    ai_col.get_normalized_expr().length(), ai_col.get_normalized_expr().ptr()));
  OZ(append_ai_identifier(sql, database_name));
  OZ(sql.append("."));
  OZ(append_ai_identifier(sql, table_name));
  OZ(sql.append_fmt(" WHERE (%.*s) LIMIT 1", rowkey_predicate.length(), rowkey_predicate.ptr()));
  OZ(sql_proxy_->read(response, sql.ptr()));
  if (OB_SUCC(ret) && nullptr == response.get_result()) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = response.get_result()->next(); }
  if (ret == OB_ITER_END) { ret = OB_ENTRY_NOT_EXIST; }
  OZ(response.get_result()->get_varchar(static_cast<int64_t>(0), value));
  OZ(ob_write_string(allocator, value, result));
  return ret;
}
int ObAiColumnWorker::update_task_state(int64_t task_id, ObAiColumnTaskState state,
                                       const ObString &result)
{
  UNUSED(result);
  int ret = OB_SUCCESS;
  if (nullptr == sql_proxy_) { return OB_NOT_INIT; }
  ObSqlString sql;
  int64_t affected = 0;
  OZ(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%d WHERE task_id=%ld",
                    static_cast<int>(state), task_id));
  if (state == AI_TASK_RUNNING) { OZ(sql.append_fmt(" AND state=%d", static_cast<int>(AI_TASK_PENDING))); }
  OZ(sql_proxy_->write(sql.ptr(), affected));
  if (OB_SUCC(ret) && affected != 1) { ret = OB_EAGAIN; }
  return ret;
}
int ObAiColumnWorker::update_result_column(const ObAiColumnTask &task, const ObString &result)
{
  int ret = OB_SUCCESS;
  if (nullptr == sql_proxy_) { return OB_NOT_INIT; }
  ObArenaAllocator allocator;
  ObAiColumnSchema definition(&allocator);
  ObString database, table, state_column, request_column;
  ObSqlString sql;
  int64_t affected = 0;
  OZ(load_ai_column_schema(task.table_id_, task.column_id_, definition));
  if (OB_SUCC(ret) && definition.get_definition_version() != task.definition_version_) { ret = OB_EAGAIN; }
  OZ(load_table_info(task.table_id_, database, table, allocator));
  OZ(load_column_name(task.table_id_, definition.get_state_column_id(), state_column, allocator));
  OZ(load_column_name(task.table_id_, definition.get_request_column_id(), request_column, allocator));
  OZ(sql.append("UPDATE "));
  OZ(append_ai_identifier(sql, database));
  OZ(sql.append("."));
  OZ(append_ai_identifier(sql, table));
  OZ(sql.append(" SET "));
  OZ(append_ai_identifier(sql, definition.get_column_name()));
  OZ(sql.append("="));
  OZ(append_ai_literal(sql, result, allocator));
  OZ(sql.append(","));
  OZ(append_ai_identifier(sql, state_column));
  OZ(sql.append("='READY' WHERE ("));
  OZ(sql.append(task.rowkey_predicate_));
  OZ(sql.append(") AND "));
  OZ(append_ai_identifier(sql, request_column));
  OZ(sql.append("="));
  OZ(append_ai_literal(sql, task.request_id_, allocator));
  OZ(sql.append_fmt(" AND EXISTS (SELECT 1 FROM oceanbase.__all_ai_column a "
                    "WHERE a.table_id=%lu AND a.column_id=%lu AND a.definition_version=%ld) "
                    "AND EXISTS (SELECT 1 FROM oceanbase.__all_ai_column_task t "
                    "WHERE t.task_id=%lu AND t.state=%d AND t.request_id=",
                    task.table_id_, task.column_id_, task.definition_version_,
                    task.task_id_, static_cast<int>(AI_TASK_RUNNING)));
  OZ(append_ai_literal(sql, task.request_id_, allocator));
  OZ(sql.append(")"));
  OZ(sql_proxy_->write(sql.ptr(), affected));
  if (OB_SUCC(ret) && affected != 1) { ret = OB_EAGAIN; }
  return ret;
}
bool ObAiColumnWorker::should_retry(const ObAiColumnTask &task, int error_code)
{
  bool should_retry = false;
  UNUSED(error_code);

  if (task.retry_count_ < MAX_RETRY_COUNT) {
    should_retry = true;
  }

  return should_retry;
}

int64_t ObAiColumnWorker::calculate_next_retry_time(int64_t retry_count)
{
  int64_t now_us = ObTimeUtility::current_time();
  int64_t delay_us = (1LL << std::min<int64_t>(6, std::max<int64_t>(0, retry_count))) * 1000 * 1000; // Exponential backoff: 1s, 2s, 4s, ...
  return now_us + delay_us;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
