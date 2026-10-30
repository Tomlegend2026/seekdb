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
#include "share/schema/ob_ai_column_dml_handler.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase {
using namespace common;
namespace share {
namespace schema {
int ObAiColumnDMLHandler::build_rowkey_predicate(const ObTableSchema &table_schema,
                                                const ObIArray<ObString> &rowkey_values,
                                                ObString &predicate, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  predicate.reset();
  const auto &keys = table_schema.get_rowkey_info();
  if (keys.get_size() == 0 || rowkey_values.count() != keys.get_size()) { return OB_INVALID_ARGUMENT; }
  ObSqlString sql;
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.get_size(); ++i) {
    const auto *key = keys.get_column(i);
    const auto *column = nullptr == key ? nullptr : table_schema.get_column_schema(key->column_id_);
    if (nullptr == column || rowkey_values.at(i).empty()) { ret = OB_ERR_UNEXPECTED; break; }
    if (i > 0) { OZ(sql.append(" AND ")); }
    OZ(sql.append("`"));
    const ObString name = column->get_column_name_str();
    for (int64_t c = 0; OB_SUCC(ret) && c < name.length(); ++c) {
      if (name.ptr()[c] == '`') { OZ(sql.append("``")); }
      else { OZ(sql.append(name.ptr() + c, 1)); }
    }
    OZ(sql.append("` = "));
    // Values are typed SQL literals emitted by the DML expression serializer.
    OZ(sql.append(rowkey_values.at(i)));
  }
  OZ(ob_write_string(allocator, sql.string(), predicate));
  return ret;
}
} // namespace schema
} // namespace share
} // namespace oceanbase
