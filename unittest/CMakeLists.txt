# Focused competition regression targets. Reuse the production shared library
# and bundled GoogleTest instead of replacing any production implementation.
find_library(SEEKDB_REGRESSION_GTEST gtest PATHS "${DEP_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(SEEKDB_REGRESSION_GTEST_MAIN gtest_main PATHS "${DEP_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

add_executable(ai_function_regression sql/test_ai_function_regression.cpp)
target_link_libraries(ai_function_regression PRIVATE
  oceanbase ${SEEKDB_REGRESSION_GTEST_MAIN} ${SEEKDB_REGRESSION_GTEST} Threads::Threads)
add_test(NAME ai_function_regression COMMAND ai_function_regression)

add_executable(fts_word_regression storage/test_fts_word_regression.cpp)
target_link_libraries(fts_word_regression PRIVATE
  oceanbase ${SEEKDB_REGRESSION_GTEST_MAIN} ${SEEKDB_REGRESSION_GTEST} Threads::Threads)
add_test(NAME fts_word_regression COMMAND fts_word_regression)

add_executable(fts_word_benchmark storage/benchmark_fts_word.cpp)
target_link_libraries(fts_word_benchmark PRIVATE oceanbase Threads::Threads)
