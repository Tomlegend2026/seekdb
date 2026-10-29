#pragma once

#include "share/schema/ob_ai_column_schema.h"

namespace oceanbase {
namespace common { class ObISQLClient; }
namespace share { namespace schema {
class ObTableSchema;

// Callers supply the existing DDL SQL transaction. No independent commit.
class AiColumnCatalog final
{
public:
  static bool has_storage(const ObTableSchema &table);
  static int insert(common::ObISQLClient &client, const ObTableSchema &table);
  static int remove(common::ObISQLClient &client, const ObTableSchema &table,
                    int64_t schema_version);
  static int load(common::ObISQLClient &client, ObTableSchema &table);
  static int encode(const ObAiColumnSchema &definition, common::ObIAllocator &allocator,
                    common::ObString &hex);
  static int decode(const common::ObString &hex, common::ObIAllocator &allocator,
                    ObAiColumnSchema &definition);
private:
  static int write_definitions(common::ObISQLClient &client, const ObTableSchema &table,
                               bool history, bool deleted, int64_t schema_version);
};
} } }
