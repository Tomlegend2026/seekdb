# AI 列阶段验证

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
