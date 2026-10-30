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

#include "share/schema/ob_schema_struct.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

class ObAiColumnSchema;
class ObTableSchema;

enum ObAiColumnTaskState
{
  AI_TASK_PENDING = 0,
  AI_TASK_RUNNING = 1,
  AI_TASK_COMPLETED = 2,
  AI_TASK_FAILED = 3
};

class ObAiColumnTask : public ObSchema
{
  friend class ObAiColumnWorker;
public:
  ObAiColumnTask() { reset(); }
  ~ObAiColumnTask() = default;

  bool is_valid() const override { return task_id_ != 0 && table_id_ != 0 && column_id_ != 0; }

  int assign(const ObAiColumnTask &other)
  {
    int ret = common::OB_SUCCESS;
    if (this != &other) {
      task_id_ = other.task_id_;
      table_id_ = other.table_id_;
      column_id_ = other.column_id_;
      state_ = other.state_;
      retry_count_ = other.retry_count_;
      eligible_ts_ = other.eligible_ts_;
      definition_version_ = other.definition_version_;
      if (OB_FAIL(set_request_id(other.request_id_))) {
      } else if (OB_FAIL(set_rowkey_predicate(other.rowkey_predicate_))) {
      }
    }
    return ret;
  }

  void reset()
  {
    task_id_ = 0;
    table_id_ = 0;
    column_id_ = 0;
    request_id_.reset();
    rowkey_predicate_.reset();
    state_ = AI_TASK_PENDING;
    retry_count_ = 0;
    eligible_ts_ = 0;
    definition_version_ = 0;
  }

  OB_INLINE uint64_t get_task_id() const { return task_id_; }
  OB_INLINE uint64_t get_table_id() const { return table_id_; }
  OB_INLINE uint64_t get_column_id() const { return column_id_; }
  OB_INLINE common::ObString get_request_id() const { return request_id_; }
  OB_INLINE common::ObString get_rowkey_predicate() const { return rowkey_predicate_; }
  OB_INLINE int64_t get_state() const { return state_; }
  OB_INLINE int64_t get_retry_count() const { return retry_count_; }
  OB_INLINE int64_t get_eligible_ts() const { return eligible_ts_; }
  OB_INLINE int64_t get_definition_version() const { return definition_version_; }

  OB_INLINE void set_task_id(uint64_t id) { task_id_ = id; }
  OB_INLINE void set_table_id(uint64_t id) { table_id_ = id; }
  OB_INLINE void set_column_id(uint64_t id) { column_id_ = id; }
  OB_INLINE int set_request_id(const common::ObString &id) { return deep_copy_str(id, request_id_); }
  OB_INLINE int set_rowkey_predicate(const common::ObString &pred) { return deep_copy_str(pred, rowkey_predicate_); }
  OB_INLINE void set_state(int64_t state) { state_ = state; }
  OB_INLINE void set_retry_count(int64_t count) { retry_count_ = count; }
  OB_INLINE void set_eligible_ts(int64_t ts) { eligible_ts_ = ts; }
  OB_INLINE void set_definition_version(int64_t ver) { definition_version_ = ver; }

private:
  uint64_t task_id_;
  uint64_t table_id_;
  uint64_t column_id_;
  common::ObString request_id_;
  common::ObString rowkey_predicate_;
  int64_t state_;
  int64_t retry_count_;
  int64_t eligible_ts_;
  int64_t definition_version_;
};

} // namespace schema
} // namespace share
} // namespace oceanbase
