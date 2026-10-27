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

add_executable(file_scan_utils_regression
  sql/test_file_scan_utils.cpp
  sql/test_file_scan_file.cpp
  sql/test_file_scan_json.cpp
  sql/test_file_scan_glob.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_file.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_cursor.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_glob.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/file_scan_json.cpp)
target_include_directories(file_scan_utils_regression PRIVATE
  ${PROJECT_SOURCE_DIR}/src ${DEP_DIR}/include)
target_link_libraries(file_scan_utils_regression PRIVATE
  ${SEEKDB_REGRESSION_GTEST_MAIN} ${SEEKDB_REGRESSION_GTEST} Threads::Threads)
add_test(NAME file_scan_utils_regression COMMAND file_scan_utils_regression)

add_executable(fts_word_benchmark storage/benchmark_fts_word.cpp)
target_link_libraries(fts_word_benchmark PRIVATE oceanbase Threads::Threads)
