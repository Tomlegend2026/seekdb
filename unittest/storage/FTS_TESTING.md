# 全文索引候选优化

当前状态：候选代码和词频回归测试通过语法检查；CMake 测试目标配置成功。尚未运行回归或测量收益，不能声称提速或本题完成。

## 改动

- 重复词频聚合：已存在的词直接更新文档私有词表中的计数，减少一次哈希及比较；首次插入、词表比较规则和错误返回保留原行为。
- 停用词：输入和字典排序规则完全相同时直接查询；其他排序规则继续转换，并在转换缓冲区释放之前完成查询。

测试覆盖重复词精确计数、增加已有词的频率、大小写不敏感排序规则、未初始化词表的错误，以及不同排序规则下的停用词结果。

## 验证入口

```sh
cmake --build build_release --target fts_word_regression fts_word_benchmark -j2
ctest --test-dir build_release -R '^fts_word_regression$' --output-on-failure
./build_release/unittest/fts_word_benchmark
```

微基准调用生产 `ObAddWord::process_word`，处理 400 万个词，并在输出耗时前核对全部计数。它只测重复词聚合热点，不能替代官方全文索引测试或用于宣称比赛得分。

正式性能验证应使用未修改的 `tools/benchmark/fts_large_bench.sh`，在独立测试实例上保存优化前后原始报告。两个版本保持编译方式、机器资源和测试参数一致；运行基准时不并行编译。完整核对建索引、分词和查询三组指标，以及分词和查询结果。官方脚本会重建自己的测试库，应只用于测试实例。
