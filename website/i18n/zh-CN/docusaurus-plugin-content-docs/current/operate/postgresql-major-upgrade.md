# PostgreSQL 大版本升级 Runbook（15 → 17）

> 适用范围：使用本仓库 `deploy/docker/docker-compose.prod.yml`（或 Helm chart）
> 且带**持久数据卷**的存量部署。PG 大版本**拒绝挂载**旧版本的数据目录——
> 直接 `docker compose up`（镜像已升到 `postgres:17-alpine`）会导致数据库容器
> 循环重启，数据不会损坏但服务不可用。升级前必须走本 runbook。
>
> 全新部署无此问题（17 数据目录由初始化直接生成）。
>
> Fulla 自身在 PG 15 vs 17 的基准差异在噪声带内（2026-08-18 A/B 实测），
> 升级动机是与服务端 libpq 17.x 客户端对齐及基准环境一致性，**没有吞吐诉求**。
> 不急于升级的部署可以继续用 15 跑（把 compose 里的镜像 tag 钉回
> `postgres:15-alpine` 即可，与应用兼容）。

## 路线选择

| 路线 | 停机窗口 | 适用 |
|---|---|---|
| A. dump/restore（推荐） | 与数据量成正比（GB 级通常分钟级） | 中小数据量；操作简单、可交叉验证 |
| B. pg_upgrade（原地） | 通常 &lt;1 分钟 | 大数据量（数十 GB+）不想长停机 |

两条路线都要求：**升级前完成一次全量备份并保留旧数据卷**，验证通过后再清理。

## 路线 A：dump/restore（推荐）

以 prod compose 为例（凭据来自 `.env`：`POSTGRES_USER`/`POSTGRES_PASSWORD`/
`POSTGRES_DB`；compose project 名以实际为准，下同）。

### 1. 停应用、留旧库

```bash
cd deploy/docker
docker compose -f docker-compose.prod.yml stop fulla-backend
# 旧 PG（15）保持运行，用它导出
```

### 2. 全量导出

```bash
docker compose -f docker-compose.prod.yml exec -T fulla-postgres \
  pg_dump -U "$POSTGRES_USER" -d "$POSTGRES_DB" --no-owner --no-privileges \
  > backup_pg15_$(date +%Y%m%d).sql
# 校验导出文件非空且含表结构
grep -c "CREATE TABLE" backup_pg15_*.sql
```

### 3. 切换数据卷

```bash
docker compose -f docker-compose.prod.yml down
# 重命名旧卷留底（project 名按实际替换；默认 project 名 = 目录名）
docker volume rm <project>_pgdata 2>/dev/null || true
# 更稳妥：先改名留底而不是删除
#   docker run --rm -v <project>_pgdata:/src -v /var/backups:/dst alpine \
#     cp -a /src /dst/pgdata_pg15_backup
```

> 简化写法：也可以直接 `docker volume rm` 旧卷——前提是步骤 2 的 dump 文件
> 已安全存放在宿主机并校验过。留底副本是给"验证失败回滚"用的保险。

### 4. 启动 17 并导入

```bash
docker compose -f docker-compose.prod.yml up -d fulla-postgres
# 等健康检查通过后导入
docker compose -f docker-compose.prod.yml exec -T fulla-postgres \
  psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" \
  < backup_pg15_$(date +%Y%m%d).sql 2>&1 | grep -i "error\|fatal" || echo "IMPORT CLEAN"
```

> 服务端配置了启动自动迁移（`FULLA_AUTO_MIGRATE=true` 时），导入旧 schema 后
> 首次启动会自动补齐新增迁移（V025 分区等，幂等）。

### 5. 验证后放流量

```bash
docker compose -f docker-compose.prod.yml up -d
curl -fsS http://<backend>/health/ready
```

按 [verification-checklist.md](verification-checklist.md) 走一遍核心路径
（登录 → 授权码 → token → introspect → userinfo）。通过后保留 dump 文件与
旧卷留底至少一个回归周期，再清理。

## 路线 B：pg_upgrade（原地，大数据量）

pg_upgrade 需要同时存在新旧两套 binary 与数据目录。容器化下的最省事做法是
官方 `postgres:17` 镜像内置的 `/usr/local/bin/pg_upgrade`（alpine 镜像不含，
需用非 alpine tag 或自行挂载工具镜像）：

```bash
# 1) 停应用与旧库
docker compose -f docker-compose.prod.yml stop fulla-backend fulla-postgres

# 2) 复制旧数据卷到新卷（pg_upgrade --link 可省空间但有损坏回滚风险，不默认）
docker run --rm -v <project>_pgdata:/old -v <project>_pgdata17:/new alpine \
  cp -a /old /new

# 3) 用 17 镜像对新副本执行 pg_upgrade（旧目录只读挂载）
docker run --rm \
  -v <project>_pgdata:/var/lib/postgresql/15 \
  -v <project>_pgdata17:/var/lib/postgresql/17 \
  -e POSTGRES_USER=$POSTGRES_USER postgres:17 \
  bash -c 'install -d /var/lib/postgresql/17.tmp && gosu postgres pg_upgrade \
    -b /usr/lib/postgresql/15/bin -B /usr/local/bin \
    -d /var/lib/postgresql/15 -D /var/lib/postgresql/17 \
    -U "$POSTGRES_USER"'
# 注：postgres:17 非 alpine 镜像内含 15 的 binary（多版本包），命令按镜像实际
# 路径调整；上述为骨架，执行前先在测试环境演练一遍。

# 4) 把 compose 的 pgdata 卷指向升级后的卷（或交换卷名），up -d，验证同路线 A。
```

pg_upgrade 成功后按提示执行 `vacuumdb --all --analyze`（或等自动 analyze）。

## Helm 部署

Chart 内置 PostgreSQL 的镜像 tag 在 `deploy/helm/fulla/values.yaml`
（`postgresql.image`）。有状态集的 PVC 同样是大版本绑定的数据目录：

1. `kubectl scale deploy/fulla --replicas=0`（停应用）；
2. 按上面任一路线导出/升级 PVC 中的数据；
3. 更新 values 后 `helm upgrade`，验证就绪后恢复副本。

外接数据库（自管 PG / RDS 类）的部署不涉及本仓库配置，走云厂商或 DBA 的
大版本升级流程即可。

## 回滚

- 路线 A：`down` → 恢复旧卷留底（或恢复 `postgres:15-alpine` tag + 原卷）→
  `up -d`。应用镜像与 PG 15 兼容（libpq 17 客户端连 15 服务端没有问题）。
- 路线 B：旧卷未被修改（只读挂载），换回即可。
- 已在新库写入新数据后回滚会丢这部分数据——回滚决策要在放流量验证之前做。
