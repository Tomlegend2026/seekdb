#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ai_column_catalog.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "lib/allocator/ob_allocator.h"
#include <gtest/gtest.h>
#include <vector>
#include <cstdlib>
#include <iostream>

using namespace oceanbase::common;
using oceanbase::share::schema::ObAiColumnSchema;
using oceanbase::share::schema::ObTableSchema;
using oceanbase::share::schema::ObColumnSchemaV2;
using oceanbase::share::schema::AiColumnCatalog;

class CatalogWriteProbe final : public ObISQLClient
{
public:
  int escape(const char *, int64_t, char *, int64_t, int64_t &) override
  { return OB_NOT_SUPPORTED; }
  int read(ReadResult &, const char *sql, int32_t) override
  { reads.emplace_back(sql); return error == OB_SUCCESS ? OB_NOT_SUPPORTED : error; }
  int write(const char *sql, int32_t, int64_t &affected) override
  {
    statements.emplace_back(sql);
    affected = affected_rows;
    return fail_at == 0 || statements.size() == fail_at ? error : OB_SUCCESS;
  }
  oceanbase::common::sqlclient::ObISQLConnection *get_connection() override { return nullptr; }
  std::vector<std::string> statements;
  std::vector<std::string> reads;
  int64_t affected_rows = 1;
  int error = OB_SUCCESS;
  size_t fail_at = 0;
};

static void populate(ObAiColumnSchema &schema)
{
  schema.set_table_id(1001);
  schema.set_column_id(20);
  schema.set_state_column_id(21);
  schema.set_request_column_id(22);
  schema.set_result_spec_column_id(23);
  schema.set_max_retries(3);
  schema.set_definition_version(7);
  schema.set_result_dimension(4);
  ASSERT_EQ(OB_SUCCESS, schema.set_column_name(ObString("tags")));
  ASSERT_EQ(OB_SUCCESS, schema.set_ai_model_name(ObString("model")));
  ASSERT_EQ(OB_SUCCESS, schema.set_normalized_expr(ObString("AI_COMPLETE(model, content)")));
  ASSERT_EQ(OB_SUCCESS, schema.set_sync_mode(ObString("async")));
  ASSERT_EQ(OB_SUCCESS, schema.add_dependency_column_id(17));
}

TEST(AiColumnCatalog, EncodingRejectsCorruptionWithoutReplacingDefinition)
{
  ObArenaAllocator allocator, destination_allocator;
  ObAiColumnSchema source(&allocator), decoded(&destination_allocator);
  populate(source);
  ObString hex;
  ASSERT_EQ(OB_SUCCESS, AiColumnCatalog::encode(source, allocator, hex));
  ASSERT_EQ(OB_SUCCESS, AiColumnCatalog::decode(hex, allocator, decoded));
  EXPECT_EQ(17, decoded.get_dependency_column_ids().at(0));
  EXPECT_EQ(OB_INVALID_ARGUMENT, AiColumnCatalog::decode(ObString("abc"), allocator, decoded));
  EXPECT_EQ(OB_INVALID_ARGUMENT, AiColumnCatalog::decode(ObString("zz"), allocator, decoded));
  std::string trailing(hex.ptr(), hex.length());
  trailing += "00";
  EXPECT_EQ(OB_INVALID_ARGUMENT, AiColumnCatalog::decode(
      ObString(trailing.length(), trailing.data()), allocator, decoded));
  std::string truncated(hex.ptr(), hex.length() - 2);
  EXPECT_NE(OB_SUCCESS, AiColumnCatalog::decode(
      ObString(truncated.length(), truncated.data()), allocator, decoded));
  allocator.reuse();
  EXPECT_EQ(ObString("tags"), decoded.get_column_name());
  EXPECT_EQ(ObString("AI_COMPLETE(model, content)"), decoded.get_normalized_expr());
}

TEST(AiColumnCatalog, WritesUseCallerClientAndPropagateFailure)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema source(&allocator);
  populate(source);
  ASSERT_EQ(OB_SUCCESS, source.set_normalized_expr(
      ObString("AI_COMPLETE('model', CONCAT(content, '\n''; DROP TABLE victim;--'))")));
  ObSEArray<ObAiColumnSchema, 1> definitions;
  ASSERT_EQ(OB_SUCCESS, definitions.push_back(source));
  ObTableSchema table(&allocator);
  table.set_table_id(2001);
  table.set_schema_version(7);
  ASSERT_EQ(OB_SUCCESS, table.set_ai_column_infos(definitions));
  CatalogWriteProbe client;
  ASSERT_EQ(OB_SUCCESS, AiColumnCatalog::insert(client, table));
  ASSERT_EQ(2, client.statements.size());
  if (std::getenv("AI_CATALOG_SQL_FIXTURE") != nullptr) {
    for (const auto &sql : client.statements) { std::cout << "AI_CATALOG_INSERT_SQL " << sql << '\n'; }
  }
  EXPECT_NE(std::string::npos, client.statements.front().find("definition_data"));
  EXPECT_NE(std::string::npos, client.statements.front().find("2001"));
  EXPECT_EQ(std::string::npos, client.statements.front().find("DROP TABLE victim"));
  client.statements.clear();
  client.error = OB_TIMEOUT;
  client.fail_at = 2;
  EXPECT_EQ(OB_TIMEOUT, AiColumnCatalog::insert(client, table));
  EXPECT_EQ(2, client.statements.size());
  client.fail_at = 0;
  client.error = OB_TIMEOUT;
  EXPECT_EQ(OB_TIMEOUT, AiColumnCatalog::insert(client, table));
  client.error = OB_SUCCESS;
  client.affected_rows = 0;
  EXPECT_EQ(OB_ERR_UNEXPECTED, AiColumnCatalog::insert(client, table));
  client.statements.clear();
  client.affected_rows = 1;
  client.error = OB_TIMEOUT;
  EXPECT_EQ(OB_TIMEOUT, AiColumnCatalog::remove(client, table, 9));
  ASSERT_EQ(1, client.statements.size());
  EXPECT_NE(std::string::npos, client.statements.front().find("__all_ai_column_history"));
  client.statements.clear();
  client.error = OB_SUCCESS;
  EXPECT_EQ(OB_SUCCESS, AiColumnCatalog::remove(client, table, 9));
  EXPECT_EQ(3, client.statements.size());
  if (std::getenv("AI_CATALOG_SQL_FIXTURE") != nullptr) {
    for (const auto &sql : client.statements) { std::cout << "AI_CATALOG_DELETE_SQL " << sql << '\n'; }
  }
  ObTableSchema ordinary(&allocator);
  client.statements.clear();
  EXPECT_EQ(OB_SUCCESS, AiColumnCatalog::insert(client, ordinary));
  EXPECT_EQ(OB_SUCCESS, AiColumnCatalog::remove(client, ordinary, 9));
  EXPECT_EQ(OB_SUCCESS, AiColumnCatalog::load(client, ordinary));
  EXPECT_TRUE(client.statements.empty());
  EXPECT_TRUE(client.reads.empty());
}

TEST(AiColumnCatalog, FailedHistoricalReadKeepsSchema)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema source(&allocator);
  populate(source);
  ObSEArray<ObAiColumnSchema, 1> definitions;
  ASSERT_EQ(OB_SUCCESS, definitions.push_back(source));
  ObTableSchema table(&allocator);
  table.set_table_id(2001);
  table.set_schema_version(7);
  ASSERT_EQ(OB_SUCCESS, table.set_ai_column_infos(definitions));
  ObColumnSchemaV2 hidden(&allocator);
  hidden.set_table_id(2001);
  hidden.set_column_id(21);
  hidden.set_data_type(ObIntType);
  hidden.set_is_hidden(true);
  ASSERT_EQ(OB_SUCCESS, hidden.set_column_name(ObString("__ai_state_20")));
  ASSERT_EQ(OB_SUCCESS, table.add_column(hidden));
  ASSERT_TRUE(AiColumnCatalog::has_storage(table));
  CatalogWriteProbe client;
  client.error = OB_TIMEOUT;
  EXPECT_EQ(OB_TIMEOUT, AiColumnCatalog::load(client, table));
  ASSERT_EQ(1, client.reads.size());
  EXPECT_NE(std::string::npos, client.reads.front().find("schema_version<=7"));
  EXPECT_NE(std::string::npos, client.reads.front().find("is_deleted=0"));
  if (std::getenv("AI_CATALOG_SQL_FIXTURE") != nullptr) {
    std::cout << "AI_CATALOG_READ_SQL " << client.reads.front() << '\n';
  }
  ASSERT_EQ(1, table.get_ai_column_infos().count());
  EXPECT_EQ(ObString("tags"), table.get_ai_column_infos().at(0).get_column_name());
}

TEST(AiColumnSchema, DependencyCopyAndSerialization)
{
  ObArenaAllocator allocator, copy_allocator, decoded_allocator;
  ObAiColumnSchema source(&allocator), copy(&copy_allocator), decoded(&decoded_allocator);
  populate(source);
  ASSERT_EQ(OB_SUCCESS, source.add_dependency_column_id(17));
  ASSERT_EQ(1, source.get_dependency_column_ids().count());
  EXPECT_EQ(OB_INVALID_ARGUMENT, source.add_dependency_column_id(20));
  EXPECT_EQ(OB_INVALID_ARGUMENT, source.add_dependency_column_id(OB_INVALID_ID));
  ASSERT_EQ(OB_SUCCESS, copy.assign(source));
  std::vector<char> data(source.get_serialize_size());
  int64_t position = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize(data.data(), data.size(), position));
  const int64_t length = position;
  position = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(data.data(), length, position));
  std::fill(data.begin(), data.end(), '\0');
  source.reset();
  allocator.reuse();
  for (const auto *schema : {&copy, &decoded}) {
    EXPECT_EQ(ObString("tags"), schema->get_column_name());
    EXPECT_EQ(ObString("model"), schema->get_ai_model_name());
    EXPECT_EQ(ObString("AI_COMPLETE(model, content)"), schema->get_normalized_expr());
    EXPECT_EQ(ObString("async"), schema->get_sync_mode());
    EXPECT_EQ(3, schema->get_max_retries());
    EXPECT_EQ(7, schema->get_definition_version());
    EXPECT_EQ(4, schema->get_result_dimension());
    ASSERT_EQ(1, schema->get_dependency_column_ids().count());
    EXPECT_EQ(17, schema->get_dependency_column_ids().at(0));
  }
}

TEST(AiColumnSchema, RemapIncludesHiddenAndDependencyIds)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema schema(&allocator);
  populate(schema);
  ObSEArray<uint64_t, 8> from, to;
  for (auto id : {17, 20, 21, 22, 23}) {
    ASSERT_EQ(OB_SUCCESS, from.push_back(id));
    ASSERT_EQ(OB_SUCCESS, to.push_back(id + 100));
  }
  ASSERT_EQ(OB_SUCCESS, schema.remap_column_ids(from, to));
  EXPECT_EQ(120, schema.get_column_id());
  EXPECT_EQ(121, schema.get_state_column_id());
  EXPECT_EQ(122, schema.get_request_column_id());
  EXPECT_EQ(123, schema.get_result_spec_column_id());
  EXPECT_EQ(117, schema.get_dependency_column_ids().at(0));
  EXPECT_EQ(OB_INVALID_ARGUMENT, schema.remap_column_ids(from, to));
  EXPECT_EQ(120, schema.get_column_id());
  EXPECT_EQ(117, schema.get_dependency_column_ids().at(0));
}

TEST(AiColumnSchema, DefinitionTravelsWithTableSchema)
{
  ObArenaAllocator allocator, copy_allocator, decoded_allocator;
  ObAiColumnSchema definition(&allocator);
  populate(definition);
  ObSArray<ObAiColumnSchema> definitions;
  ASSERT_EQ(OB_SUCCESS, definitions.prepare_allocate(1));
  ASSERT_EQ(OB_SUCCESS, definitions.at(0).assign(definition));
  ObTableSchema table(&allocator), copy(&copy_allocator), decoded(&decoded_allocator);
  table.set_table_id(1001);
  ASSERT_EQ(OB_SUCCESS, table.set_ai_column_infos(definitions));
  ASSERT_EQ(OB_SUCCESS, copy.assign(table));
  std::vector<char> data(table.get_serialize_size());
  int64_t position = 0;
  ASSERT_EQ(OB_SUCCESS, table.serialize(data.data(), data.size(), position));
  const int64_t length = position;
  position = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(data.data(), length, position));
  std::fill(data.begin(), data.end(), '\0');
  table.reset();
  definitions.reset();
  allocator.reuse();
  for (const auto *result : {&copy, &decoded}) {
    ASSERT_EQ(1, result->get_ai_column_infos().count());
    const auto &column = result->get_ai_column_infos().at(0);
    EXPECT_EQ(ObString("tags"), column.get_column_name());
    ASSERT_EQ(1, column.get_dependency_column_ids().count());
    EXPECT_EQ(17, column.get_dependency_column_ids().at(0));
    EXPECT_EQ(21, column.get_state_column_id());
    EXPECT_EQ(22, column.get_request_column_id());
    EXPECT_EQ(23, column.get_result_spec_column_id());
    EXPECT_EQ(ObString("model"), column.get_ai_model_name());
    EXPECT_EQ(7, column.get_definition_version());
  }
}

TEST(AiColumnSchema, InvalidReplacementKeepsExistingDefinition)
{
  ObArenaAllocator allocator;
  ObTableSchema table(&allocator);
  ObSArray<ObAiColumnSchema> definitions;
  ASSERT_EQ(OB_SUCCESS, definitions.prepare_allocate(1));
  populate(definitions.at(0));
  ASSERT_EQ(OB_SUCCESS, table.set_ai_column_infos(definitions));
  ASSERT_EQ(OB_SUCCESS, definitions.prepare_allocate(2));
  populate(definitions.at(1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, table.set_ai_column_infos(definitions));
  ASSERT_EQ(1, table.get_ai_column_infos().count());
  definitions.at(1).set_column_id(30);
  ASSERT_EQ(OB_SUCCESS, definitions.at(1).add_dependency_column_id(20));
  EXPECT_EQ(OB_INVALID_ARGUMENT, table.set_ai_column_infos(definitions));
  EXPECT_EQ(20, table.get_ai_column_infos().at(0).get_column_id());
}
