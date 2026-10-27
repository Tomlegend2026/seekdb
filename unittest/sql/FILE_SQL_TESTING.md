# FileSQL 扫描基础

当前已完成 CSV / JSONL 流式读取、全量类型推断、安全文件流、glob 文件发现、seekdb 原生 SQL 接入，`DESCRIBE` 原生结果元数据，以及 `COPY` 导出。**整题尚未完成**：Parquet 仍未实现。

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
- 日期和时间按实际值核对；DATETIME 列按 MySQL 的 `DATETIME`（精度 0）语义渲染，不带多余小数秒。
- JSONL 字段并集、缺失字段补 NULL；CSV / JSONL INNER JOIN、LEFT JOIN、双 CSV JOIN 及原生执行计划。
- 无表头、分隔符、NULL 文本；坏 CSV / JSONL、非法采样及选项、路径越界和符号链接拒绝；错误中包含坏行号。
- 同一 SQL 对应文件由整数列改成字符串列后重新推断，未复用旧投影。

扫描回归目前共 41 项，通过 CMake / CTest、GCC 严格警告、AddressSanitizer 和 UndefinedBehaviorSanitizer。尚未提测，不据此宣称平台得分。

## 安全文件流与 JSONL

`file_scan_file.{h,cpp}` 提供 Linux 只读文件流。调用方必须显式启用并提供绝对安全根目录；空配置拒绝打开。逐级 `openat` 配合 `O_NOFOLLOW` 拒绝所有路径组件中的符号链接，按路径组件检查安全根边界，拒绝 `..`、NUL、非普通文件和超限文件。FIFO 以非阻塞方式打开并拒绝，不会挂住查询线程。

读取持有同一个描述符；每个 64 KiB 缓冲读取前后及重扫前核验描述符和当前完整路径的身份、大小、纳秒时间戳，并检查 inode 的 inotify 变更事件。快速同长度改写曾在回归中暴露时间戳粒度问题，新增变更事件检测后该用例通过。调用方仍须在每批结果发布前再次 `validate()`，并把错误返回到 SQL 层；工具不会自动设置数据库系统变量。

`file_scan_json.{h,cpp}` 使用仓库已有 RapidJSON 的 SAX 解析器，不引入外部查询引擎。逐行只保留当前平坦对象，完整扫描字段并集，补齐缺失字段为 NULL；保留数值原始文本，避免大整数先转 double 后丢失精度。JSON 字符串保持字符串语义，仅合法日期和时间参与日期类型推断。拒绝嵌套对象/数组、重复或大小写冲突字段、非法 UTF-8、畸形行和字段上限越界。

新增 10 项文件安全和 10 项 JSONL 回归，连同 CSV 共 37 项已执行通过；相同 37 项另外通过 GCC 严格警告、AddressSanitizer 和 UndefinedBehaviorSanitizer。包含同长度改写、路径替换、目录替换、删除、缓冲边界、晚出现字段、缺失值、Unicode、类型冲突和完整推断失败时不发布部分结构。

## DESCRIBE 原生元数据

2026-10-27：`DESCRIBE` / `DESC` 作用于 FileSQL 查询时返回原生结果元数据，列名为 `column_name`、`column_type`、`null`、`key`、`default`、`extra`，与官方测试 `tools/deploy/mysql_test/test_suite/file_sql` 中 `describe select * from read_csv(...)` 的期望一致。

- 类型名取自文件推断结果（BIGINT、DOUBLE、BOOLEAN、DATE、DATETIME、VARCHAR），不回落为通用类型名；可空性来自推断阶段统计的 NULL 计数，外连接空侧列强制为可空。
- 投影、别名、派生表和 CTE 沿列引用逐层解析，文件类型名与可空性在投影后仍然保留。派生表与 CTE 的列号基址是 `OB_APP_MIN_COLUMN_ID`，文件表则是 1 基，两种基址都会被探测并用列名消歧。递归 CTE 的遍历有深度上限。
- 解析器根节点的 `value_` 仍专用于参数计数，DESCRIBE 标记改用格式子节点的 `T_BOOL`，普通 `EXPLAIN` 行为不变。
- 只含文件表的查询走元数据路径；一旦查询同时读取受保护基表，则保持原有 `EXPLAIN` 输出，由执行器对原查询做权限检查，避免绕过原查询的权限判定。文件访问本身仍受 `enable_file_sql`、`FILE` 权限和 `secure_file_priv` 约束。

验证：扫描回归共 63 项通过；完整 seekdb 构建通过；真实 SQL 联调通过 CSV 类型、JSONL 缺失字段、列别名、外连接可空性、空结果查询、派生表、CTE，以及权限回归（无授权时报错、授权后放行、撤销 `FILE` 权限后报错）。权限回归用临时账号 `'file_desc_xxx'@'%'`，并以不指定默认库的方式连接，确保该账号不因默认库而获得额外权限。

## COPY 导出

2026-10-27：`COPY (查询) TO '文件' (选项)` 与 `COPY 表 TO '文件' (选项)` 已接入原生 SQL，复用 `INTO OUTFILE` 的流式写出链路，因此 FILE 权限、`secure_file_priv` 边界与文件流安全校验全部沿用既有实现，不引入任何外部引擎。

- 语法：`COPY '(' select_stmt ')' TO STRING_VALUE opt_copy_option_list`，以及 `COPY tbl_name TO STRING_VALUE opt_copy_option_list`；表形式等价于 `COPY (SELECT * FROM 表) TO ...`。`COPY` 沿用仓库既有的非保留关键字，不改变关键字表。
- 选项：`format csv`（仅支持 csv）、`header`（裸写、`true`、`false`）、`nullstr` / `null`、`delimiter` / `sep`、`quote`、`escape`、`overwrite` / `or_replace`（默认禁止覆盖）。取值支持布尔字面量、字符串字面量、标识符和整数字面量。
- 按题目要求禁止覆盖已有文件，报 `OB_FILE_ALREADY_EXIST`；`overwrite true`、裸 `overwrite` 和 `or_replace` 均拒绝，不再先删除已有文件。底层文件写入器继续使用 `O_EXCL` 防止检查后出现文件的竞争覆盖。
- `header` 写出的列名取自优化器阶段的投影列（别名优先），`nullstr` 写出的 NULL 文本不加引号，字符串列仍按 CSV 规则加引号与转义。
- 导出失败时删除半成品文件，不留残缺文件；空结果也会落盘表头行。
- COPY 语句不参与 PS 参数化：选项值会被解析进计划，若被参数化则不同选项值的语句会共用同一份缓存计划（`header false` 与 `header true` 会串味）。因此在 `ObSqlParameterization::mark_tree` 中把 `T_COPY_OUTFILE` 标记为不可参数化，使其保留在计划缓存键中。

验证：

```sh
python3 unittest/sql/file_sql_integration.py \
  --socket /home/tom/oceanbase2026-filesql-test-instance/run/sql.sock
python3 unittest/sql/file_sql_official_case.py \
  --socket /home/tom/oceanbase2026-filesql-test-instance/run/sql.sock
```

集成脚本覆盖查询导出、表导出、表头、NULL 文本、禁止覆盖、非法选项与关闭功能开关时的报错；`file_sql_official_case.py` 复现官方 `tools/deploy/mysql_test/test_suite/file_sql` 用例的语句与期望输出。Parquet 仍未实现，默认完整验收会失败；开发期间显式传 `--allow-incomplete` 才允许跳过，并输出 PARTIAL，不宣称整题通过。

2026-10-27 复核 MiniMax 提交 `185724e`：新增的特殊表头测试先在原程序中失败，修复后完整构建、63 项扫描回归及原生 SQL 联调七组均通过。新增边界包括含逗号、引号、中文和换行的表头（Python csv 与原生 read_csv 双重读回）、空结果表头、NULL/空字符串/字面 NULL 的区分、自定义分隔符及转义、拒绝覆盖且原文件不变、运行时 JSON 错误后删除半成品、路径越界及空 secure_file_priv 拒绝。COPY 现在也复用 session_policy，解析阶段检查 enable_file_sql、FILE 权限和非空安全目录。

文件创建成功后立即登记清理状态，避免表头或缓冲初始化失败时漏清理；关闭/刷新报错会触发清理并释放描述符，清理结果不再覆盖原有错误。关闭/刷新故障分支经代码检查，尚未做系统调用故障注入，不把运行时 JSON 错误测试当作该分支的故障注入证据。
