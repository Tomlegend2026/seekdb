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

#include "share/schema/ob_ai_column_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

OB_SERIALIZE_MEMBER(ObAiColumnSchema,
                    table_id_,
                    column_id_,
                    column_name_,
                    ai_model_name_,
                    ai_func_type_,
                    normalized_expr_,
                    max_retries_,
                    sync_mode_,
                    definition_version_,
                    state_column_id_,
                    request_column_id_,
                    result_spec_column_id_,
                    result_type_,
                    result_dimension_);

int64_t ObAiColumnSchema::get_convert_size() const
{
  int64_t convert_size = sizeof(*this);
  convert_size += column_name_.length() + 1;
  convert_size += ai_model_name_.length() + 1;
  convert_size += normalized_expr_.length() + 1;
  convert_size += sync_mode_.length() + 1;
  return convert_size;
}

int ObAiColumnSchema::assign(const ObAiColumnSchema &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    reset();
    table_id_ = other.table_id_;
    column_id_ = other.column_id_;
    ai_func_type_ = other.ai_func_type_;
    max_retries_ = other.max_retries_;
    definition_version_ = other.definition_version_;
    state_column_id_ = other.state_column_id_;
    request_column_id_ = other.request_column_id_;
    result_spec_column_id_ = other.result_spec_column_id_;
    result_type_ = other.result_type_;
    result_dimension_ = other.result_dimension_;
    if (OB_FAIL(set_column_name(other.column_name_))) {
    } else if (OB_FAIL(set_ai_model_name(other.ai_model_name_))) {
    } else if (OB_FAIL(set_normalized_expr(other.normalized_expr_))) {
    } else if (OB_FAIL(set_sync_mode(other.sync_mode_))) {
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
