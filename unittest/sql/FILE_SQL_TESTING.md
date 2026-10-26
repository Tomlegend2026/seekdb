# FileSQL 扫描基础

当前完成 CSV / JSONL 流式读取、全量类型推断和安全文件流工具，**尚未接入 seekdb SQL 解析、元数据和执行入口，不算本题完成**。Parquet、glob、COPY 和原生 SQL 接入仍未完成。

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

## 安全文件流与 JSONL

`file_scan_file.{h,cpp}` 提供 Linux 只读文件流。调用方必须显式启用并提供绝对安全根目录；空配置拒绝打开。逐级 `openat` 配合 `O_NOFOLLOW` 拒绝所有路径组件中的符号链接，按路径组件检查安全根边界，拒绝 `..`、NUL、非普通文件和超限文件。FIFO 以非阻塞方式打开并拒绝，不会挂住查询线程。

读取持有同一个描述符；每个 64 KiB 缓冲读取前后及重扫前核验描述符和当前完整路径的身份、大小、纳秒时间戳，并检查 inode 的 inotify 变更事件。快速同长度改写曾在回归中暴露时间戳粒度问题，新增变更事件检测后该用例通过。调用方仍须在每批结果发布前再次 `validate()`，并把错误返回到 SQL 层；工具不会自动设置数据库系统变量。

`file_scan_json.{h,cpp}` 使用仓库已有 RapidJSON 的 SAX 解析器，不引入外部查询引擎。逐行只保留当前平坦对象，完整扫描字段并集，补齐缺失字段为 NULL；保留数值原始文本，避免大整数先转 double 后丢失精度。JSON 字符串保持字符串语义，仅合法日期和时间参与日期类型推断。拒绝嵌套对象/数组、重复或大小写冲突字段、非法 UTF-8、畸形行和字段上限越界。

新增 10 项文件安全和 10 项 JSONL 回归，连同 CSV 共 37 项已执行通过；相同 37 项另外通过 GCC 严格警告、AddressSanitizer 和 UndefinedBehaviorSanitizer。包含同长度改写、路径替换、目录替换、删除、缓冲边界、晚出现字段、缺失值、Unicode、类型冲突和完整推断失败时不发布部分结构。
