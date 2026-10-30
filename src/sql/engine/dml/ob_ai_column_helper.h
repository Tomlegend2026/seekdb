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

#include "common/object/ob_object.h"
#include "lib/string/ob_string.h"
#include "sql/engine/expr/ob_expr.h"
#include "sql/engine/ob_exec_context.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace sql
{
class ObTableModifyOp;
struct ObDMLBaseCtDef;

class ObAiColumnHelper
{
public:
  // Extract rowkey values from expression array and convert to string format
  static int extract_rowkey_values(const common::ObIArray<ObExpr *> &row_exprs,
                                    int64_t rowkey_cnt,
                                    ObEvalCtx &eval_ctx,
                                    common::ObIArray<common::ObString> &rowkey_values);

  // Create AI column tasks for INSERT operation
  static int create_tasks_for_insert(ObTableModifyOp &dml_op,
                                      const share::schema::ObTableSchema &table_schema,
                                      const common::ObIArray<common::ObString> &rowkey_values,
                                      const ObDMLBaseCtDef &ctdef,
                                      const common::ObIArray<uint64_t> *changed = nullptr);

  // Create AI column tasks for UPDATE operation
  static int create_tasks_for_update(ObTableModifyOp &dml_op,
                                      const share::schema::ObTableSchema &table_schema,
                                      const common::ObIArray<common::ObString> &rowkey_values,
                                      const common::ObIArray<uint64_t> &updated_column_ids,
                                      const ObDMLBaseCtDef &ctdef);

  // Delete AI column tasks for DELETE operation
  static int delete_tasks_for_delete(ObTableModifyOp &dml_op,
                                      const share::schema::ObTableSchema &table_schema,
                                      const common::ObIArray<common::ObString> &rowkey_values);

  // Check if table has AI columns
  static bool table_has_ai_columns(const share::schema::ObTableSchema &table_schema);

private:
  // Convert ObObj to string representation for SQL
  static int obj_to_sql_string(const common::ObObj &obj,
                                common::ObString &str,
                                common::ObIAllocator &allocator);
};

} // namespace sql
} // namespace oceanbase
