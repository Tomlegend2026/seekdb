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
                    result_dimension_,
                    dependency_column_ids_);

int64_t ObAiColumnSchema::get_convert_size() const
{
  int64_t convert_size = sizeof(*this);
  convert_size += column_name_.length() + 1;
  convert_size += ai_model_name_.length() + 1;
  convert_size += normalized_expr_.length() + 1;
  convert_size += sync_mode_.length() + 1;
  convert_size += dependency_column_ids_.count() * sizeof(uint64_t);
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
    if (OB_FAIL(dependency_column_ids_.assign(other.dependency_column_ids_))) {
    } else if (OB_FAIL(set_column_name(other.column_name_))) {
    } else if (OB_FAIL(set_ai_model_name(other.ai_model_name_))) {
    } else if (OB_FAIL(set_normalized_expr(other.normalized_expr_))) {
    } else if (OB_FAIL(set_sync_mode(other.sync_mode_))) {
    }
  }
  return ret;
}

int ObAiColumnSchema::add_dependency_column_id(uint64_t id)
{
  int ret = OB_SUCCESS;
  if (id == OB_INVALID_ID || id == 0 || id == column_id_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    bool found = false;
    for (int64_t i = 0; i < dependency_column_ids_.count(); ++i) {
      found |= dependency_column_ids_.at(i) == id;
    }
    if (!found) { ret = dependency_column_ids_.push_back(id); }
  }
  return ret;
}

int ObAiColumnSchema::remap_column_ids(const common::ObIArray<uint64_t> &from,
                                      const common::ObIArray<uint64_t> &to)
{
  int ret = OB_SUCCESS;
  if (from.count() != to.count()) { return OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; i < from.count(); ++i) {
    if (from.at(i) == 0 || from.at(i) == OB_INVALID_ID || to.at(i) == 0
        || to.at(i) == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
    for (int64_t j = 0; j < i; ++j) {
      if (from.at(j) == from.at(i) || to.at(j) == to.at(i)) { return OB_INVALID_ARGUMENT; }
    }
  }
  const auto lookup = [&](uint64_t input, uint64_t &output) {
    for (int64_t i = 0; i < from.count(); ++i) {
      if (from.at(i) == input) { output = to.at(i); return OB_SUCCESS; }
    }
    return OB_INVALID_ARGUMENT;
  };
  uint64_t ids[4] = {column_id_, state_column_id_, request_column_id_, result_spec_column_id_};
  for (auto &id : ids) {
    if (OB_SUCC(ret)) { ret = lookup(id, id); }
  }
  common::ObSEArray<uint64_t, 4> dependencies;
  for (int64_t i = 0; OB_SUCC(ret) && i < dependency_column_ids_.count(); ++i) {
    uint64_t mapped = 0;
    if (OB_FAIL(lookup(dependency_column_ids_.at(i), mapped))) {
    } else if (OB_FAIL(dependencies.push_back(mapped))) {
    }
  }
  if (OB_SUCC(ret) && OB_SUCC(ret = dependency_column_ids_.assign(dependencies))) {
    column_id_ = ids[0];
    state_column_id_ = ids[1];
    request_column_id_ = ids[2];
    result_spec_column_id_ = ids[3];
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
