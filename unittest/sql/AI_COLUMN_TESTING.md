# AI 列阶段验证

2026-10-30 官方 DDL 验收推进（本轮实测）：

- **修复 `ERROR 4019 Size overflow`**：`ObSysFunRawExpr::exprs_` 是 `ObFixedArray`，容量在原始表达式解析时按参数个数固定为 2，`push_back` 在 `count_ >= capacity_` 时直接返回 `OB_SIZE_OVERFLOW`，且 `ObFixedArrayImpl::reserve` 在已初始化时同样拒绝扩容（`src/oblib/lib/container/ob_fixed_array.h:169`）。此前把类型提示作为第 4 个函数参数追加进去必然溢出。
- **类型提示改走第三个（config）参数**：`ai_complete` 的函数注册只接受 2~3 个 SQL 参数，第 4 个参数报 `ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT`(1582)；`ObMySQLProxy` 建立的也不是 inner session，因此第 4 参数方案在 SQL 层不可用。改为把提示作为 config 对象的保留键 `type_hint`，由 `ObExprAIComplete::eval_ai_complete` 取出并提升为 provider 请求的 system prompt（`call_completion` 的第 4 个参数是 C++ 参数），同时从 config 中 `remove` 掉，避免污染 provider 请求体。官方 mock 断言的是 `event["system"]`，通道一致。
- **修复维度校验被吞**：`parse_ai_column_options` 的返回值覆盖了此前设置的 `OB_ERR_INVALID_VECTOR_DIM`，导致维度不符不报错。现改为逐步 fail-fast。
- **官方 `ai_column_ddl.test` 逐条实测**：statement 12~19 通过（建表、元数据 2 行、max_retries、3 个隐藏列共 6 个、AI 列可见、json 类型提示已持久化、alter_docs 建表）；维度不符返回 `ERROR 7600 (22000) inconsistent dimension: expected 4 got 3`，AI→AI 依赖返回 `ERROR 1235 (0A000)`，均与官方期望逐字一致；失败用例不留下表。
- **未通过**：`ALTER TABLE ... ADD COLUMN ... AI COLUMN` 仍报 `ERROR 1054 Unknown column 'content' in 'generated column function'`——ALTER 路径用的是 `AlterTableSchema`，解析依赖表达式时看不到原表已有列，且隐藏列与 AI 元数据随 ALTER 持久化的链路尚未接通。
- **普通 SQL 回归**：有主键/无主键建表、INSERT/SELECT/UPDATE/DELETE、无主键表 DML、事务回滚全部正常，AI 接线未破坏普通路径。
- **修正两个会影响评分的仓库问题**：自写的 `ai_column_basic.test` / `ai_column_comprehensive.test` 原本放在官方 fixture 目录 `tools/deploy/mysql_test/test_suite/ai_column/t/` 下但没有对应 `.result`，评测方按官方 `.result` 比对时会直接失败，已移到 `unittest/sql/ai_column/`；`sql_source_ownership_check.py` 的 `SQL_UNITY_GROUPS`/`SQL_STANDALONE_SOURCES` 期望值与本分支实际源文件数对齐，`sql_header_inventory.bzl` 补登记 `engine/basic/file_scan_parquet.h`。

以上只覆盖官方 DDL 一组用例。DML/事务、worker 生命周期、重试与严格读取、向量索引四组仍未通过，`ObAiColumnWorker::init` 目前没有任何调用者，worker 尚未接入 ObServer 启停，不得据此宣称 AI 列已实现。

2026-10-30 recheck, executed against the current working-tree draft:

- Production Bison 2.4.1 invocation (`-v -Werror -d`) failed at `src/sql/parser/sql_parser_mysql_mode.y:14889`: undefined `RETRY`, undefined `opt_where_clause`, and undeclared type for `$5`. Generated output was directed outside the repository.
- Production Clang 17 flags were used for focused syntax checks. All three new translation units failed: Worker (`lib/thread/ob_thread.h` missing), handler and helper (`common/ob_string.h` missing). The DDL resolver additionally fails on the newly added `ObDataType` argument to `set_data_type`, which expects `ObObjType` (`ob_ddl_resolver.cpp:2190`).
- A standalone table-schema check also reported incomplete column types, but this file is built through unity includes; that result is NOT treated as a demonstrated draft regression.
- Full build/link and official SQL acceptance tests are not recorded as passed for this draft. Parser generation already blocks the normal build; standalone checks cannot establish link success.

Actionable review findings (ordered by impact):

1. **Blocker — dangling result storage** (`src/share/schema/ob_ai_column_worker.cpp:448`): `result` borrows `response_str` from the arena declared at line 378. Returning destroys that arena before `process_task` writes the result. Copy the response into caller-owned storage; test delayed writeback under ASan. Numeric rowkey conversion in `ob_ai_column_helper.cpp:100` and the predicate return in `ob_ai_column_dml_handler.cpp:155` also escape local buffers.
2. **Blocker — unsafe writeback** (`src/share/schema/ob_ai_column_worker.cpp:524`): model output is interpolated into a SQL literal without escaping, and the WHERE clause checks only the row key. An apostrophe breaks the statement; a slow response can replace a newer result. Use typed/escaped values plus request ID and definition version checks, and atomically publish the row/change group. Test quoted responses and reversed completion order.
3. **Major — the submitted grammar cannot generate** (`src/sql/parser/sql_parser_mysql_mode.y:14889`): register the RETRY token/lexer mapping and use a defined, typed WHERE production. Then regenerate with the production Bison command before claiming parser support. Correct the missing header paths and new DDL type mismatch before linking.
4. **Major — worker has no execution lifecycle** (`src/share/schema/ob_ai_column_worker.cpp:39`): the proxy is initialized to null, no assignment/startup call was found elsewhere, and the new sources are absent from production inventories. Wire ownership, proxy initialization, start, stop and build membership; test actual post-commit processing and shutdown.
5. **Major — model inputs and outputs are not implemented** (`src/share/schema/ob_ai_column_worker.cpp:403`): requests contain the SQL definition text instead of its evaluated row-specific argument; line 443 leaves response decoding TODO and returns the whole response object. Evaluate the declared expression, decode completion/embedding payloads, and validate the destination type/dimension. Run the official worker/vector tests with Mock request assertions.

Detailed local compiler diagnostics: `/home/tom/ai-column-draft-audit.log` (WSL). This review preserves the draft and does not assert any new contest score.

2026-10-30 draft audit: the new uncommitted worker/DML changes are NOT build-verified or acceptance-tested. Do not use the generated completion reports as evidence of completion. The last verified and pushed baseline remains `a4b8b4858`.

- The existing WSL Clang 17 toolchain is available. Focused syntax checks using its production compilation flags fail for all three new translation units: worker includes nonexistent `lib/thread/ob_thread.h`; task/helper headers include nonexistent `common/ob_string.h`. Re-downloading devtools cannot fix these source errors.
- Worker/DML helper sources are absent from their production source inventories; no worker startup call was found outside its own implementation. A successful build that excludes these files would not validate the feature.
- `call_ai_model` sends `normalized_expr` as prompt/input rather than evaluating it against the row, and response decoding remains TODO. It returns response text allocated in a local arena, leaving a dangling result after return.
- Result writeback predicates contain only the row predicate, without request-ID/definition-version guards. A slow old response can overwrite a newer update. Each column is published separately rather than as one row/change group transaction.
- DML task writes use the SQL proxy directly; sharing the business transaction has not been established or tested. Rowkey helpers also return strings backed by local stack/SQL buffers. Strict reads, index consistency, and rollback semantics must not be reported as complete.

No new official test pass or competition score is established by this audit. Preserve the draft for repair; do not publish it as a completed submission.

2026-10-29 目录持久化接线：新增 `AiColumnCatalog`，由现有建表 SQL client 写当前目录和历史目录；删表在同一 client 写历史删除标记、清理任务与当前定义。目录的序列化内容使用十六进制文本，保留依赖 ID，SQL 字符串使用 `ObHexEscapeSqlStr`。写目录时使用建表最终分配的 table ID；历史记录使用表 Schema 版本，加载时取不超过目标 Schema 版本的最新未删除定义，检查结果列与隐藏列是否存在。

完整表结构读取流程已接入目录加载。未来 DDL 创建的状态隐藏列须使用 `__ai_state_` 前缀；普通表没有这种隐藏列时不访问 AI 目录，避免在 bootstrap 中递归查询目录。新增 `__all_ai_column_history`（1144）及目录 `definition_data` 列。既有测试实例不会自动补齐新增系统表/列，本轮数据库验证须使用独立新实例，不改旧实例数据。

新增目录原生测试覆盖损坏/截断数据、原定义保护、SQL 转义、当前目录和历史目录之间失败、影响行数异常、清理失败以及历史读取失败。可选数据库联调脚本 `ai_column_catalog_integration.py` 使用原生测试输出的生产 SQL，验证历史版本、删除标记和事务回滚；所有写入均回滚，拒绝已有 fixture ID。它验证目录 SQL，不等同于用户 `CREATE TABLE ... AI COLUMN` 的完整执行。

本轮仍保留 `T_CONSTR_AI_COLUMN` 的执行保护：表达式绑定、隐藏列创建、ALTER、DML、worker、严格读取和向量索引尚未实现，官方五组验收仍未通过。

实测：完整 seekdb 构建通过，原生回归 7/7 通过，Share 头文件归属门禁通过。端口 2884 的独立新实例 bootstrap 成功，目录 SQL/历史/删除标记/回滚联调通过；6 种 CREATE 与 1 种 ALTER 到达原执行保护，3 种畸形语法拒绝，普通 SQL 正常，失败后无表/列残留。联调使用现有 `mysql` 客户端，无额外 Python 依赖。

复现目录联调：

```sh
python3 unittest/sql/ai_column_catalog_integration.py \
  --binary build_release/unittest/ai_column_schema_regression \
  --socket /home/tom/ai-column-catalog-instance-20261029/run/sql.sock
```

以下为前序阶段记录，其“尚未持久化”等描述只对应当时的提交。

2026-10-29 后续：将 AI 定义集合接入 `ObTableSchema` 的复制、reset、空间估算及序列化/反序列化，使定义可随 DDL 请求和表 Schema 传递。修复 `ObAiColumnSchema` 反序列化字符串借用输入缓冲区的问题，改为深拷贝。集合替换拒绝重复结果列 ID 和 AI 到 AI 的依赖，非法输入不清除原有定义。

实测：完整 seekdb 构建通过，Schema 回归 4/4 通过；测试在独立内存池间复制并释放源内存、清空反序列化输入缓冲后核对全部字符串和依赖。新增表 Schema 整体传递及非法替换回归。端口 2883 专用实例启动成功，原生语法联调通过 6 种 CREATE、1 种 ALTER、3 种错误写法及失败无残留检查。Share 头文件归属门禁通过。

本阶段只完成 DDL 定义传递基础，未接通系统表写入、隐藏列创建或后台计算。AI COLUMN 建表仍由显式保护返回 1235，不能把本阶段当作官方 DDL 验收通过。

2026-10-29 接续：Schema 新增依赖列 ID 集合，纳入 assign、reset、序列化和空间估算；列 ID 重映射同时覆盖结果、三个隐藏列及依赖列，缺失映射和重复映射会拒绝。新增 `ai_column_schema_regression`，实际执行 2/2 通过；FTS 原有正确性回归 4/4 通过。完整 seekdb 构建通过。测试使用生产 Schema 源码，与生产共享库链接，不用替身。

本次同时修正 Share 头文件归属清单，镜像目录中门禁通过。以上只验证元数据对象，不代表已持久化到系统表或已接通 DDL、DML、后台 worker。`T_CONSTR_AI_COLUMN` 的执行保护仍在，官方 AI 列五组验收仍未通过。

2026-10-28：分支 `feat/ai-column`，catalog 定义基于 `d8053de`。

本轮完成列属性语法接入与明确的未实现保护，不代表 AI 列执行功能完成。

此前合法 `AI COLUMN` 返回 1149 的原因是 DDL resolver 的列属性 switch 缺少 `T_CONSTR_AI_COLUMN` 分支，落入默认的 `OB_ERR_PARSER_SYNTAX`。不是词法 AI token 或 bison 冲突。保留已有语法改动，并为新节点加入 fail-closed 分支：返回 1235 与 `AI COLUMN execution (syntax recognized)`，不允许静默创建普通列、丢失 AI 定义。

完整 seekdb 构建通过；在专用实例 `/home/tom/ai-column-test-instance`、端口 2883 上运行：

```sh
python3 unittest/sql/ai_column_syntax_integration.py \
  --socket /home/tom/ai-column-test-instance/run/sql.sock
```

结果：6 种 CREATE 语法、1 种 ALTER ADD 语法到达 DDL 保护分支；3 种畸形语法拒绝；普通建表仍成功；失败的 AI DDL 没有留下表或列。测试使用独立随机数据库，结束删除自身测试库。

未完成：AI 表达式与选项的语义校验、DDL 元数据和隐藏列持久化、SHOW/RETRY 语句、DML 任务生成、worker、严格读取及向量索引联动。官方五组 AI 列验收尚未通过，不应据本轮测试宣称 AI 列已实现。
