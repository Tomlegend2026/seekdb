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
#include <string>
#include <vector>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace
{
class AiLocalRegression : public ::testing::Test
{
protected:
  ObJsonObject *parse(const char *json)
  {
    ObJsonObject *object = nullptr;
    ObString text = ObString::make_string(json);
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, text, object));
    return object;
  }

  ObArenaAllocator allocator_;
};

class RecordingAllocator : public ObWrapperAllocator
{
public:
  explicit RecordingAllocator(ObIAllocator &allocator) : ObWrapperAllocator(allocator) {}
  void free(void *pointer) override
  {
    freed.push_back(pointer);
    ObWrapperAllocator::free(pointer);
  }
  std::vector<void *> freed;
};

TEST_F(AiLocalRegression, serialized_json_retains_its_allocation)
{
  ObJsonObject *object = parse(R"({"text":"persistent result"})");
  ASSERT_NE(nullptr, object);
  RecordingAllocator tracked(allocator_);
  ObString output;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(tracked, object, output));
  ASSERT_NE(nullptr, output.ptr());
  for (void *freed : tracked.freed) {
    EXPECT_NE(freed, output.ptr()) << "serialization returned an already freed buffer";
  }
  EXPECT_NE(std::string::npos,
      std::string(output.ptr(), output.length()).find("persistent result"));
  tracked.free(output.ptr());
}

TEST_F(AiLocalRegression, repeated_placeholder_uses_full_output_size)
{
  const std::string argument(4096, 'x');
  ObJsonArray args(&allocator_);
  ObJsonString value(ObString(argument.size(), argument.data()));
  ASSERT_EQ(OB_SUCCESS, args.append(&value));
  ObString text("{0}{0}{0}");
  ObJsonObject *prompt = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, text, &args, prompt));
  ObString output;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  EXPECT_EQ(argument + argument + argument, std::string(output.ptr(), output.length()));
}

TEST_F(AiLocalRegression, nested_prompt_is_rendered)
{
  ObJsonObject *prompt = parse(R"({"template":"<{0}>","args":[{"template":"hello {0}","args":["world"]}]})");
  ASSERT_NE(nullptr, prompt);
  ObString output;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  EXPECT_EQ("<hello world>", std::string(output.ptr(), output.length()));
}

TEST_F(AiLocalRegression, json_objects_and_arrays_are_serialized)
{
  ObJsonObject *prompt = parse(R"({"template":"{0}|{1}","args":[{"name":"alice"},[2,true]]})");
  ASSERT_NE(nullptr, prompt);
  EXPECT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));
  ObString output;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  const std::string actual(output.ptr(), output.length());
  EXPECT_NE(std::string::npos, actual.find("alice"));
  EXPECT_NE(std::string::npos, actual.find("true"));
  EXPECT_NE(std::string::npos, actual.find("|["));
}

TEST_F(AiLocalRegression, invalid_placeholder_indices_do_not_overflow)
{
  for (const char *text : {"{1}", "{9223372036854775808}", "{18446744073709551616}"}) {
    ObJsonArray args(&allocator_);
    ObJsonString value(ObString::make_string("x"));
    ASSERT_EQ(OB_SUCCESS, args.append(&value));
    ObString format = ObString::make_string(text);
    ObJsonObject *prompt = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, format, &args, prompt));
    ObString output;
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  }
}

TEST_F(AiLocalRegression, malformed_prompt_is_rejected_before_casting)
{
  for (const char *json : {R"({"template":12,"args":[]})", R"({"template":"{0}","args":{}})"}) {
    ObJsonObject *prompt = parse(json);
    ASSERT_NE(nullptr, prompt);
    ObString output;
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  }
}

TEST_F(AiLocalRegression, openai_embedding_restores_input_order)
{
  ObJsonObject *response = parse(R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})");
  ASSERT_NE(nullptr, response);
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, response, result));
  ASSERT_NE(nullptr, result);
  auto *rows = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, rows->element_count());
  auto *first = static_cast<ObJsonArray *>(rows->get_value(0));
  EXPECT_EQ(1, first->get_value(0)->get_int());
}

TEST_F(AiLocalRegression, malformed_embeddings_are_rejected)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *json : {
      R"({"data":{}})",
      R"({"data":[]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
      R"({"data":[{"index":1,"embedding":[1]}]})",
      R"({"data":[{"embedding":[1]}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[2,3]}]})",
      R"({"data":[{"index":0,"embedding":["not a number"]}]})"}) {
    ObJsonObject *response = parse(json);
    ASSERT_NE(nullptr, response);
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, response, result));
  }
}

TEST_F(AiLocalRegression, completion_rejects_empty_or_non_string_content)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  for (const char *json : {
      R"({"choices":[{"message":{"content":""}}]})",
      R"({"choices":[{"message":{"content":null}}]})",
      R"({"choices":[{"message":{"content":{}}}]})"}) {
    ObJsonObject *response = parse(json);
    ASSERT_NE(nullptr, response);
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, response, result));
  }
}

TEST_F(AiLocalRegression, dashscope_embedding_rejects_malformed_shapes)
{
  ObDashscopeUtils::ObDashscopeEmbed provider;
  for (const char *json : {
      R"({"output":[]})",
      R"({"output":{"embeddings":{}}})",
      R"({"output":{"embeddings":[]}})",
      R"({"output":{"embeddings":[[]]}})",
      R"({"output":{"embeddings":[{"embedding":["bad"]}]}})",
      R"({"output":{"embeddings":[{"embedding":[1]},{"embedding":[1,2]}]}})"}) {
    ObJsonObject *response = parse(json);
    ASSERT_NE(nullptr, response);
    ObIJsonBase *result = nullptr;
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator_, response, result));
  }
}

TEST_F(AiLocalRegression, dashscope_completion_rejects_malformed_shapes)
{
  ObDashscopeUtils::ObDashscopeComplete provider;
  for (const char *json : {
      R"({"output":[]})", R"({"output":{"choices":{}}})",
      R"({"output":{"choices":[]}})", R"({"output":{"choices":[12]}})",
      R"({"output":{"choices":[{"message":12}]}})",
      R"({"output":{"choices":[{"message":{"content":{}}}]}})",
      R"({"output":{"choices":[{"message":{"content":""}}]}})"}) {
    ObJsonObject *response = parse(json);
    ASSERT_NE(nullptr, response);
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, response, result));
  }
}

TEST_F(AiLocalRegression, ollama_embedding_rejects_invalid_values)
{
  ObOllamaUtils::ObOllamaEmbed provider;
  for (const char *json : {
      R"({"embeddings":{}})", R"({"embeddings":[]})",
      R"({"embeddings":[[]]})", R"({"embeddings":[[null]]})",
      R"({"embeddings":[[true]]})", R"({"embeddings":[["1"]]})",
      R"({"embeddings":[[1],[1,2]]})"}) {
    ObJsonObject *response = parse(json);
    ASSERT_NE(nullptr, response);
    ObIJsonBase *result = nullptr;
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator_, response, result));
  }
}

TEST_F(AiLocalRegression, prompt_nesting_has_a_depth_limit)
{
  std::string json = R"({"template":"{0}","args":["leaf"]})";
  for (int i = 0; i < 33; ++i) {
    json = std::string(R"({"template":"{0}","args":[)") + json + "]}";
  }
  ObJsonObject *prompt = parse(json.c_str());
  ASSERT_NE(nullptr, prompt);
  ObString output;
  EXPECT_EQ(OB_INVALID_ARGUMENT,
      ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
}

TEST_F(AiLocalRegression, ollama_is_non_streaming_and_case_insensitive)
{
  ObAIFuncIComplete *provider = nullptr;
  ObString name("oLlAmA");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, name, provider));
  ASSERT_NE(nullptr, provider);
  ObString model("model");
  ObString system;
  ObString content("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator_, model, system, content, nullptr, body));
  ASSERT_NE(nullptr, body);
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ObArray<ObString> headers;
  ObString no_key;
  EXPECT_EQ(OB_SUCCESS, provider->get_header(allocator_, no_key, headers));
}
} // namespace
