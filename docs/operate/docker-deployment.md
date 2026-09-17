# Docker Deployment and Container Orchestration Guide

This document explains how to deploy the complete OAuth2 service stack with Docker Compose, locally or in production.

---

## 1. Service Stack Architecture

`docker-compose.yml` orchestrates the following 6 services:

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

| Service | Image/build | Exposed port | Notes |
|---|---|---|---|
| `fulla-frontend` | `deploy/docker/Dockerfile` (`frontend-runtime`) | `8080:80` | Vue SPA (user-facing) + Nginx |
| `fulla-admin` | `frontends/admin/Dockerfile` | `8081:80` | Admin console frontend |
| `fulla-backend` | `deploy/docker/Dockerfile` (`backend-runtime`) | `5555:5555` | Drogon C++ backend |
| `fulla-postgres` | `postgres:17-alpine` | `5433:5432` | PostgreSQL (host port 5433, avoiding local conflicts)|
| `fulla-redis` | `redis:7-alpine` | `6380:6379` | Redis (host port 6380, avoiding local conflicts)|
| `fulla-prometheus` | `prom/prometheus:latest` | `9090:9090` | Metrics collection |

---

## 2. Quick Start

See the [Docker image and container specification guide](#image--container--network-naming-conventions).

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

## 3. Environment Variables and Secret Injection

`fulla-backend` receives sensitive configuration through environment variables in the `environment`
section of `docker-compose.yml`, **fully overriding the defaults in `config.json`**. The development
defaults (for local evaluation only) are:

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

> **WARNING** **Production security notes**:
> - **Never** write real passwords directly into `docker-compose.yml` and commit them to Git.
> - **Docker Secrets** or an external secret manager (Vault, AWS Secrets Manager) is recommended.
> - Minimum requirement: use an `.env` file and add it to `.gitignore`.

### Using an `.env` file (recommended)

The repository ships the example files `deploy/env/docker.env.example` (and `deploy/env/server.env.example`). Copy one to `.env.docker` (already excluded via `.gitignore`) and fill in production values:

```env
FULLA_DB_PASSWORD=your_strong_password
FULLA_REDIS_PASSWORD=your_redis_password
FULLA_VUE_CLIENT_SECRET=your_client_secret
# SMTP（留空则后端回退到控制台模式）
FULLA_SMTP_HOST=
FULLA_SMTP_PORT=465
...
```

Then reference the values from `docker-compose.yml` via `${VAR_NAME:-default}`.

---

## 4. Data Persistence

Data persistence is achieved through named volumes, so container restarts do not lose data:

```yaml
volumes:
  pgdata:    # PostgreSQL 数据文件
  redisdata: # Redis RDB / AOF 文件
```

Database initialization is performed automatically by the backend at startup (`FULLA_AUTO_MIGRATE=true`
executes `apps/server/migrations/V*.sql` in filename order, followed by `apps/server/seed/*.sql`).
`docker-compose.yml` also mounts the migration and seed scripts into subdirectories of the
postgres container:

```yaml
volumes:
  - ../../apps/server/migrations:/docker-entrypoint-initdb.d/migrations:ro
  - ../../apps/server/seed:/docker-entrypoint-initdb.d/seed:ro
```

> **WARNING** **Note**: the postgres entrypoint does **not** recurse into subdirectories of
> `/docker-entrypoint-initdb.d`, so these two mounts are **no-ops** for first-time
> initialization; actual schema initialization is performed by the backend's `FULLA_AUTO_MIGRATE`.

---

## 5. Prometheus Monitoring Configuration

`prometheus.yml` configures Prometheus to scrape the `/metrics` endpoint of `fulla-backend`:

```yaml
scrape_configs:
  - job_name: "fulla-backend"
    static_configs:
      - targets: ["fulla-backend:5555"]
```

Prometheus sits on the same Docker network as fulla-backend, `oauth2-net`, and reaches it directly by service name (no host port needs to be exposed).

Visit `http://localhost:9090` for the Prometheus UI.

---

## 6. Production Deployment Recommendations

### 6.1 Add SSL termination in front of the frontend service

The Nginx inside `fulla-frontend` serves static files; put an SSL-terminating Nginx/Traefik layer in front of it:

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

> **Important**: forward the `X-Forwarded-For` header so the backend's Hodor plugin can obtain the real client IP.

### 6.2 Block the `/metrics` endpoint

The Prometheus `/metrics` endpoint must not be exposed to the public internet. Add to Nginx:

```nginx
location /metrics {
    deny all;
}
```

Alternatively, do not expose `fulla-backend:5555` through Docker at all, allowing access only to Prometheus on the internal network.

### 6.3 Database connection pool tuning

For production, consider raising `number_of_connections` in `config.prod.json` from `4` to `10-50`, determined by testing against actual concurrency.

---

## 7. Health Checks and Troubleshooting

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

## Image / Container / Network Naming Conventions

| Image purpose | Name | Build target | Notes |
|---------|------|--------------------|------|
| Production backend | `fulla-backend` | `backend-runtime` | Runtime only, small footprint; multi-arch GHCR release |
| Debug backend | `fulla-backend-debug` | `backend-dev` | Includes the full compilation toolchain |
| Production frontend | `fulla-frontend` | `frontend-runtime` | Nginx + static assets |

Container naming: `fulla-{service}[-debug]` (backend/frontend/postgres/redis); networks: `oauth2-net` for Release, `fulla-debug-net` for Debug (see the compose files for legacy retained names). Three compose matrices: `docker-compose.yml` (development, 6 services), `docker-compose.debug.yml` (debug, 3 services), and `docker-compose.prod.yml` (production, 8 services including Nginx and a migrate job). **All compose commands are run from the repository root with `-f deploy/docker/...`**.

## Debug Environment (source mounts / GDB)

```bash
docker build -f deploy/docker/Dockerfile --target backend-dev -t fulla-backend-debug:v1.3.2 .
docker compose -f deploy/docker/docker-compose.debug.yml up -d
docker compose -f deploy/docker/docker-compose.debug.yml run --rm debug-env bash
```

## Automated Verification

- `deploy/docker/docker-quick-verify-debug.sh` (full in-container pipeline: dependency check → wait for PG/Redis readiness → create the database → parallel build → unit tests);
- `scripts/backend/full_test_docker.bat` (one-click on the host: start containers → initialize → regenerate the ORM → build → test → start the server → OAuth2/Admin endpoint tests → cleanup).
