#include "share/schema/ob_ai_column_task.h"
#include <gtest/gtest.h>
#include <string>

using namespace oceanbase::common;
using oceanbase::share::schema::ObAiColumnTask;

TEST(AiColumnTask, OwnsStringsAfterSourceAndResultBuffersAreReleased)
{
  ObSEArray<ObAiColumnTask, 1> queued;
  {
    std::string request = "request-unique", predicate = "`id` = 42";
    ObAiColumnTask task;
    task.set_task_id(99);
    task.set_table_id(1001);
    task.set_column_id(20);
    ASSERT_EQ(OB_SUCCESS, task.set_request_id(ObString(request.length(), request.data())));
    ASSERT_EQ(OB_SUCCESS, task.set_rowkey_predicate(ObString(predicate.length(), predicate.data())));
    std::fill(request.begin(), request.end(), 'x');
    std::fill(predicate.begin(), predicate.end(), 'x');
    ASSERT_EQ(OB_SUCCESS, queued.push_back(task));
    task.reset();
  }
  ASSERT_EQ(1, queued.count());
  EXPECT_EQ(ObString("request-unique"), queued.at(0).get_request_id());
  EXPECT_EQ(ObString("`id` = 42"), queued.at(0).get_rowkey_predicate());
  EXPECT_EQ(99, queued.at(0).get_task_id());
  EXPECT_TRUE(queued.at(0).is_valid());
}

TEST(AiColumnTask, AssignmentReplacesOwnedFieldsAndResetClearsState)
{
  ObAiColumnTask target;
  ASSERT_EQ(OB_SUCCESS, target.set_request_id(ObString("old")));
  {
    ObAiColumnTask source;
    source.set_task_id(3);
    ASSERT_EQ(OB_SUCCESS, source.set_request_id(ObString("new")));
    ASSERT_EQ(OB_SUCCESS, target.assign(source));
    ASSERT_EQ(OB_SUCCESS, target.assign(target));
  }
  EXPECT_EQ(ObString("new"), target.get_request_id());
  target.reset();
  EXPECT_TRUE(target.get_request_id().empty());
  EXPECT_TRUE(target.get_rowkey_predicate().empty());
  EXPECT_FALSE(target.is_valid());
}
