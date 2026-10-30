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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_ai_column_helper.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "share/schema/ob_ai_column_dml_handler.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_ai_column_task.h"
#include "lib/string/ob_sql_string.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace sql
{

int ObAiColumnHelper::extract_rowkey_values(const common::ObIArray<ObExpr *> &row_exprs,
                                             int64_t rowkey_cnt,
                                             ObEvalCtx &eval_ctx,
                                             ObIArray<ObString> &rowkey_values)
{
  int ret = OB_SUCCESS;
  rowkey_values.reset();

  if (rowkey_cnt <= 0 || rowkey_cnt > row_exprs.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(row_exprs), K(rowkey_cnt));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cnt; ++i) {
      if (OB_ISNULL(row_exprs.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("row expression is null", K(ret), K(i));
      } else {
        ObDatum *datum = nullptr;
        ObObj obj;
        if (OB_FAIL(row_exprs.at(i)->eval(eval_ctx, datum))) {
          LOG_WARN("failed to evaluate expression", K(ret), K(i));
        } else if (OB_FAIL(datum->to_obj(obj, row_exprs.at(i)->obj_meta_, row_exprs.at(i)->obj_datum_map_))) {
        } else {
          ObString str;
          ObIAllocator &allocator = eval_ctx.get_expr_res_alloc();
          if (OB_FAIL(obj_to_sql_string(obj, str, allocator))) {
            LOG_WARN("failed to convert obj to sql string", K(ret), K(i));
          } else if (OB_FAIL(rowkey_values.push_back(str))) {
            LOG_WARN("failed to push back rowkey value", K(ret), K(i));
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnHelper::obj_to_sql_string(const ObObj &obj, ObString &str, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  str.reset();
  if (obj.is_string_type()) {
    // Binary hex SQL literals are independent of NO_BACKSLASH_ESCAPES and
    // preserve arbitrary primary-key bytes, including quotes and NULs.
    const ObString value = obj.get_string();
    const int64_t size = static_cast<int64_t>(value.length()) * 2 + 4;
    char *buffer = static_cast<char *>(allocator.alloc(size));
    if (nullptr == buffer) { return OB_ALLOCATE_MEMORY_FAILED; }
    const char *digits = "0123456789abcdef";
    buffer[0] = 'X'; buffer[1] = '\'';
    for (int64_t i = 0; i < value.length(); ++i) {
      const unsigned char byte = static_cast<unsigned char>(value.ptr()[i]);
      buffer[2 + 2 * i] = digits[byte >> 4];
      buffer[3 + 2 * i] = digits[byte & 15];
    }
    buffer[2 + 2 * value.length()] = '\'';
    str.assign_ptr(buffer, static_cast<int32_t>(3 + 2 * value.length()));
  } else {
    const int64_t size = 4096;
    char *buffer = static_cast<char *>(allocator.alloc(size));
    int64_t pos = 0;
    if (nullptr == buffer) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(obj.print_sql_literal(buffer, size, pos))) {
    } else { str.assign_ptr(buffer, static_cast<int32_t>(pos)); }
  }
  return ret;
}
bool ObAiColumnHelper::table_has_ai_columns(const schema::ObTableSchema &table_schema)
{
  return table_schema.has_ai_columns();
}

namespace {
int execute_ai_task_sql(ObTableModifyOp &op, const char *sql)
{
  int ret = OB_SUCCESS;
  bool nested = false;
  if (OB_FAIL(op.open_inner_conn())) {
  } else if (OB_FAIL(op.begin_nested_session(false))) {
  } else {
    nested = true;
    ret = op.execute_write(sql);
  }
  if (nested) {
    const int end_ret = op.end_nested_session();
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
}

namespace {
bool ai_definition_changed(const schema::ObTableSchema &table, const schema::ObAiColumnSchema &definition,
                           const ObIArray<uint64_t> *changed)
{
  if (nullptr == changed) { return true; }
  for (int64_t i = 0; i < changed->count(); ++i) {
    const uint64_t id = changed->at(i);
    if ((nullptr != table.get_column_schema(id) && table.get_column_schema(id)->is_rowkey_column())) { return true; }
    const auto &dependencies = definition.get_dependency_column_ids();
    for (int64_t j = 0; j < dependencies.count(); ++j) {
      if (id == dependencies.at(j)) { return true; }
    }
  }
  return false;
}
int invalidate_ai_frame(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef,
                        const schema::ObAiColumnSchema &definition, const ObString &request)
{
  int ret = OB_SUCCESS;
  const uint64_t ids[] = {definition.get_column_id(), definition.get_state_column_id(),
                         definition.get_request_column_id(), definition.get_result_spec_column_id()};
  ObSqlString spec;
  OZ(spec.append_fmt("%ld:%ld:%ld", definition.get_result_type(), definition.get_result_dimension(),
                     definition.get_definition_version()));
  for (int64_t i = 0; OB_SUCC(ret) && i < 4; ++i) {
    int64_t index = -1;
    for (int64_t j = 0; j < ctdef.column_ids_.count(); ++j) {
      if (ctdef.column_ids_.at(j) == ids[i]) { index = j; break; }
    }
    ObDatum *datum = nullptr;
    if (index < 0 || index >= ctdef.new_row_.count() || nullptr == ctdef.new_row_.at(index)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ctdef.new_row_.at(index)->eval(op.get_eval_ctx(), datum))) {
    } else if (i == 0) {
      datum->set_null();
    } else {
      ObString value = i == 1 ? ObString("PENDING") : i == 2 ? request : spec.string();
      ObString owned;
      OZ(ob_write_string(op.get_eval_ctx().get_expr_res_alloc(), value, owned));
      if (OB_SUCC(ret)) { datum->set_string(owned); }
    }
    if (OB_SUCC(ret)) { ctdef.new_row_.at(index)->get_eval_info(op.get_eval_ctx()).evaluated_ = true; }
  }
  return ret;
}
}

int ObAiColumnHelper::create_tasks_for_insert(ObTableModifyOp &dml_op,
                                             const schema::ObTableSchema &table_schema,
                                             const ObIArray<ObString> &rowkey_values,
                                             const ObDMLBaseCtDef &ctdef,
                                             const ObIArray<uint64_t> *changed)
{
  int ret = OB_SUCCESS;
  if (!table_has_ai_columns(table_schema)) { return ret; }
  ObArenaAllocator allocator;
  ObString predicate, predicate_literal;
  ObObj predicate_value;
  OZ(schema::ObAiColumnDMLHandler::build_rowkey_predicate(table_schema, rowkey_values, predicate, allocator));
  predicate_value.set_varchar(predicate);
  OZ(obj_to_sql_string(predicate_value, predicate_literal, allocator));
  char request[sql::UuidCommon::LENGTH_UUID + 1] = {0};
  OZ(ObExprUuid::gen_server_uuid(request, sql::UuidCommon::LENGTH_UUID));
  const auto &definitions = table_schema.get_ai_column_infos();
  for (int64_t i = 0; OB_SUCC(ret) && i < definitions.count(); ++i) {
    const auto &definition = definitions.at(i);
    if (!ai_definition_changed(table_schema, definition, changed)) { continue; }
    ObSqlString remove, sql;
    OZ(remove.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu "
                          "AND column_id=%lu AND rowkey_predicate=%.*s", table_schema.get_table_id(),
                          definition.get_column_id(), predicate_literal.length(), predicate_literal.ptr()));
    OZ(execute_ai_task_sql(dml_op, remove.ptr()));
    OZ(sql.append_fmt("INSERT INTO oceanbase.__all_ai_column_task "
                      "(task_id,table_id,column_id,request_id,rowkey_predicate,state,retry_count,eligible_ts,definition_version) "
                      "VALUES(UUID_SHORT(),%lu,%lu,'%s',%.*s,%d,0,CURRENT_TIMESTAMP(6),%ld)",
                      table_schema.get_table_id(), definition.get_column_id(), request,
                      predicate_literal.length(), predicate_literal.ptr(),
                      static_cast<int>(schema::AI_TASK_PENDING), definition.get_definition_version()));
    OZ(execute_ai_task_sql(dml_op, sql.ptr()));
    OZ(invalidate_ai_frame(dml_op, ctdef, definition, ObString(request)));
  }
  return ret;
}
int ObAiColumnHelper::create_tasks_for_update(ObTableModifyOp &dml_op,
                                             const schema::ObTableSchema &table_schema,
                                             const ObIArray<ObString> &rowkey_values,
                                             const ObIArray<uint64_t> &updated_column_ids,
                                             const ObDMLBaseCtDef &ctdef)
{
  int ret = OB_SUCCESS;
  bool primary_changed = false;
  for (int64_t i = 0; i < updated_column_ids.count(); ++i) {
    primary_changed |= (nullptr != table_schema.get_column_schema(updated_column_ids.at(i)) && table_schema.get_column_schema(updated_column_ids.at(i))->is_rowkey_column());
  }
  if (primary_changed) {
    ObSEArray<ObString, 4> old_key;
    OZ(extract_rowkey_values(ctdef.old_row_, table_schema.get_rowkey_column_num(), dml_op.get_eval_ctx(), old_key));
    OZ(delete_tasks_for_delete(dml_op, table_schema, old_key));
  }
  OZ(create_tasks_for_insert(dml_op, table_schema, rowkey_values, ctdef, &updated_column_ids));
  return ret;
}

int ObAiColumnHelper::delete_tasks_for_delete(ObTableModifyOp &dml_op,
                                             const schema::ObTableSchema &table_schema,
                                             const ObIArray<ObString> &rowkey_values)
{
  int ret = OB_SUCCESS;
  if (!table_has_ai_columns(table_schema)) { return ret; }
  ObArenaAllocator allocator;
  ObString predicate, literal;
  ObObj value;
  ObSqlString sql;
  OZ(schema::ObAiColumnDMLHandler::build_rowkey_predicate(table_schema, rowkey_values, predicate, allocator));
  value.set_varchar(predicate);
  OZ(obj_to_sql_string(value, literal, allocator));
  OZ(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate=%.*s",
                    table_schema.get_table_id(), literal.length(), literal.ptr()));
  OZ(execute_ai_task_sql(dml_op, sql.ptr()));
  return ret;
}
} // namespace sql
} // namespace oceanbase
