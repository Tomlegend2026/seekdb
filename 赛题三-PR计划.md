# 赛题三 PR 执行计划（seekdb · 2026-10-27）

## 已完成：官方 PR #1905，基础实践满分

最终 PR：https://github.com/oceanbase/seekdb/pull/1905 ，修复 #1480。公开 Fork 分支 `fix/1480-uuid-random-node` 已发布，正文包含本队独立验证行。2026-10-27 14:41 平台第 8 次提测执行成功，累计 **30.000 分，成功 3 / 失败 0**；无需等待 PR 合并。

已完成：原程序在受限网卡枚举下启动失败的复现；完整 seekdb 构建；正常网卡、枚举被拒、空接口列表、熵读取 EINTR/短读、熵源拒绝访问、熵提前结束六类真实进程测试。四个成功场景各验证 1,000 个不重复的标准 UUID，随机节点场景验证 multicast 位。

#1477 在本次工作期间已由上游合并，未重复发布该补丁。报名仓库保持私有；来自公开 Fork 的 #1905 已实测通过平台核验。以下为历史计划，已不属于当前待办。

## 目标与判定

- 向 `oceanbase/seekdb` 提交一个修复 PR，目标为官方仓库；**PR 正文中把本队验证行原样、独立一行写入**（不是评论）。
- 2026-10-27 实时页面存在冲突：基础实践总说明写明“PR 不要求来自报名仓库”；PR 子题详情却写“来源必须是本队报名登记的 seekdb 仓库”。不能将其中一页的表述当作无争议的最终规则，提交前需核实实际核验要求。报名答题仓库保持私有。
- 核验只看“已提交”，不要求合并。
- 验证行：`OB-P:gd4uONi4eOGOgpU4rAg55Q`

规则证据：总说明 https://open.oceanbase.com/train/detail/21?questionId=600071 ；PR 子题 https://open.oceanbase.com/train/TopicDetails?questionId=600071&subQesitonId=800507 。此次仅准备缺陷修复，不因规则差异修改仓库可见性或报名信息。

## #1511 复现要点（已读 issue 原文）

2026-10-27 新候选：Harness 已在未包含本轮修复的本地实例上复现 [#1477](https://github.com/oceanbase/seekdb/issues/1477)：`IFNULL((SELECT FALSE), !(!(1 LIKE 'x')))` 用于 WHERE / HAVING 时出现 4016。候选修改位于 `src/sql/rewrite/ob_transform_simplify_expr.cpp`，在两处移除偶数 NOT 时调用现有类型保持辅助函数；尚未完整构建或验证修复效果，不能据此创建已验证的 PR。

验证更新：Codex 已使用构建后的 seekdb 和自带 `deps/3rd/u01/obclient/bin/mysqltest`，实际运行新回归用例、通过 `--record` 重新生成结果，再以比较模式复验通过；修正了草稿中遗漏的结果头 / 行和关闭日志语句。AI 函数 SQL 联调四组通过。官方 issue 已有多个未合并候选 PR，发布前还需核对现有修复并说明关联。

- 现象：同一批 200,107 行，`NOT (category5 IN (1,2,3,4))` 比等价的四条 `NOT (category5 = n)` 合取慢 **290.698×**（ANN 端到端 p50 571.75ms vs 1.97ms）。
- 表结构自带 `category5 INT NOT NULL` + 普通索引 + `VECTOR(128)` 的 HNSW 向量索引（`distance=l2, type=hnsw, lib=vsag, m=16, ef_construction=128, sync_mode=i`）。
- 说明问题落在「否定条件 / NOT-IN 的过滤下推与向量索引过滤链路」，而非纯标量查询；修复需在过滤改写或向量索引过滤路径上做，属于内核级改动，工作量远大于 README/仓库配置类任务。

## 候选 Issue（label=2026-competition，open，共 19 个）

优先与队伍已有改动方向（`src/sql/engine/basic/file_scan_*`、过滤条件等价改写）接近的：

| Issue | 标题要点 | 备注 |
| --- | --- | --- |
| #1511 | 等价 NOT-IN 与否定等值过滤导致 290.698× 搜索延迟差 | 收益最大，复现脚本大概率自带 |
| #1501 | 等价 BETWEEN 改写导致 7.37× 延迟上升 | 有 5 条讨论，信息较全 |
| #1489 | OR-FALSE 恒等过滤导致 71.38× 延迟差 | 讨论较多 |
| #1440 | Range-to-OR 等价过滤导致 73.578× 延迟差 | |
| #1538 / #1535 / #1534 | 算术运算 / AND 吸收 / 冗余条件下的等价过滤延迟差 | 同族问题，可共用修复思路 |

## 执行步骤

1. 读 Issue 描述里的复现 SQL 与预期行为（GitHub 网页或 API）。
2. 在本地 `初赛/seekdb`（origin 指向私有报名仓库，upstream 为官方）基于 `upstream/master` 建分支。
3. 定位改写/优化器入口，做最小修复，补回归测试（遵循 `初赛/seekdb/AGENTS.md`：新头文件用 `#pragma once`、接口类保留 `I` 前缀、回归测试放既有测试目录）。
4. 构建并跑相关测试；记录“修复前后行为/延迟”对照。
5. 推送到 `Tomlegend2026/seekdb`（公开 fork）的新分支，向 `oceanbase/seekdb` 开 PR。
6. **在 PR 正文独立一行写入验证行**，保存后再去平台「官方 PR 链接」填 `https://github.com/oceanbase/seekdb/pull/<N>` 提测。

## 已知环境约束

- 本机 `git clone https://github.com/...` 会超时（300s），`git ls-remote` / `git push --dry-run` 可通；大文件传输不可靠。必要时用 GitHub 网页编辑，或改用 `git push` 只推增量提交。
- 平台提测存在配额/限流：同一账号 6 次后持续返回「该题目距离上一次提测时间间隔太短」，可换队友账号提测（官方允许任一队员提测）。

## 交付物清单（提交前自查）

- [ ] PR 链接可访问，目标仓库为 `oceanbase/seekdb`
- [ ] PR 正文含独立一行的 `OB-P:gd4uONi4eOGOgpU4rAg55Q`
- [ ] 修复有测试与前后对比数据
- [ ] `初赛/seekdb/提测记录.md` 记录 PR 链接与提测结果
