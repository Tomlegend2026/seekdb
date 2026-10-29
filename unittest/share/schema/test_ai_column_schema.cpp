#include "share/schema/ob_ai_column_schema.h"
#include "lib/allocator/ob_allocator.h"
#include <gtest/gtest.h>
#include <vector>

using namespace oceanbase::common;
using oceanbase::share::schema::ObAiColumnSchema;

static void populate(ObAiColumnSchema &schema)
{
  schema.set_table_id(1001);
  schema.set_column_id(20);
  schema.set_state_column_id(21);
  schema.set_request_column_id(22);
  schema.set_result_spec_column_id(23);
  ASSERT_EQ(OB_SUCCESS, schema.set_column_name(ObString("tags")));
  ASSERT_EQ(OB_SUCCESS, schema.set_ai_model_name(ObString("model")));
  ASSERT_EQ(OB_SUCCESS, schema.set_normalized_expr(ObString("AI_COMPLETE(model, content)")));
  ASSERT_EQ(OB_SUCCESS, schema.set_sync_mode(ObString("async")));
  ASSERT_EQ(OB_SUCCESS, schema.add_dependency_column_id(17));
}

TEST(AiColumnSchema, DependencyCopyAndSerialization)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema source(&allocator), copy(&allocator), decoded(&allocator);
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
  source.reset();
  for (const auto *schema : {&copy, &decoded}) {
    EXPECT_EQ(ObString("tags"), schema->get_column_name());
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
