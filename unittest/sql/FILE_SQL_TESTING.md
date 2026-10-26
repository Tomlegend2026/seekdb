# FileSQL 扫描基础

当前只完成 CSV 流式读取和全量类型推断工具，**尚未接入 seekdb SQL 解析、元数据和执行入口，不算本题完成**。也尚未实现 JSONL、Parquet、路径校验、文件变化检测及 COPY。

实现位于 `src/sql/engine/basic/file_scan_utils.{h,cpp}`。读取器只保留一条记录；全量推断保留列结构和当前记录，支持重开文件后进入第二遍执行扫描。文件必须由后续安全层校验并打开；该工具不自行打开路径。

验证：

```sh
cmake --build build_release --target file_scan_utils_regression -j2
ctest --test-dir build_release -R file_scan_utils_regression --output-on-failure
```

2026-10-26，17 项 C++ 测试通过，独立 AddressSanitizer / UndefinedBehaviorSanitizer 构建的相同 17 项测试也通过：

- BOM、UTF-8、四字节字符、非法编码、LF/CRLF、多行引号、双引号及可配置转义。
- NULL 与引号中的空字符串、末尾空列、坏行定位、NUL、读取失败、记录和列数上限。
- 不可重定位的 10 万条生成记录流；未把输入文件整体物化。
- BIGINT 边界、浮点非有限值、日期闰年和时间边界、空列、类型宽化顺序无关。
- 全量推断包含末尾记录、无表头首行、字段数不一致时拒绝发布部分结构、重复表头名称避让。

后续 SQL 接入必须先检查默认关闭的功能开关和 `secure_file_priv`；每次批量取数核验文件身份及变化，并通过原生算子执行过滤、聚合、排序和 JOIN。
