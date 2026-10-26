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

#include <chrono>
#include <cstdio>
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_property.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

int main()
{
  constexpr int64_t repetitions = 500000;
  const ObString tokens[] = {ObString("database"), ObString("index"), ObString("query"),
      ObString("token"), ObString("document"), ObString("storage"), ObString("search"), ObString("engine")};
  const int64_t token_count = sizeof(tokens) / sizeof(tokens[0]);
  ObArenaAllocator allocator;
  ObFTWordMap words;
  int ret = words.create(37, "FTSWordBench", "FTSWordBench");
  if (OB_SUCCESS != ret) {
    return 1;
  }
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObAddWordFlag flag;
  flag.set_groupby_word();
  ObFTParserProperty properties;
  ObAddWord add(properties, meta, flag, allocator, words);
  const auto begin = std::chrono::steady_clock::now();
  for (int64_t i = 0; OB_SUCCESS == ret && i < repetitions; ++i) {
    for (const auto &token : tokens) {
      ret = add.process_word(token.ptr(), token.length(), token.length(), 1);
      if (OB_SUCCESS != ret) {
        break;
      }
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - begin).count();
  for (const auto &token : tokens) {
    int64_t count = 0;
    if (OB_SUCCESS != words.get_refactored(ObFTWord(token.length(), token.ptr(), meta), count)
        || count != repetitions) {
      return 2;
    }
  }
  if (OB_SUCCESS != ret || words.size() != token_count
      || add.get_add_word_count() != repetitions * token_count) {
    return 3;
  }
  printf("{\"benchmark\":\"fts_word_grouping\",\"tokens\":%ld,\"elapsed_ns\":%ld,"
         "\"ns_per_token\":%.3f,\"correctness\":true}\n",
         repetitions * token_count, static_cast<int64_t>(elapsed),
         static_cast<double>(elapsed) / (repetitions * token_count));
  return 0;
}
