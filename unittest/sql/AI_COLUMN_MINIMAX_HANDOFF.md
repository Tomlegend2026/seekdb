# MiniMax 接续指令：seekdb AI COLUMN

请先完整读取本文件，再直接继续实现和验证。用户已授权修复、测试，并将验证过的成果提交到原 GitHub 私有答题仓库。不需要反复问“是否继续”。本次先完成 AI COLUMN（50 分），随后再处理第四题全文索引性能优化。不要重新处理基础实践 30 分，不动队友 PowerContext。

## 工作位置与安全边界

- Windows 主仓库：D:\desktop\Partial\Competition\OceanBase\初赛\seekdb
- 当前分支：feat/ai-column；HEAD：a4b8b4858（feat(ai): persist column definitions with DDL）。当前有大量未提交修改及新增源文件，是需要接续的成果；先 git status/diff，不要 reset/clean/checkout 覆盖。
- 原远端：https://github.com/Tomlegend2026/oceanbase2026-seekdb，必须保持私有。保持当前仓库地址和团队权限，不删除、改名、转移或重建仓库，不推坏代码到 main 或已得分分支。
- 当前修复尚未提交、尚未推送，尚无新提测分数。main/已得分分支未被本轮合并修改。
- 读取根目录 AGENTS.md。新头文件用 pragma once；不要仅为命名更改已有类。
- Windows 用 PowerShell；默认不要用 fish。本机 Windows 没有 python 命令，Python 用 WSL Ubuntu 的 python3。
- 不使用昂贵 DeepSeek harness，不启动其他付费代理。浏览器确有需要时，先读 D:/code/huawei/local-browser/README.md，发现 local_browser 工具，先 browser_tabs；不使用 TinyFish，不读取凭据，登录/验证码由用户完成。
- 不修改用于基础实践 UUID 遥测的 Docker 部署（2881），不删除旧测试实例或数据卷。

## 当前真实状态（优先于其他报告）

其他代理写的 AI_COLUMN_COMPLETION_REPORT.md、AI_COLUMN_FINAL_SUMMARY.md、AI_COLUMN_IMPLEMENTATION.md 等“100%完成”报告不可信，不作为证据，也不要顺手全部提交。FTS 完成报告同理。unittest/sql/AI_COLUMN_TESTING.md 原先记录的是修复前审计，里面“Bison失败”等部分已经过时。

已经做的修复：

1. 补 RETRY 非保留关键字、修复语法中的 opt_where_clause 为已有 opt_where，同步两份 ob_item_type.h 的新节点。严格 Bison 2.4.1 生成已通过；镜像内已运行 gen_parser.sh。
2. 新源文件已加入 share/sql source inventories，头文件归属已调整，真实生产构建会编译 Worker/Handler/Helper。
3. ObAiColumnTask 继承 ObSchema，字符串深拷贝，assign/reset 正确；新增原生任务数据生命周期测试。
4. Worker 改为 lib::Threads，修正不存在的头文件/API；模型结果由调用方 allocator 持有；模型调用改为在实际业务行上执行原生 AI_COMPLETE/AI_EMBED，而非把 SQL 文本当 prompt 发 HTTP。
5. AI_COMPLETE 普通用户仍只允许 2/3 参数；非用户 inner SQL 可用第四个内部类型提示参数。call_completion 复用已有 provider 请求构造，将提示放入 system prompt。需实测内部调用和原有调用兼容性。
6. DML helper 使用 ObTableModifyOp 的同会话 nested SQL，替代另开 sql_proxy 写任务；错误不再被吞。事务原子性尚未通过真实 DML/回滚验收，不能仅根据接线宣称已满足。
7. 主键值使用类型化 SQL，字符串/外层谓词使用二进制 hex literal，避免引号、NUL、NO_BACKSLASH_ESCAPES 的转义问题；谓词按实际完整主键顺序构造，内存由调用方持有。
8. DDL 绑定复用生成表达式解析器，收集真实依赖（含 CONCAT 内列），规范打印表达式；保存类型提示，构造 __ai_state_<id>/__ai_request_<id>/__ai_result_spec_<id> 隐藏列；WITH 选项有类型/重复/递归数量检查。
9. CREATE 在全部普通列解析后再绑定 AI 列，避免向前引用和列 ID 碰撞；add_ai_column_info 通过已有 set_ai_column_infos 正确扩容并校验 AI→AI 依赖。
10. UPDATE resolver 为 AI 结果/隐藏列增加独立的新行表达式，避免清空新结果时破坏旧值和索引删除所用旧行。DML 根据新旧 datum 实际变化挑选依赖，主键变化触发全部 AI 列，删除旧键任务。为同一行触发的列共享 request UUID，生成任务时填 PENDING/request/spec 并把结果置 NULL。以上新行/索引逻辑需要真实联调，仍可能有投影、缓存或索引维护缺口。
11. 普通 UPDATE 和基础表 INSERT 的显式 AI 结果赋值已有拒绝检查；视图、MERGE、INSERT ON DUPLICATE 等还需检查，不能假定全覆盖。
12. 删除了 Handler 里未使用且不安全的重复任务 SQL API，现在只保留共享主键谓词构造。

构建与测试证据：

- 加入真实新源文件后的完整 seekdb 构建通过。
- ai_function_regression、ai_column_schema_regression、ai_column_task_regression 三个 CTest target 曾全部通过；任务生命周期测试含 2 个 case，Schema/Catalog 原有 7 个 case。
- 后来发现构建镜像残留另一版本的 6 个源文件，导致端点注册报 Unknown column max_concurrency。已逐文件备份，然后以 Windows 当前分支覆盖这 6 个文件，再次完整构建及三个测试二进制构建通过；覆盖后 CTest 还需要重跑。
- 再次运行真实官方 DDL，模型和端点注册均通过；到 CREATE AI 表时返回 ERROR 4019 Size overflow。
- 已定位并修复我们新 CREATE 接线的列 ID 初始化问题：resolve_ai_columns 在普通最终 max-used-column-id 设置之前执行；现在先 table.set_max_used_column_id(cur_column_id_) 再分配隐藏列，最后更新 cursor。该最新修复的 seekdb 已构建成功，但尚未重启实例运行验证。请从这里接续。
- 当前实例仍运行修复前的进程，不能直接重跑并认为新代码没有生效。
- 没有任何官方五组 AI COLUMN 验收通过的证据，不得声称 50 分完成。

## 构建环境：现成可用，不需要重新下载 devtools

WSL Ubuntu 镜像根：
/home/tom/oceanbase2026-seekdb-build/seekdb-5bce9e06d1281347ad61344e4b01fab9158c58a2

Windows 可通过 UNC 访问：
\\wsl.localhost\Ubuntu\home\tom\oceanbase2026-seekdb-build\seekdb-5bce9e06d1281347ad61344e4b01fab9158c58a2

Windows 仓库是权威版本。编辑后同步相关文件到镜像再构建；不要只改 Linux 镜像而忘记保存到 Git 工作区。镜像本身不是 git 仓库。

可用 Clang17、Bison2.4.1；Cargo 在 /home/tom/.cargo/bin/cargo。WSL 8GB RAM+2GB swap，使用 -j4，避免 OOM。不要把带 Windows 空格路径的 $PATH 直接拼进 bash；用下面明确 PATH：

```sh
cd /home/tom/oceanbase2026-seekdb-build/seekdb-5bce9e06d1281347ad61344e4b01fab9158c58a2
env PATH=/home/tom/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  cmake --build build_release --target seekdb ai_column_task_regression ai_column_schema_regression ai_function_regression -j4
cd build_release
ctest --output-on-failure -R '^(ai_column_task_regression|ai_column_schema_regression|ai_function_regression)$'
```

语法变更后在镜像 src/sql/parser 运行 bash gen_parser.sh；生成器和依赖已经能工作。留意生成物是否需要同步，别手改生成 C 代替 .y。

原生二进制：build_release/src/observer/seekdb。定向编译脚本 /home/tom/ai-column-draft-audit.py 使用生产 compile_commands 和 Windows include 根，可改 files 列表检查少量修改；它不是完整编译或功能验证的替代品。不要将缺少 unity 隐式 include 的单文件错误，误认为生产构建必然失败。

## 最新实例、日志和源码一致性

- 本轮专用实例：/home/tom/ai-column-functional-20261030，SQL 端口 2885。
- 最近核验运行进程 PID 1732，命令行明确包含上述 base-dir。PID 会变化，必须重新检查完整命令行，确认后只终止这个测试实例，再启动新二进制。不要 killall/pkill seekdb。
- Codex 启动的 PTY session 为 52568；MiniMax 可能无法接管该句柄，按精确 PID 温和关闭即可。
- 启动示例（前台运行并保持会话；后台运行也需确保 WSL 不会退出）：

```sh
/home/tom/oceanbase2026-seekdb-build/seekdb-5bce9e06d1281347ad61344e4b01fab9158c58a2/build_release/src/observer/seekdb \
 --nodaemon --base-dir=/home/tom/ai-column-functional-20261030 --port=2885 \
 --parameter memory_budget=2G --parameter log_disk_size=2G \
 --parameter datafile_size=512M --parameter datafile_maxsize=2G
```

- 初次旧版启动时有部分系统包记录 Size overflow，如有相关影响应使用新的隔离实例目录；保留旧目录，不删除。
- MySQL 客户端 /usr/bin/mysql 可用；Python 默认没有 PyMySQL，已有 DDL/目录脚本通过 mysql CLI 工作，不需要额外依赖。
- 新 DDL 验收脚本（真实执行官方 fixture，断言错误码及元数据结果）：
  unittest/sql/ai_column_ddl_integration.py --port 2885
  可直接在 WSL 用 /mnt/d/desktop/Partial/Competition/OceanBase/初赛/seekdb/unittest/sql/ai_column_ddl_integration.py 运行。
- 最新 ID 修复构建日志：/home/tom/ai-column-ddl-id-fix-build.log（已到 Built target seekdb）。
- 对齐源码后的构建日志：/home/tom/ai-column-parity-rebuild.log。
- DDL 最近失败日志：/home/tom/ai-column-ddl-acceptance.log。
- 实例日志：/home/tom/ai-column-functional-20261030/log/seekdb.log；启动 stdout /home/tom/ai-column-functional-server.log。
- 历史三组测试日志 /home/tom/ai-column-native-tests.log 是源码对齐前结果；请重跑刷新，别当最新。
- 镜像源码一致性脚本：/home/tom/check-ai-source-parity.py；结果 /home/tom/ai-column-source-parity.json。它比较 git 跟踪 src 中主要源码（不包括全部构建脚本和测试文件），不能替代完整清单。
- 6 个残留文件原件备份在 /home/tom/ai-build-residual-backup-20261030，附 manifest.json。不要未经检查把它们恢复进当前镜像：它们来自另一版 AI 并发实现，和当前系统表不配套。备份保留，未丢弃用户代码。

## 立即执行顺序

1. 检查工作区/当前分支，读相关源码和本指令；不要从“下载依赖”重来。
2. 重启 2885 的专用实例，确认加载最新 ID 修复二进制；先测普通 CREATE（含有/无主键）、普通 INSERT/UPDATE/DELETE，避免 AI 接线破坏普通 SQL。
3. 重跑 DDL 脚本，修到全部通过。当前修复后的 CREATE 尚未验证，不能先假定已经成功。之后 ALTER ADD 很可能仍失败：原型只在部分 AlterTableSchema 上绑定，原表依赖/隐藏列的 ALTER 操作类型/最终列 ID 重映射/新元数据随 ALTER 持久化链路都未实现完整。SHOW AI COLUMNS 也需要实际核验。
4. 先做真实 DML/事务验收，确认任务与业务写同事务、结果失效、新旧投影、主键变化、批量、删除和回滚。不要在这些基础未证实时贸然开启不安全 Worker。
5. 完善并接入 Worker 生命周期（当前没有 ObServer 启动/关闭接线）。当前 process_task 仍逐列发布、任务不清理完整、无可靠 RUNNING 恢复，不满足官方要求。实现原子并发领取/恢复、必要重试、结果转换、请求 ID+定义版本+任务归属校验、分组发布、终态清理。不要把每列单独提交当分组发布。
6. 实现 RETRY 真正执行、状态伪列与反引号业务 status 区分、严格读取系统变量及列级检查。当前仅补了 parser 骨架，不存在完整可用路径，不能通过打印 SQL 当作执行。
7. 验证 AI 向量的生成、失效、旧索引删除、新索引加入、ANN 和回滚。不允许通过缓存模型结果、硬编码主键/模型/Mock 响应绕过测试。
8. 五套官方用例和必要故障场景通过、普通 SQL/AI 函数回归通过后，更新真实进度，选择性 git add 源码/测试/有效文档，提交并推送原私有远端分支。用户已授权推送；不要提交个人报名资料、凭据、构建目录或虚假完成报告。推送后核对远端 commit，不要只声称上传。

## 官方验收位置及必须满足的语义

实际 fixture 路径是 tools/deploy/mysql_test/test_suite/ai_column，不是用户题面旧路径 ai_function。

- t/ai_column_ddl.test：建表、ALTER ADD、元数据、每列 3 个隐藏列、类型提示、维度 7600、AI→AI 依赖拒绝 1235。
- t/ai_column_t1_dml.test：事务内任务可见/提交后请求、依赖更新失效、无关更新零请求、主键变更、批量、回滚、删除清理。
- t/ai_column_worker.test：同一行同次变更共享请求组、多模型输入和 system 类型提示、无部分可见、重试/失败、迟到结果保护。
- t/ai_column_retry_strict.test：PENDING/READY/FAILED、按状态/主键/业务条件重试（允许 READY）、status 伪列/`status` 业务列、严格读取 SELECT/UPDATE/DELETE 的正确错误语义。
- t/ai_column_vector_index.test：向量、VSAG/HNSW、ANN、依赖失效与旧索引移除、重算和回滚。
- include/ai_column_mock.py：本地 Mock 服务，端口 19120～19124，无需真实模型 key。不要改其行为、期望或测试以掩盖实现缺陷。
- r/mysql/*.result：对照官方期望。Mysqltest 有连接状态、循环、sleep 和变量，不能把事务语句拆成互不相关的 mysql 进程后声称通过。

关键要求：模型请求只能在业务提交后发生；回滚不能残留任务/请求；默认未就绪/失败结果 NULL 但状态必须独立；同一行同次变更的一组 AI 列达到终态后一个事务发布，允许组内 READY 与 FAILED 并存；旧请求不得覆盖新结果/复活删除行；严格读取检查“引用列在整张表是否存在未就绪行”，不能仅检查 WHERE 命中的行，普通列和其他全部就绪 AI 列不受影响，且不可绕过权限泄漏受保护表状态。

当前任务枚举是 PENDING=0、RUNNING=1、COMPLETED=2、FAILED=3；官方 worker fixture 对“等待重试 state=3”有断言，现有枚举/调度逻辑不匹配，需要设计完整状态机并一致实现，而非简单改一个断言。

## 提交与门禁注意事项

- Windows 写文件用 UTF-8 无 BOM、LF。git -c core.autocrlf=false diff --check；不要造成数千行 CRLF 噪声。
- share_header_ownership_check 曾通过；SQL 门禁原先发现 Worker 引用 SQL 私有 AI 头，现已移除这些无用 include。另有预先存在、未跟踪的 engine/basic/file_scan_parquet.h 原型会触发本地 SQL 门禁缺失归属；不要误删、误提交该原型，也不要把其旧问题算成 AI 新修复已通过。
- 生产源码新增必须注册 share_source_inventory.bzl/sql_source_inventory.bzl，头文件按真实跨模块使用注册；只编译没注册的新 .cpp 不能证明最终程序有效。
- 如需代理可沿用已知 Git 网络设置：http.proxy=http://127.0.0.1:7890、http.version=HTTP/1.1。不要输出 token 或读取 Cookie。
- 此轮没有新增官方 PR 或提测；先完成真实实现。基础实践官方 PR #1905 与 30 分是旧任务，不要重做。
- 小步做完一个可验证模块就报告实测结果和剩余缺口，持续推进。不得用“代码完整性100%”“再写七份总结”代替构建、实例、Mock 和官方断言通过。
