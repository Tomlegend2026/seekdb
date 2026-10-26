# 部署 seekdb

## 题目与要求

- 题目：赛题一，部署一个 seekdb（10 分）。
- 官方教程：https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lz6ixow11mar233g
- 部署平台不限，实例需要能执行 SQL。
- 提交实例实际生成的 `run/telemetry.json` 中 `content.id`，不要伪造或修改该值。

## 本机部署

环境：Windows、Docker Desktop Linux 容器。
镜像版本：seekdb-v1.4.0.0；完整镜像摘要固定在 `compose.yaml`。
数据卷：`oceanbase2026_seekdb-data`。
连接地址：`127.0.0.1:2881`，不对局域网开放。
容器内 root 当前无密码，仅用于本机练习，请勿公开暴露端口。

在本文件所在目录执行：

```powershell
docker compose -p oceanbase2026 -f compose.yaml up -d
docker logs --tail 30 oceanbase2026-seekdb
docker exec oceanbase2026-seekdb mysql --host=127.0.0.1 --port=2881 --user=root --execute="SELECT VERSION() AS version, 1 + 1 AS sql_check;"
```

2026-10-26 SQL 验证结果：

```text
version                                  sql_check
5.7.25-OceanBase seekdb-v1.4.0.0           2
```

## 部署 UUID

以下值读取自本机容器 `/var/lib/seekdb/run/telemetry.json` 的 `content.id`：

```text
79d70cdc-b03a-8185-ac43-dbbce52980f4
```

重新读取：

```powershell
$record = docker exec oceanbase2026-seekdb cat /var/lib/seekdb/run/telemetry.json | ConvertFrom-Json
$record.content.id
```

提测时只填写部署 UUID；本题不需要 Fork、PR 或验证行。
首次提测结果为 0 分（成功 0、失败 3），尚未确认部署项通过。
排查发现容器继承了不可用的代理 `host.docker.internal:12000`。
已在 compose 中清空本容器代理，并验证官方遥测地址可直接访问。
保留原数据卷和 hostname，重建容器后 UUID 不变，SQL 再次验证通过。
网络修复不等于平台已接收遥测，仍需再次提测核验。
若平台未查到遥测，保留实例运行，待同步后重试，不要修改 UUID。

## 暂停与恢复

```powershell
docker compose -p oceanbase2026 -f compose.yaml stop
docker compose -p oceanbase2026 -f compose.yaml start
```

不要删除数据卷；删除后重建实例可能会生成不同的 UUID。
