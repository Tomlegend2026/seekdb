#define USING_LOG_PREFIX SHARE_SCHEMA
#include "share/schema/ai_column_catalog.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase { namespace share { namespace schema {
using namespace common;

bool AiColumnCatalog::has_storage(const ObTableSchema &table)
{
  bool found = false;
  for (int64_t i = 0; !found && i < table.get_column_count(); ++i) {
    const auto *column = table.get_column_schema_by_idx(i);
    if (nullptr != column && column->is_hidden()) {
      const ObString name = column->get_column_name_str();
      found = name.length() > 11 && 0 == MEMCMP(name.ptr(), "__ai_state_", 11);
    }
  }
  return found;
}

int AiColumnCatalog::encode(const ObAiColumnSchema &definition, ObIAllocator &allocator,
                            ObString &hex)
{
  int ret = OB_SUCCESS;
  const int64_t size = definition.get_serialize_size();
  char *data = nullptr;
  char *encoded = nullptr;
  int64_t pos = 0;
  hex.reset();
  if (!definition.is_valid() || size <= 0 || size > 8 * 1024 * 1024) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(data = static_cast<char *>(allocator.alloc(size)))
             || OB_ISNULL(encoded = static_cast<char *>(allocator.alloc(size * 2)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(definition.serialize(data, size, pos))) {
  } else {
    const char *digits = "0123456789abcdef";
    for (int64_t i = 0; i < pos; ++i) {
      const auto value = static_cast<unsigned char>(data[i]);
      encoded[2 * i] = digits[value >> 4];
      encoded[2 * i + 1] = digits[value & 15];
    }
    hex.assign_ptr(encoded, static_cast<int32_t>(2 * pos));
  }
  return ret;
}

int AiColumnCatalog::decode(const ObString &hex, ObIAllocator &allocator,
                            ObAiColumnSchema &definition)
{
  int ret = OB_SUCCESS;
  char *data = nullptr;
  const int64_t size = hex.length() / 2;
  auto nibble = [](char c) -> int {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  if (hex.empty() || hex.length() % 2 != 0 || size > 8 * 1024 * 1024) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(data = static_cast<char *>(allocator.alloc(size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
      const int high = nibble(hex.ptr()[2 * i]);
      const int low = nibble(hex.ptr()[2 * i + 1]);
      if (high < 0 || low < 0) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        data[i] = static_cast<char>((high << 4) | low);
      }
    }
    int64_t pos = 0;
    ObAiColumnSchema decoded(&allocator);
    if (OB_SUCC(ret) && OB_FAIL(decoded.deserialize(data, size, pos))) {
    } else if (OB_SUCC(ret) && (pos != size || !decoded.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_SUCC(ret) && OB_FAIL(definition.assign(decoded))) {
    }
  }
  return ret;
}

int AiColumnCatalog::insert(ObISQLClient &client, const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  OZ(write_definitions(client, table, false, false, table.get_schema_version()));
  OZ(write_definitions(client, table, true, false, table.get_schema_version()));
  return ret;
}

int AiColumnCatalog::write_definitions(ObISQLClient &client, const ObTableSchema &table,
                                      bool history, bool deleted, int64_t schema_version)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  if (!table.get_ai_column_infos().empty()
      && (table.get_table_id() == 0 || table.get_table_id() == OB_INVALID_ID
          || schema_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_ai_column_infos().count(); ++i) {
    allocator.reuse();
    const auto &definition = table.get_ai_column_infos().at(i);
    ObAiColumnSchema stored(&allocator);
    ObString hex;
    ObDMLSqlSplicer dml;
    ObSqlString sql;
    int64_t affected = 0;
    if (OB_FAIL(stored.assign(definition))) {
    } else {
      // CREATE TABLE may allocate its real table ID after resolution.
      stored.set_table_id(table.get_table_id());
      OZ(encode(stored, allocator, hex));
      OZ(dml.add_pk_column("table_id", stored.get_table_id()));
      OZ(dml.add_pk_column("column_id", stored.get_column_id()));
      OZ(dml.add_column("column_name", ObHexEscapeSqlStr(stored.get_column_name())));
      OZ(dml.add_column("ai_model_name", ObHexEscapeSqlStr(stored.get_ai_model_name())));
      OZ(dml.add_column("ai_func_type", stored.get_ai_func_type()));
      OZ(dml.add_column("normalized_expr", ObHexEscapeSqlStr(stored.get_normalized_expr())));
      OZ(dml.add_column("max_retries", stored.get_max_retries()));
      OZ(dml.add_column("sync_mode", ObHexEscapeSqlStr(stored.get_sync_mode())));
      OZ(dml.add_column("definition_version", stored.get_definition_version()));
      OZ(dml.add_column("state_column_id", stored.get_state_column_id()));
      OZ(dml.add_column("request_column_id", stored.get_request_column_id()));
      OZ(dml.add_column("result_spec_column_id", stored.get_result_spec_column_id()));
      OZ(dml.add_column("result_type", stored.get_result_type()));
      OZ(dml.add_column("result_dimension", stored.get_result_dimension()));
      OZ(dml.add_column("definition_data", ObHexEscapeSqlStr(hex)));
      if (history) {
        OZ(dml.add_pk_column("schema_version", schema_version));
        OZ(dml.add_column("is_deleted", deleted ? 1 : 0));
      }
      OZ(dml.splice_insert_sql(history ? "__all_ai_column_history" : "__all_ai_column", sql));
      OZ(client.write(sql.ptr(), affected));
      if (OB_SUCC(ret) && affected != 1) { ret = OB_ERR_UNEXPECTED; }
    }
  }
  return ret;
}

int AiColumnCatalog::remove(ObISQLClient &client, const ObTableSchema &table, int64_t schema_version)
{
  int ret = OB_SUCCESS;
  if (has_storage(table) || !table.get_ai_column_infos().empty()) {
    OZ(write_definitions(client, table, true, true, schema_version));
    for (const char *name : {"__all_ai_column_task", "__all_ai_column"}) {
      ObSqlString sql;
      int64_t affected = 0;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id=%lu", name,
                                       table.get_table_id()))) {
      } else if (OB_FAIL(client.write(sql.ptr(), affected))) {
      }
    }
  }
  return ret;
}

int AiColumnCatalog::load(ObISQLClient &client, ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  if (has_storage(table)) {
    ObSqlString sql;
    ObISQLClient::ReadResult result;
    ObArenaAllocator allocator;
    ObSEArray<ObAiColumnSchema, 4> definitions;
    OZ(sql.append_fmt(
        "SELECT a.* FROM __all_ai_column_history a JOIN "
        "(SELECT column_id, MAX(schema_version) AS version FROM __all_ai_column_history "
        "WHERE table_id=%lu AND schema_version<=%ld GROUP BY column_id) v "
        "ON a.column_id=v.column_id AND a.schema_version=v.version "
        "WHERE a.table_id=%lu AND a.is_deleted=0 ORDER BY a.column_id",
        table.get_table_id(), table.get_schema_version(), table.get_table_id()));
    OZ(client.read(result, sql.ptr()));
    if (OB_SUCC(ret) && OB_ISNULL(result.get_result())) { ret = OB_ERR_UNEXPECTED; }
    while (OB_SUCC(ret)) {
      const int next_ret = result.get_result()->next();
      if (next_ret == OB_ITER_END) { break; }
      if (next_ret != OB_SUCCESS) { ret = next_ret; break; }
      ObString hex;
      int64_t column_id = 0;
      ObAiColumnSchema definition(&allocator);
      OZ(result.get_result()->get_varchar("definition_data", hex));
      OZ(result.get_result()->get_int("column_id", column_id));
      OZ(decode(hex, allocator, definition));
      if (OB_SUCC(ret) && (definition.get_table_id() != table.get_table_id()
                          || definition.get_column_id() != static_cast<uint64_t>(column_id))) {
        ret = OB_ERR_UNEXPECTED;
      }
      if (OB_SUCC(ret)) {
        const auto *result_column = table.get_column_schema(definition.get_column_id());
        if (nullptr == result_column || result_column->is_hidden()
            || result_column->get_column_name_str() != definition.get_column_name()
            || result_column->get_data_type() != definition.get_result_type()) {
          ret = OB_ERR_UNEXPECTED;
        }
        for (const uint64_t id : {definition.get_state_column_id(),
                                 definition.get_request_column_id(),
                                 definition.get_result_spec_column_id()}) {
          const auto *hidden = table.get_column_schema(id);
          if (nullptr == hidden || !hidden->is_hidden()) { ret = OB_ERR_UNEXPECTED; }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < definition.get_dependency_column_ids().count(); ++i) {
          const auto *dependency = table.get_column_schema(definition.get_dependency_column_ids().at(i));
          if (nullptr == dependency || dependency->is_hidden()) { ret = OB_ERR_UNEXPECTED; }
        }
      }
      OZ(definitions.push_back(definition));
    }
    if (OB_SUCC(ret) && definitions.empty()) { ret = OB_ERR_UNEXPECTED; }
    OZ(table.set_ai_column_infos(definitions));
  }
  return ret;
}
} } }
