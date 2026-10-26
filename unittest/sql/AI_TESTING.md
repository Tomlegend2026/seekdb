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

2026-10-26：原有七个文件通过 Clang 17 / C++20 检查；新增 HTTP 客户端检查另行执行。Rust 工作区依赖构建通过。完整构建仍未完成。

进一步修复 JSON 序列化返回局部缓冲区的问题，并增加记录释放地址的回归测试，防止常见的 arena 分配器掩盖提前释放。HTTP 请求使用 libcurl 复制请求体，避免局部 JSON 缓冲区析构后仍被引用；单次及批量 HTTP 响应在转换成对象前检查根节点类型。

新增的 `unittest/CMakeLists.txt` 已通过生产 CMake 配置，可在完整依赖编译后链接并运行 C++ 测试：

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

## 尚未完成的验证

- `test_ai_function_regression.cpp` 尚未链接运行。本队已补上独立 CMake 入口，等待生产共享库构建完成。
- `ai_function_integration.py` 尚未对修改后编译的数据库运行。
- 新增的 `ai_prompt_regression.test` 尚未运行。
- 未重新提测，未声称平台得分或全部 seekdb 赛题已完成。
