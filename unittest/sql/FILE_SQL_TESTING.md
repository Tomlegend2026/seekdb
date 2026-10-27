# FileSQL 扫描基础

当前已完成 CSV / JSONL 流式读取、全量类型推断、安全文件流、glob 文件发现及 seekdb 原生 SQL 接入，并通过真实 SQL 联调。**整题尚未完成**：Parquet、COPY 和 DESCRIBE 返回列元数据仍未实现。

2026-10-27：glob 已随提交 `1eb0b60` 上传。全部扫描回归 61 项通过；其中 19 项 glob 测试另经 GCC 严格警告、AddressSanitizer / UndefinedBehaviorSanitizer 执行通过。原生 SQL 联调通过文件列、排序、过滤、相对与绝对模式、空匹配及路径边界检查。glob 只发现文件并返回单列 `file`，不自动合并文件内容。目录项在追加前受结果数量 / 遍历数量限制，拒绝符号链接，错误时不发布部分结果。

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

原生 SQL 接入先检查默认关闭的功能开关和 `secure_file_priv`，通过原生算子执行过滤、聚合、排序和 JOIN。

## 原生 SQL 接入与实测

2026-10-26：完整 seekdb 构建通过。新增 `enable_file_sql`，默认 OFF；启用后要求 FILE 权限和非空的绝对 `secure_file_priv`。读取函数以原生 JSON_TABLE 逻辑算子承载动态列，并在执行器中流式输出类型化的行；过滤、分组、排序和 JOIN 继续由 seekdb 执行。未调用任何外部 SQL 引擎。

支持 `read_csv`、`read_json` / `read_ndjson`、别名，以及 CSV 的 header / delim / delimiter / quote / escape / nullstr 选项。JSONL 的 format 支持 newline_delimited；sample_size 仅接受 -1。所有读取参数须为静态标量表达式，拒绝依赖数据行和子查询。单个列名最多 256 字节。

解析阶段完整推断列信息，执行阶段重新完整核验文件内容指纹和结构，再重扫输出；全程只保留列信息与当前记录。文件计划禁止加入计划缓存。执行前、每行输出前后及重扫前校验文件，异常在 SQL 客户端返回记录号、物理行号及原因。

```sh
python3 unittest/sql/file_sql_integration.py \
  --socket /home/tom/oceanbase2026-filesql-test-instance/run/sql.sock
```

该脚本会短暂限制独立实例的 secure_file_priv，结束时恢复。修改这个全局变量要求本机 Unix socket，不能用 TCP 绕过。实例必须与脚本共享本机文件系统。原有测试实例包含旧系统变量元数据，新程序在该实例启动时返回 OB_ERR_SYS_VARIABLE_UNKNOWN；此次联调使用新建的独立实例 `/home/tom/oceanbase2026-filesql-test-instance`、端口 2882，不改动报名 Docker 实例。

实际通过：

- CSV 类型、NULL、多行引号、重复表头、别名、WHERE、ORDER BY、LIMIT、COUNT/SUM、GROUP BY、HAVING。
- 日期和时间按实际值核对；当前 DATETIME 默认显示 6 位小数，未将字符串格式差别误判为日期值错误。
- JSONL 字段并集、缺失字段补 NULL；CSV / JSONL INNER JOIN、LEFT JOIN、双 CSV JOIN 及原生执行计划。
- 无表头、分隔符、NULL 文本；坏 CSV / JSONL、非法采样及选项、路径越界和符号链接拒绝；错误中包含坏行号。
- 同一 SQL 对应文件由整数列改成字符串列后重新推断，未复用旧投影。

扫描回归目前共 41 项，通过 CMake / CTest、GCC 严格警告、AddressSanitizer 和 UndefinedBehaviorSanitizer。尚未提测，不据此宣称平台得分。

## 安全文件流与 JSONL

`file_scan_file.{h,cpp}` 提供 Linux 只读文件流。调用方必须显式启用并提供绝对安全根目录；空配置拒绝打开。逐级 `openat` 配合 `O_NOFOLLOW` 拒绝所有路径组件中的符号链接，按路径组件检查安全根边界，拒绝 `..`、NUL、非普通文件和超限文件。FIFO 以非阻塞方式打开并拒绝，不会挂住查询线程。

读取持有同一个描述符；每个 64 KiB 缓冲读取前后及重扫前核验描述符和当前完整路径的身份、大小、纳秒时间戳，并检查 inode 的 inotify 变更事件。快速同长度改写曾在回归中暴露时间戳粒度问题，新增变更事件检测后该用例通过。调用方仍须在每批结果发布前再次 `validate()`，并把错误返回到 SQL 层；工具不会自动设置数据库系统变量。

`file_scan_json.{h,cpp}` 使用仓库已有 RapidJSON 的 SAX 解析器，不引入外部查询引擎。逐行只保留当前平坦对象，完整扫描字段并集，补齐缺失字段为 NULL；保留数值原始文本，避免大整数先转 double 后丢失精度。JSON 字符串保持字符串语义，仅合法日期和时间参与日期类型推断。拒绝嵌套对象/数组、重复或大小写冲突字段、非法 UTF-8、畸形行和字段上限越界。

新增 10 项文件安全和 10 项 JSONL 回归，连同 CSV 共 37 项已执行通过；相同 37 项另外通过 GCC 严格警告、AddressSanitizer 和 UndefinedBehaviorSanitizer。包含同长度改写、路径替换、目录替换、删除、缓冲边界、晚出现字段、缺失值、Unicode、类型冲突和完整推断失败时不发布部分结构。
