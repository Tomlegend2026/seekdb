/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_property.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

namespace
{
ObObjMeta text_meta(ObCollationType collation)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  return meta;
}

TEST(FTSWordRegression, repeated_tokens_keep_exact_counts)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(37, "FTSWordTest", "FTSWordTest"));
  ObFTParserProperty properties;
  const ObObjMeta meta = text_meta(CS_TYPE_UTF8MB4_BIN);
  ObAddWordFlag flag;
  flag.set_groupby_word();
  ObAddWord add(properties, meta, flag, allocator, words);
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(OB_SUCCESS, add.process_word("apple", 5, 5, 1));
    ASSERT_EQ(OB_SUCCESS, add.process_word("banana", 6, 6, 1));
  }
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(5, "apple", meta), count));
  EXPECT_EQ(1000, count);
  ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(6, "banana", meta), count));
  EXPECT_EQ(1000, count);
  EXPECT_EQ(2, words.size());
  EXPECT_EQ(2000, add.get_add_word_count());
  ASSERT_EQ(OB_SUCCESS, add.process_word("apple", 5, 5, 3));
  ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(5, "apple", meta), count));
  EXPECT_EQ(1003, count);
}

TEST(FTSWordRegression, comparison_collation_still_controls_grouping)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(37, "FTSWordTest", "FTSWordTest"));
  ObFTParserProperty properties;
  const ObObjMeta meta = text_meta(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObAddWordFlag flag;
  flag.set_groupby_word();
  ObAddWord add(properties, meta, flag, allocator, words);
  ASSERT_EQ(OB_SUCCESS, add.process_word("Apple", 5, 5, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("apple", 5, 5, 1));
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(5, "APPLE", meta), count));
  EXPECT_EQ(2, count);
  EXPECT_EQ(1, words.size());
}

TEST(FTSWordRegression, grouping_reports_uninitialized_map)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ObFTParserProperty properties;
  const ObObjMeta meta = text_meta(CS_TYPE_UTF8MB4_BIN);
  ObAddWordFlag flag;
  flag.set_groupby_word();
  ObAddWord add(properties, meta, flag, allocator, words);
  EXPECT_NE(OB_SUCCESS, add.process_word("apple", 5, 5, 1));
  EXPECT_EQ(0, add.get_add_word_count());
}

TEST(FTSWordRegression, stopwords_match_in_same_and_different_collations)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  for (auto collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN, CS_TYPE_LATIN1_SWEDISH_CI}) {
    const ObObjMeta meta = text_meta(collation);
    for (const char *word : {"the", "THE", "from", "FROM"}) {
      bool stopped = false;
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(strlen(word), word, meta), stopped));
      EXPECT_TRUE(stopped);
    }
    for (const char *word : {"database", "oceanbase", "theory"}) {
      bool stopped = true;
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(strlen(word), word, meta), stopped));
      EXPECT_FALSE(stopped);
    }
  }
}
} // namespace
