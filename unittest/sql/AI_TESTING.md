# AI 函数本地验证

这些检查针对本队修改后的源码。现成 Docker 部署镜像不能替代源码构建产物。

## 编译检查

先完成官方依赖初始化和 Release 配置，并生成 Rust 桥接头文件：

```sh
bash build.sh release --init
cmake -S . -B build_release -DCARGO:FILEPATH="$HOME/.cargo/bin/cargo" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build_release --target rust_staticlibs_build -j2
python3 unittest/sql/check_ai_sources.py --build-dir build_release
```

`check_ai_sources.py` 从实际构建目录读取编译参数，检查七个实现文件和 C++ 回归测试。它只证明语法与类型检查通过，不执行测试，不验证链接或 SQL 行为。

2026-10-26：八个文件通过 Clang 17 / C++20 检查；Rust 工作区、完整 seekdb 可执行程序及 C++ 回归目标均构建成功。14 项 AI C++ 回归已运行通过。

进一步修复 JSON 序列化返回局部缓冲区的问题，并增加记录释放地址的回归测试，防止常见的 arena 分配器掩盖提前释放。HTTP 请求使用 libcurl 复制请求体，避免局部 JSON 缓冲区析构后仍被引用；单次及批量 HTTP 响应在转换成对象前检查根节点类型。

新增的 `unittest/CMakeLists.txt` 接入生产共享库，以下检查已实际执行通过：

```sh
cmake --build build_release --target ai_function_regression -j2
ctest --test-dir build_release -R '^ai_function_regression$' --output-on-failure
```

## SQL 联调

在修改后的 seekdb 实例启动后执行：

```sh
python3 unittest/sql/ai_function_integration.py --mysql-client /path/to/mysql --port 2881
```

默认连接本机 `root`、数据库 `test`。认证使用客户端正常配置或 `MYSQL_PWD`，不要把密码写入仓库。脚本创建随机前缀的专用表、模型和端点，退出时清理；不会调用真实收费模型。

脚本会在本机随机端口启动模拟服务，通过真实 SQL 检查：

- LONGTEXT、Unicode、重复占位符、嵌套提示及 JSON 对象/数组。
- OpenAI Completion 和无需密钥的大小写混合 Ollama。
- 显式向量维度，以及无效维度在发起 HTTP 前被拒绝。
- 畸形响应、错误向量维度、非对象的响应根节点及错误后数据库仍可查询。
- 130 行输入中的 NULL 跳过、真正批量请求，以及响应乱序后的逐行还原。
- 61 篇文档跨批次的全局索引、并列分数稳定排序、对象文档映射。

若数据库运行在其他网络空间，需要显式设置 `--mock-bind` 和 `--model-host`，确保数据库能够访问模拟服务。默认仅监听本机。

也可独立启动 `ai_function_mock.py --port 8000`，供现有 `ai_function_enhancement` mysqltest 使用。

## 已完成的 SQL 联调

2026-10-26，在修改后编译的原生 seekdb、独立目录 `/home/tom/oceanbase2026-test-instance-v2` 和端口 2882 上执行 `ai_function_integration.py --port 2882`，全部检查通过。

- LONGTEXT / JSON / 嵌套提示 / OpenAI Completion / Ollama：通过。
- 单行 Embedding、显式维度、无效维度在 HTTP 前拒绝、错误响应及向量维度：通过。
- 130 行输入、NULL 跳过、输入输出逐行核对：通过，实际为 2 次 HTTP 请求，每次不超过 64 条有效输入。
- 61 篇文档跨不等长批次 Rerank、全局索引、稳定并列排序和对象映射：通过。

首次联调发现测试脚本错误地把 Completion 的原始 SQL 文本再当 JSON 字符串解析；已修正测试，随后完整重跑通过。

新建测试实例应使用 `memory_budget=2G`，不是已废弃且不控制实际预算的 `memory_limit`；`log_disk_size` 至少 2G。本实例仅用于源码验证，不替代报名使用的 Docker UUID。

同日，通过 mysql 客户端执行独立 `ai_prompt_regression.test` 中的 SQL，并精确核对六行输出：`ARRAY`、`OBJECT`、`inner {0}`、`70000`、`70000`、`E4BDA0`，通过。没有运行 mysqltest 调度器。

尚未重新提测，未声称平台得分或全部 seekdb 赛题完成。

FileSQL 原生接入后重新完整构建 seekdb，并在新建的独立实例 `/home/tom/oceanbase2026-filesql-test-instance` 上再次执行 `ai_function_integration.py --port 2882`，上述四组联调全部通过。
