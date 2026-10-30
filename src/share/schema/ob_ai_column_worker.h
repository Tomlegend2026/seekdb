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

#pragma once

#include "lib/thread/threads.h"
#include <atomic>
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_ai_column_task.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

class ObAiColumnWorker : public lib::Threads
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker();

  int init(common::ObMySQLProxy &proxy);
  int start();
  void stop();
  virtual void run1() override;

  static ObAiColumnWorker &get_instance();

private:
  int scan_pending_tasks(common::ObIArray<ObAiColumnTask> &tasks);
  int load_ai_column_schema(uint64_t table_id, uint64_t column_id,
                            ObAiColumnSchema &ai_col);
  int load_table_info(uint64_t table_id,
                      common::ObString &database_name,
                      common::ObString &table_name, common::ObIAllocator &allocator);
  int load_column_name(uint64_t table_id, uint64_t column_id, common::ObString &name, common::ObIAllocator &allocator);
  int process_task(const ObAiColumnTask &task);
  int call_ai_model(const ObAiColumnSchema &ai_col,
                    const common::ObString &rowkey_predicate,
                    common::ObString &result, common::ObIAllocator &allocator);
  int update_task_state(int64_t task_id, ObAiColumnTaskState state,
                        const common::ObString &result = common::ObString());
  int update_result_column(const ObAiColumnTask &task,
                           const common::ObString &result);
  bool should_retry(const ObAiColumnTask &task, int error_code);
  int64_t calculate_next_retry_time(int64_t retry_count);

private:
  std::atomic<bool> stopped_;
  common::ObMySQLProxy *sql_proxy_;
  static const int64_t WORKER_INTERVAL_US = 1000 * 1000; // 1 second
  static const int64_t MAX_RETRY_COUNT = 3;
};

} // namespace schema
} // namespace share
} // namespace oceanbase
