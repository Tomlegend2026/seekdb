# AI 列阶段验证

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
