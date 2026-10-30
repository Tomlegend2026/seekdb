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
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"
namespace oceanbase {
namespace common { class ObIAllocator; }
namespace share {
namespace schema {
class ObTableSchema;
// A shared, allocator-owned representation of the complete typed primary key.
class ObAiColumnDMLHandler {
public:
  static int build_rowkey_predicate(const ObTableSchema &table_schema,
                                    const common::ObIArray<common::ObString> &rowkey_values,
                                    common::ObString &predicate, common::ObIAllocator &allocator);
};
} // namespace schema
} // namespace share
} // namespace oceanbase
