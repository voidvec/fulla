# Docker 部署与容器编排指南 (Docker Deployment)

本文档详细说明如何使用 Docker Compose 在本地或生产环境部署完整的 OAuth2 服务栈。

---

## 1. 服务栈架构

`docker-compose.yml` 编排以下 6 个服务：

```
Internet
    │
    │ :8080 / :8081
    ▼
┌────────────────────────┐   ┌────────────────────────┐
│  fulla-frontend        │   │  fulla-admin           │
│  Vue 用户前端 (Nginx)  │   │  管理后台前端 (Nginx)  │
│  Port: 8080            │   │  Port: 8081            │
└───────────┬────────────┘   └───────────┬────────────┘
            │         内网               │
            └────────────┬───────────────┘
                         ▼
            ┌────────────────────────┐
            │  fulla-backend         │
            │  Drogon 后端 :5555     │
            │  → postgres → redis    │
            └───────────┬────────────┘
                   ┌────┴─────┐
                   ▼          ▼
              postgres      redis
              (5433:5432)   (6380:6379)

              prometheus (9090:9090)
```

| 服务 | 镜像/构建 | 对外端口 | 说明 |
|---|---|---|---|
| `fulla-frontend` | `deploy/docker/Dockerfile` (`frontend-runtime`) | `8080:80` | Vue SPA (用户端) + Nginx |
| `fulla-admin` | `frontends/admin/Dockerfile` | `8081:80` | 管理后台前端 |
| `fulla-backend` | `deploy/docker/Dockerfile` (`backend-runtime`) | `5555:5555` | Drogon C++ 后端 |
| `fulla-postgres` | `postgres:17-alpine` | `5433:5432` | PostgreSQL（宿主机 5433，避开本地冲突）|
| `fulla-redis` | `redis:7-alpine` | `6380:6379` | Redis（宿主机 6380，避开本地冲突）|
| `fulla-prometheus` | `prom/prometheus:latest` | `9090:9090` | 指标采集 |

---

## 2. 快速启动

详见 [Docker 容器和镜像规范指南](#镜像--容器--网络命名规范)。

```bash
# 第一次或代码变更后：重新构建并启动（在项目根目录执行）
docker-compose -f deploy/docker/docker-compose.yml up -d --build

# 后续启动（无代码变更）
docker-compose -f deploy/docker/docker-compose.yml up -d

# 查看服务状态
docker-compose -f deploy/docker/docker-compose.yml ps

# 实时查看后端日志
docker-compose -f deploy/docker/docker-compose.yml logs -f fulla-backend

# 停止所有服务
docker-compose -f deploy/docker/docker-compose.yml down

# 停止并删除数据卷（数据库会被清空）
docker-compose -f deploy/docker/docker-compose.yml down -v
```

---

## 3. 环境变量与密钥注入

`fulla-backend` 在 `docker-compose.yml` 的 `environment` 节中通过环境变量注入敏感配置，**完全覆盖 `config.json` 中的默认值**。开发环境默认值（仅用于本地评估）如下：

```yaml
environment:
  - FULLA_DB_HOST=fulla-postgres         # 指向 Docker 内网的 postgres 服务名
  - FULLA_DB_NAME=fulla_db
  - FULLA_DB_PASSWORD=123456
  - FULLA_REDIS_HOST=fulla-redis
  - FULLA_REDIS_PASSWORD=redis_secret_pass
  - FULLA_VUE_CLIENT_SECRET=123456
  - FULLA_AUTO_MIGRATE=true               # 启动时自动执行 apps/server/migrations
  - FULLA_FRONTEND_URL=http://localhost:8080
  # SMTP 配置经 ${FULLA_SMTP_*:-} 占位从 .env.docker 注入；留空则回退到控制台模式
  - FULLA_SMTP_HOST=${FULLA_SMTP_HOST:-}
  ...
```

> **WARNING** **生产环境安全提示**：
> - **禁止**将真实密码直接写在 `docker-compose.yml` 中并提交到 Git。
> - 推荐使用 **Docker Secrets** 或外部密钥管理（Vault、AWS Secrets Manager）。
> - 最低要求：使用 `.env` 文件，并将其加入 `.gitignore`。

### 使用 `.env` 文件（推荐）

仓库提供了示例文件 `deploy/env/docker.env.example`（以及 `deploy/env/server.env.example`）。复制为 `.env.docker`（已在 `.gitignore` 中排除）并填入生产值：

```env
FULLA_DB_PASSWORD=your_strong_password
FULLA_REDIS_PASSWORD=your_redis_password
FULLA_VUE_CLIENT_SECRET=your_client_secret
# SMTP（留空则后端回退到控制台模式）
FULLA_SMTP_HOST=
FULLA_SMTP_PORT=465
...
```

然后 `docker-compose.yml` 中通过 `${VAR_NAME:-default}` 引用即可。

---

## 4. 数据持久化

通过命名 Volume 实现数据持久化，容器重启不丢数据：

```yaml
volumes:
  pgdata:    # PostgreSQL 数据文件
  redisdata: # Redis RDB / AOF 文件
```

数据库初始化由后端在启动时自动完成（`FULLA_AUTO_MIGRATE=true`，按文件名顺序执行 `apps/server/migrations/V*.sql`，再执行 `apps/server/seed/*.sql`）。`docker-compose.yml` 同时把迁移与种子脚本挂进 postgres 容器的子目录：

```yaml
volumes:
  - ../../apps/server/migrations:/docker-entrypoint-initdb.d/migrations:ro
  - ../../apps/server/seed:/docker-entrypoint-initdb.d/seed:ro
```

> **WARNING** **注意**：postgres entrypoint **不会**递归进入 `/docker-entrypoint-initdb.d` 的子目录，因此这两个挂载对首次初始化是 **no-op**，真正的 schema 初始化由后端的 `FULLA_AUTO_MIGRATE` 完成。

---

## 5. Prometheus 监控配置

`prometheus.yml` 配置 Prometheus 采集 `fulla-backend` 的 `/metrics` 端点：

```yaml
scrape_configs:
  - job_name: "fulla-backend"
    static_configs:
      - targets: ["fulla-backend:5555"]
```

Prometheus 与 fulla-backend 位于同一 Docker 网络 `oauth2-net`，使用服务名直接访问（无需暴露宿主机端口）。

访问 `http://localhost:9090` 即可查看 Prometheus UI。

---

## 6. 生产部署建议

### 6.1 在 Nginx 前端服务添加 SSL 终结

前端 `fulla-frontend` 的 Nginx 负责静态文件托管，应在其前面增加一层带 SSL 的 Nginx/Traefik：

```nginx
server {
    listen 443 ssl;
    server_name your-domain.com;

    ssl_certificate     /etc/ssl/certs/cert.pem;
    ssl_certificate_key /etc/ssl/private/key.pem;

    location / {
        proxy_pass http://fulla-frontend:80;
        proxy_set_header X-Forwarded-Proto https;
    }

    location /api/ {
        proxy_pass http://fulla-backend:5555;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
```

> **重要**：转发 `X-Forwarded-For` 头，确保后端的 Hodor 插件能正确获取真实客户端 IP。

### 6.2 屏蔽 `/metrics` 端点

Prometheus `/metrics` 端点不应暴露到公网，在 Nginx 中添加：

```nginx
location /metrics {
    deny all;
}
```

或通过 Docker 不对外暴露 `fulla-backend:5555`，仅允许 Prometheus 内网访问。

### 6.3 数据库连接池调优

生产环境建议将 `config.prod.json` 的 `number_of_connections` 从 `4` 调整为 `10-50`，根据实际并发量测试确定。

---

## 7. 健康检查与故障排查

```bash
# 检查所有容器状态
docker-compose ps

# 检查后端服务是否可达
curl http://localhost:5555/metrics

# 查看数据库是否已初始化
docker exec -it fulla-postgres psql -U fulla_user -d fulla_db -c "\dt"

# 查看 Redis 连接
docker exec -it fulla-redis redis-cli -a redis_secret_pass ping

# 清理并重建（数据会丢失）
docker-compose down -v
docker-compose up -d --build
```

## 镜像 / 容器 / 网络命名规范

| 镜像用途 | 名称 | 构建目标 | 说明 |
|---------|------|--------------------|------|
| 生产后端 | `fulla-backend` | `backend-runtime` | 仅运行时，体积小；GHCR 多架构发布 |
| 调试后端 | `fulla-backend-debug` | `backend-dev` | 含完整编译工具链 |
| 生产前端 | `fulla-frontend` | `frontend-runtime` | Nginx + 静态资源 |

容器命名：`fulla-{service}[-debug]`（backend/frontend/postgres/redis）；网络：Release 为 `oauth2-net`，Debug 为 `fulla-debug-net`（历史保留名见 compose 文件）。三份 compose 矩阵：`docker-compose.yml`（开发，6 服务）、`docker-compose.debug.yml`（调试，3 服务）、`docker-compose.prod.yml`（生产，8 服务含 Nginx 与 migrate 作业）。**所有 compose 命令在仓库根目录执行并带 `-f deploy/docker/...`**。

## 调试环境（挂载源码 / GDB）

```bash
docker build -f deploy/docker/Dockerfile --target backend-dev -t fulla-backend-debug:v1.3.2 .
docker compose -f deploy/docker/docker-compose.debug.yml up -d
docker compose -f deploy/docker/docker-compose.debug.yml run --rm debug-env bash
```

## 自动化验证

- `deploy/docker/docker-quick-verify-debug.sh`（容器内全流程：依赖检查 → 等 PG/Redis 就绪 → 建库 → 并行编译 → 单测）；
- `scripts/backend/full_test_docker.bat`（宿主一键：起容器 → 初始化 → ORM 重生成 → 编译 → 测试 → 起服 → OAuth2/Admin 端点测试 → 清理）。
