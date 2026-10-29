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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_

#include "share/schema/ob_schema_struct.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// Definition of one AI COLUMN of a user table.  The row is persisted in
// __all_ai_column and keyed by (table_id, column_id).
class ObAiColumnSchema final : public ObSchema
{
  OB_UNIS_VERSION(1);
public:
  ObAiColumnSchema() { reset(); }
  ~ObAiColumnSchema() = default;
  explicit ObAiColumnSchema(common::ObIAllocator *allocator) : ObSchema(allocator) { reset(); }

  bool is_valid() const override
  {
    return ObSchema::is_valid()
           && table_id_ != OB_INVALID_ID
           && column_id_ != OB_INVALID_ID
           && !column_name_.empty()
           && !ai_model_name_.empty();
  }

  void reset()
  {
    table_id_ = OB_INVALID_ID;
    column_id_ = OB_INVALID_ID;
    column_name_.reset();
    ai_model_name_.reset();
    ai_func_type_ = 0;
    normalized_expr_.reset();
    max_retries_ = 0;
    sync_mode_.reset();
    definition_version_ = 0;
    state_column_id_ = OB_INVALID_ID;
    request_column_id_ = OB_INVALID_ID;
    result_spec_column_id_ = OB_INVALID_ID;
    result_type_ = 0;
    result_dimension_ = 0;
    dependency_column_ids_.reset();
  }

  int assign(const ObAiColumnSchema &other);
  int64_t get_convert_size() const override;
  int add_dependency_column_id(uint64_t id);
  int remap_column_ids(const common::ObIArray<uint64_t> &from,
                       const common::ObIArray<uint64_t> &to);
  const common::ObIArray<uint64_t> &get_dependency_column_ids() const
  { return dependency_column_ids_; }

  OB_INLINE uint64_t get_table_id() const { return table_id_; }
  OB_INLINE uint64_t get_column_id() const { return column_id_; }
  OB_INLINE ObString get_column_name() const { return column_name_; }
  OB_INLINE ObString get_ai_model_name() const { return ai_model_name_; }
  OB_INLINE int64_t get_ai_func_type() const { return ai_func_type_; }
  OB_INLINE ObString get_normalized_expr() const { return normalized_expr_; }
  OB_INLINE int64_t get_max_retries() const { return max_retries_; }
  OB_INLINE ObString get_sync_mode() const { return sync_mode_; }
  OB_INLINE int64_t get_definition_version() const { return definition_version_; }
  OB_INLINE uint64_t get_state_column_id() const { return state_column_id_; }
  OB_INLINE uint64_t get_request_column_id() const { return request_column_id_; }
  OB_INLINE uint64_t get_result_spec_column_id() const { return result_spec_column_id_; }
  OB_INLINE int64_t get_result_type() const { return result_type_; }
  OB_INLINE int64_t get_result_dimension() const { return result_dimension_; }

  OB_INLINE void set_table_id(const uint64_t table_id) { table_id_ = table_id; }
  OB_INLINE void set_column_id(const uint64_t column_id) { column_id_ = column_id; }
  OB_INLINE int set_column_name(const ObString &name) { return deep_copy_str(name, column_name_); }
  OB_INLINE int set_ai_model_name(const ObString &name) { return deep_copy_str(name, ai_model_name_); }
  OB_INLINE void set_ai_func_type(const int64_t type) { ai_func_type_ = type; }
  OB_INLINE int set_normalized_expr(const ObString &expr) { return deep_copy_str(expr, normalized_expr_); }
  OB_INLINE void set_max_retries(const int64_t max_retries) { max_retries_ = max_retries; }
  OB_INLINE int set_sync_mode(const ObString &mode) { return deep_copy_str(mode, sync_mode_); }
  OB_INLINE void set_definition_version(const int64_t version) { definition_version_ = version; }
  OB_INLINE void set_state_column_id(const uint64_t id) { state_column_id_ = id; }
  OB_INLINE void set_request_column_id(const uint64_t id) { request_column_id_ = id; }
  OB_INLINE void set_result_spec_column_id(const uint64_t id) { result_spec_column_id_ = id; }
  OB_INLINE void set_result_type(const int64_t type) { result_type_ = type; }
  OB_INLINE void set_result_dimension(const int64_t dimension) { result_dimension_ = dimension; }

  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(ai_model_name),
               K_(ai_func_type), K_(max_retries), K_(sync_mode), K_(definition_version),
               K_(state_column_id), K_(request_column_id), K_(result_spec_column_id),
               K_(result_type), K_(result_dimension));

private:
  uint64_t table_id_;
  uint64_t column_id_;
  ObString column_name_;
  ObString ai_model_name_;
  int64_t ai_func_type_;
  ObString normalized_expr_;
  int64_t max_retries_;
  ObString sync_mode_;
  int64_t definition_version_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  int64_t result_type_;
  int64_t result_dimension_;
  common::ObSEArray<uint64_t, 4> dependency_column_ids_;
};

} // namespace schema
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_
