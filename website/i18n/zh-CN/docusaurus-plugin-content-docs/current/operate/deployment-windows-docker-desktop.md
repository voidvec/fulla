# Windows Docker Desktop 部署验证指南

本指南说明如何在 Windows Docker Desktop 上验证 fulla 全栈系统的部署，**除了域名和 SSL 外，其他所有功能与 Linux 生产环境完全一致**。

---

## 为什么使用 Windows Docker Desktop 验证？

✓ **完全模拟生产环境**：使用相同的 Docker Compose 配置、相同的容器镜像、相同的网络拓扑  
✓ **快速反馈循环**：本地修改代码 → 立即验证 → 确认无误后再推送到 Linux 服务器  
✓ **节省时间**：避免每次"推送 → 服务器拉取 → 重启服务 → 发现问题"的漫长循环  
✓ **核心功能全覆盖**：数据库迁移、API 端点、前端路由、OAuth2 流程全部可测试  

**与 Linux 生产环境的差异**：
| 功能 | Windows Docker Desktop | Linux 生产环境 |
|------|------------------------|----------------|
| PostgreSQL | ✓ 完全相同 | ✓ |
| Redis | ✓ 完全相同 | ✓ |
| 后端 API | ✓ 完全相同 | ✓ |
| 前端 | ✓ 完全相同 | ✓ |
| 管理后台 | ✓ 完全相同 | ✓ |
| Nginx 反向代理 | ⚠ 简化配置（无 TLS） | ✓ |
| 域名访问 | ✗ 使用 localhost | ✓ |
| SSL/TLS | ✗ 不启用 | ✓ |

---

## 前置条件

### 软件要求

1. **Windows 10/11 专业版或企业版**（家庭版需要 WSL2 手动配置）
2. **Docker Desktop for Windows**（最新稳定版）
   - 下载：https://www.docker.com/products/docker-desktop/
   - 安装时启用 WSL2 后端（推荐）或 Hyper-V
3. **Git**（用于克隆项目）
   - 下载：https://git-scm.com/download/win
4. **OpenSSL**（用于生成 JWT 密钥，可选）
   - Windows: 下载 https://slproweb.com/products/Win32OpenSSL.html
   - 或使用 Git Bash 自带的 OpenSSL

### 验证 Docker Desktop 安装

打开 PowerShell 或 Windows Terminal：

```powershell
# 检查 Docker 版本（要求 20.10+）
docker --version

# 检查 Docker Compose 版本（要求 v2+）
docker compose version

# 检查 Docker 是否正常运行
docker ps
```

预期输出示例：
```
Docker version 24.0.7, build afdd53b
Docker Compose version v2.23.0
CONTAINER ID   IMAGE     COMMAND   CREATED   STATUS    PORTS     NAMES
```

---

## 快速开始（5 步）

### 1. 克隆项目

```powershell
# 克隆仓库（替换为实际地址）
git clone <repo-url>
cd fulla

# 检查分支
git branch
```

### 2. 生成 JWT 密钥

**方法 A：使用 Git Bash（推荐）**

```bash
# 在项目根目录执行
cd /path/to/repo-root

# 生成 JWT 签名密钥
chmod +x scripts/generate-jwt-keys.sh
./scripts/generate-jwt-keys.sh

# 验证密钥生成
ls -la deploy/keys/
# 应该看到 signing.pem 和 signing.pub
```

**方法 B：使用 PowerShell + OpenSSL**

```powershell
# 安装 OpenSSL for Windows 后
openssl genrsa -out deploy\keys\signing.pem 2048
openssl rsa -in deploy\keys\signing.pem -pubout -out deploy\keys\signing.pub

# 验证
dir deploy\keys
```

**方法 C：跳过密钥生成（仅用于测试）**

如果只是验证部署流程，可以暂时跳过此步，后端会使用内置测试密钥（⚠ 生产环境必须生成真实密钥）。

### 3. 配置环境变量

```powershell
# 复制环境变量模板
Copy deploy\env\docker.env.example .env.docker

# 编辑文件（使用 VS Code 或记事本）
notepad .env.docker
```

**编辑 `.env.docker`，设置本地测试密码**：

```env
# PostgreSQL
POSTGRES_USER=fulla_user
POSTGRES_PASSWORD=WinDockerTest2024!
POSTGRES_DB=fulla_db

# Redis
REDIS_PASSWORD=WinDockerTest2024!

# OAuth2 Backend
FULLA_DB_HOST=fulla-postgres
FULLA_DB_NAME=fulla_db
FULLA_DB_PASSWORD=WinDockerTest2024!
FULLA_REDIS_HOST=fulla-redis
FULLA_REDIS_PASSWORD=WinDockerTest2024!
FULLA_FRONTEND_URL=http://localhost:8080

# 域名（本地测试忽略）
DOMAIN=localhost
```

> **注意**：Windows 环境变量文件使用 CRLF 换行符，Docker Compose 会自动处理。

#### 邮件服务配置（可选）

后端邮件服务有两种模式（由 [EmailService.cc](https://github.com/voidvec/fulla/blob/master/libs/drogon/src/utils/EmailService.cc) 的 `getEmailService()` 决定）：

| 模式 | 触发条件 | 行为 |
|------|---------|------|
| **Console 模式**（默认） | 未设置 `FULLA_SMTP_*` | 验证邮件只输出到后端日志，不真正发送 |
| **SMTP 模式** | 设置 `FULLA_SMTP_HOST` + `FULLA_SMTP_USER` + `FULLA_SMTP_PASSWORD` | 通过 SMTP 真正发送邮件 |

**默认 Console 模式（本地验证推荐）**：

无需任何配置。点击"发送邮箱验证"后，邮件内容（含验证链接）会打到后端日志，可从中复制链接验证：

```bash
docker logs fulla-backend --tail 50 2>&1 | grep -A 5 -iE "verify|email"
```

**启用真实 SMTP 发送（163 邮箱示例）**：

在 `.env.docker` 末尾追加：

```env
# Email / SMTP
FULLA_SMTP_HOST=smtp.163.com
FULLA_SMTP_PORT=465
FULLA_SMTP_USER=your-email@163.com
FULLA_SMTP_PASSWORD=your-authorization-code   # 163 授权码，非登录密码
FULLA_SMTP_FROM_NAME=OAuth2 Platform
FULLA_SMTP_SSL=true                            # 465 端口必须 SSL
```

> **获取 163 授权码**：登录 163 邮箱网页版 → 设置 → POP3/SMTP/IMAP → 开启 SMTP 服务 → 生成授权码。

修改后重启后端使配置生效：

```bash
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker up -d fulla-backend

# 验证已切换到 SMTP 模式（应看到 "Email service: SMTP (smtp.163.com:465)"）
docker logs fulla-backend 2>&1 | grep -i "Email service"
```

> **注意**：邮件里的验证链接使用 `FULLA_FRONTEND_URL`（本地为 `http://localhost:8080`），从其他机器点开会失效——这是本地部署的预期限制。

### 4. 修改 Docker Compose 配置

由于本地环境不需要 HTTPS，我们创建一个简化的 Compose 文件：

**选项 A：使用现有的开发配置（推荐）**

```powershell
# 直接使用 docker-compose.yml（已配置好本地端口）
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker up -d --build
```

**选项 B：创建自定义配置**

如果需要更多控制，创建 `deploy/docker/docker-compose.windows.yml`：

```yaml
# 基于 docker-compose.yml，移除外部认证和 TLS 相关配置
services:
  fulla-backend:
    environment:
      - FULLA_DB_HOST=fulla-postgres
      - FULLA_DB_NAME=fulla_db
      - FULLA_DB_PASSWORD=${POSTGRES_PASSWORD}
      - FULLA_REDIS_HOST=fulla-redis
      - FULLA_REDIS_PASSWORD=${REDIS_PASSWORD}
      - FULLA_AUTO_MIGRATE=true
      - FULLA_FRONTEND_URL=http://localhost:8080
    volumes:
      - ../../deploy/keys:/app/keys:ro  # JWT 密钥
      - ../../apps/server/migrations:/app/sql/migrations:ro
      - ../../apps/server/seed:/app/sql/seed:ro

  # 其他服务保持不变...
```

### 5. 启动服务

```powershell
# 启动所有服务
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker up -d --build

# 查看启动日志
docker compose -f deploy/docker/docker-compose.yml logs -f
```

预期输出（服务启动成功）：
```
[+] Running 8/8
 [+] Network oauth2-net          Created                 0.1s
 [+] Volume "oauth2_plugin_postgres_prod" Created
 [+] Container fulla-postgres    Started                 2.3s
 [+] Container fulla-redis      Started                 1.8s
 [+] Container fulla-backend    Started                 5.2s
 [+] Container fulla-frontend    Started                 3.1s
 [+] Container fulla-admin      Started                 2.9s
 [+] Container fulla-prometheus Started                 1.5s
```

---

## 验证部署

### 1. 检查容器状态

```powershell
docker compose -f deploy/docker/docker-compose.yml ps
```

预期所有容器状态为 `Up`：

```
NAME                STATUS          PORTS
fulla-admin        Up              0.0.0.0:8081->80/tcp
fulla-backend      Up              0.0.0.0:5555->5555/tcp
fulla-frontend     Up              0.0.0.0:8080->80/tcp
fulla-postgres     Up              0.0.0.0:5433->5432/tcp
fulla-prometheus   Up              0.0.0.0:9090->9090/tcp
fulla-redis        Up              0.0.0.0:6380->6379/tcp
```

### 2. 验证后端健康

```powershell
curl http://localhost:5555/health
```

预期返回：
```json
{"status":"healthy","timestamp":"2024-06-23T10:30:00Z"}
```

### 3. 验证数据库迁移

```powershell
# 进入 postgres 容器
docker exec -it fulla-postgres psql -U fulla_user -d fulla_db -c "\dt"

# 预期看到 OAuth2 相关表
# clients, users, tokens, authorization_codes, etc.
```

### 4. 验证前端访问

在浏览器中打开：
- **用户前端**：http://localhost:8080
- **管理后台**：http://localhost:8081/admin/
- **Prometheus**：http://localhost:9090

### 5. 创建管理员账号

```Git Bash
# 执行 seed 脚本
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_admin_user.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_admin_console_client.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_vue_client.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_backend_client.sql
```

```powershell
# 执行管理员用户 seed
Get-Content apps\server\seed\dev_admin_user.sql | docker exec -i fulla-postgres psql -U fulla_user -d fulla_db

# 执行管理后台客户端 seed
Get-Content apps\server\seed\dev_admin_console_client.sql | docker exec -i fulla-postgres psql -U fulla_user -d fulla_db

# 执行 Vue 客户端 seed
Get-Content apps\server\seed\dev_vue_client.sql | docker exec -i fulla-postgres psql -U fulla_user -d fulla_db

# 执行 backend-svc 客户端 seed
Get-Content apps\server\seed\dev_backend_client.sql | docker exec -i fulla-postgres psql -U fulla_user -d fulla_db
```

验证管理员账号：

**方法 1：使用 Git Bash（推荐）**
```bash
docker exec -it fulla-postgres psql -U fulla_user -d fulla_db -c "SELECT username, email FROM users WHERE username = 'admin';"
```

**方法 2：使用 PowerShell**
```powershell
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "SELECT username, email FROM users WHERE username = 'admin';"
```

**预期输出**：
```
 username |       email       
----------+-------------------
 admin    | admin@example.com
```

**验证管理员角色**：
```bash
docker exec -it fulla-postgres psql -U fulla_user -d fulla_db -c "SELECT u.username, u.email, r.name FROM users u LEFT JOIN user_roles ur ON u.id = ur.user_id LEFT JOIN roles r ON ur.role_id = r.id WHERE u.username = 'admin';"
```

**预期输出**：
```
 username |       email       | name  
----------+-------------------+-------
 admin    | admin@example.com | admin
```

---

## 执行端点测试

项目包含完整的端点测试套件，用于验证 OAuth2 核心功能和管理后台 API。

### 推荐方式：Git Bash

**优势**：原生支持 shell 脚本、路径处理正确、与 Linux 环境一致

#### 1. 执行 OAuth2 核心端点测试

```bash
# 进入项目目录
cd /path/to/repo-root

# 确保测试脚本有执行权限
chmod +x scripts/backend/test-oauth2-endpoints.sh

# 执行测试（55个测试）
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
```

**测试覆盖**：
- 健康检查、JWKS 端点
- OAuth2 登录、令牌交换、刷新令牌
- 令牌内省、撤销
- 用户注册、登录、个人资料
- 密码重置、修改
- MFA 设置、验证、禁用
- 动态客户端注册（RFC 7591）
- WebAuthn 认证
- 设备授权流程
- 外部认证（GitHub、Google、微信）

#### 2. 执行管理后台 API 测试

```bash
chmod +x scripts/backend/test-admin-endpoints.sh
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

**测试覆盖**：
- 管理员登录、仪表板统计
- 用户管理（CRUD 操作）
- 客户端应用管理
- Scope 管理
- Token 管理
- 授权用户管理
- 角色权限管理

### 备选方式：WSL2 Ubuntu

```bash
# 1. 启动 WSL2
wsl

# 2. 进入项目目录（注意路径转换）
cd /path/to/repo-root

# 3. 执行测试
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

### PowerShell 混合方式

```powershell
# 在 PowerShell 中调用 Git Bash 执行测试
bash ./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
bash ./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

### 测试结果解读

#### 预期输出

```bash
========================================
OAuth2 Endpoints Tests (59 tests)
========================================
Base URL: http://localhost:5555

[Test 1/59] Test 1: Health Check
    Status: ok
    [+] PASS (0.1s)

[Test 10/59] Test 10: Client Credentials
    AT: eyJhbGciOiJSUzI1Ni..., Scope: read
    [+] PASS (0.2s)

...

========================================
Test Results: 59/59 passed, 0 failed
========================================
```

#### 失败排查

**端点测试应全部通过（当前口径：OAuth2 侧 59、Admin 侧 52）**。若出现失败，按以下已知环境依赖排查：

1. **Test 10 失败**：`no access_token`
   - **原因**：缺少测试客户端 `backend-svc`
   - **解决**：执行 `dev_backend_client.sql` 创建测试客户端

2. **Test 20/20b 失败**：`missing field: .client_id` 或 `Expected HTTP 400, got 403`
   - **原因**：RBAC 权限控制正确工作，动态客户端注册需要特殊配置
   - **影响**：无（这是预期行为）

3. **连锁失败**：`skipped: no token`
   - **原因**：前序测试撤销令牌导致后续测试无令牌可用
   - **影响**：无（测试脚本设计如此）

#### 成功标准

**59/52 全部通过才算部署验证成功**。个别失败若出现，先核对是否为上述已知的脚本环境依赖（数据库重置、种子数据、端口占用）；不要把『部分通过』当作部署成功标准。

### 测试前准备

#### 1. 确保数据库种子数据

```bash
# 执行必需的 seed 脚本
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_admin_user.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_admin_console_client.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_vue_client.sql

# 可选：创建测试客户端（提高测试通过率）
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_backend_client.sql
```

#### 2. 验证基础服务

```bash
# 检查容器状态
docker ps

# 检查后端健康
curl http://localhost:5555/health

# 检查数据库连接
docker exec fulla-postgres pg_isready -U fulla_user
```

### 快速验证命令

```bash
# 一键执行所有测试
cd /path/to/repo-root && \
chmod +x scripts/backend/*.sh && \
echo "[+] 执行 OAuth2 核心测试..." && \
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555 && \
echo "" && \
echo "[+] 执行管理后台 API 测试..." && \
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

---

## 功能测试清单

### 核心 OAuth2 流程

| 测试项 | 测试方法 | 预期结果 |
|--------|---------|---------|
| 用户注册 | 前端注册页面 | 注册成功，可登录 |
| 用户登录 | POST /oauth2/login（授权码 + PKCE 流程第一步） | 返回授权码 code |
| 刷新令牌 | POST /oauth2/token (refresh_token grant) | 返回新的 access_token |
| 令牌校验 | POST /oauth2/introspect | 返回 token 有效信息 |
| 令牌撤销 | POST /oauth2/revoke | 返回 200 OK |
| 授权码流程 | /oauth2/authorize → /callback | 完整 OAuth2 流程 |
| 客户端管理 | Admin Console 创建/删除客户端 | 操作成功 |

### API 端点测试

#### 方法 1：使用现有测试脚本（推荐）

项目包含完整的端点测试脚本，推荐使用 Git Bash 执行：

**执行 OAuth2 核心端点测试（59 个测试）**：
```bash
# 1. 进入项目目录（Git Bash）
cd /path/to/repo-root

# 2. 确保测试脚本有执行权限
chmod +x scripts/backend/test-oauth2-endpoints.sh

# 3. 执行测试
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
```

**执行管理后台 API 测试（52 个测试）**：
```bash
chmod +x scripts/backend/test-admin-endpoints.sh
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

**预期输出示例**：
```bash
========================================
OAuth2 Endpoints Tests (59 tests)
========================================
Base URL: http://localhost:5555

[Test 1/59] Test 1: Health Check
    Status: ok
    [+] PASS (0.1s)

...

========================================
Test Results: 59/59 passed, 0 failed
========================================
```

**成功标准**：全部通过（59/52）。个别失败先按上文「失败排查」核对环境依赖；不要把『部分通过』当作部署成功标准。

#### 方法 2：使用 PowerShell 脚本测试

`fulla-admin-console` 是 PUBLIC 客户端（PKCE 强制、无 secret、无 password grant），手工构造令牌流程较繁琐，推荐直接使用仓库自带的 PowerShell 测试脚本（内部已实现 PKCE 登录）：

```powershell
# 执行管理后台端点测试（含 PKCE 登录 + 52 项断言）
.\scripts\backend\test-admin-endpoints.ps1 -BaseUrl "http://localhost:5555"

# 若已从其他途径拿到 access_token，可直接手工调用受保护 API 验证：
$headers = @{ Authorization = "Bearer <access_token>" }
Invoke-RestMethod -Uri "http://localhost:5555/api/admin/users" -Headers $headers
# 预期：用户列表 JSON
```

#### 方法 3：使用 WSL2 Ubuntu（推荐）

```bash
# 1. 启动 WSL2
wsl

# 2. 进入项目目录
cd /path/to/repo-root

# 3. 执行测试
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

### 前端路由测试

| 路径 | 预期页面 |
|------|---------|
| http://localhost:8080/ | 用户登录页 |
| http://localhost:8080/register | 用户注册页 |
| http://localhost:8080/profile | 个人资料页（需登录） |
| http://localhost:8080/callback | OAuth2 回调页 |
| http://localhost:8081/admin/ | 管理后台（需登录） |
| http://localhost:8081/admin/apps | 应用管理页 |

---

## 常见问题排查

### Docker Desktop 启动失败

**症状**：`docker ps` 报错 "Cannot connect to the Docker daemon"

**解决**：
1. 检查 Docker Desktop 是否正在运行（系统托盘图标）
2. 重启 Docker Desktop
3. 检查 Hyper-V 或 WSL2 是否启用：
   ```powershell
   # WSL2
   wsl --list --verbose
   
   # Hyper-V
   dism /Online /Get-FeatureInformation /FeatureName:Microsoft-Hyper-V
   ```

### 端口冲突

**症状**：容器启动失败，日志显示 "port is already allocated"

**检查占用端口的进程**：
```powershell
# 检查 8080 端口
netstat -ano | findstr :8080

# 检查 5433 端口
netstat -ano | findstr :5433
```

**解决**：
1. 停止冲突的服务
2. 或修改 `docker-compose.yml` 中的端口映射（如改为 `8082:80`）

### 数据库连接失败

**症状**：后端日志显示 "Connection refused" 或 "Host unreachable"

**排查**：
```powershell
# 1. 检查 postgres 容器状态
docker ps | findstr fulla-postgres

# 2. 检查 postgres 日志
docker logs fulla-postgres

# 3. 测试数据库连接
docker exec fulla-postgres pg_isready -U fulla_user

# 4. 从后端容器测试网络连通性
docker exec fulla-backend curl -s http://fulla-postgres:5432 2>&1
docker exec fulla-backend curl -s http://fulla-redis:6379 2>&1
```

### 构建失败

**症状**：`docker compose build` 报错 "failed to solve"

**解决**：
```powershell
# 清理构建缓存
docker builder prune -a

# 重新构建
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker build --no-cache

# 如果仍失败，检查磁盘空间
docker system df
```

### Windows 路径问题

**症状**：卷挂载失败，错误 "invalid mount config"

**原因**：Windows 路径转换问题（`C:\` → `/c/`）

**解决**：
1. 使用 Git Bash 执行 Docker 命令（自动转换路径）
2. 或使用 WSL2 执行：
   ```powershell
   wsl docker compose -f deploy/docker/docker-compose.yml up -d
   ```

---

## 与 Linux 生产部署的映射关系

### 配置文件映射

| Windows 本地 | Linux 生产 | 说明 |
|-------------|-----------|------|
| `.env.docker` | `/root/fulla/.env.docker` | 环境变量完全相同 |
| `deploy/keys/signing.pem` | `/root/fulla/deploy/keys/signing.pem` | JWT 密钥 |
| `docker-compose.yml` | `docker-compose.prod.yml` | 端口和 TLS 配置差异 |

### 部署命令映射

| 操作 | Windows Docker Desktop | Linux 生产 |
|------|----------------------|-----------|
| 启动 | `docker compose --env-file .env.docker up -d` | `docker compose --env-file .env.docker -f docker-compose.prod.yml up -d` |
| 查看日志 | `docker compose logs -f` | `docker compose -f docker-compose.prod.yml logs -f` |
| 重建 | `docker compose up -d --build` | `docker compose -f docker-compose.prod.yml up -d --build` |
| 停止 | `docker compose down` | `docker compose -f docker-compose.prod.yml down` |

### 访问地址映射

| 服务 | Windows 本地 | Linux 生产 |
|------|-------------|-----------|
| 用户前端 | http://localhost:8080 | https://your-domain.com/ |
| 管理后台 | http://localhost:8081 | https://your-domain.com/admin/ |
| 后端 API | http://localhost:5555 | https://your-domain.com/api/ |
| Prometheus | http://localhost:9090 | http://your-server:9090 |

---

## 从本地验证到生产部署

### 验证通过后部署到 Linux

1. **确保本地验证成功**：
   ```powershell
   # 运行完整测试
   .\scripts\backend\test-oauth2-endpoints.ps1
   .\scripts\backend\test-admin-endpoints.ps1
   ```

2. **提交代码**：
   ```powershell
   git add .
   git commit -m "feat: XXX (tested on Windows Docker Desktop)"
   git push
   ```

3. **在 Linux 服务器上部署**：
   ```bash
   # 拉取代码
   git pull

   # 复制环境变量（只复制一次）
   cp deploy/env/docker.env.example .env.docker
   # 编辑 .env.docker 设置生产密码

   # 使用生产配置启动
   docker compose -f deploy/docker/docker-compose.prod.yml --env-file .env.docker up -d --build
   ```

4. **配置 TLS**（Linux 唯一额外步骤）：
   ```bash
   # 使用 Let's Encrypt
   sudo certbot certonly --standalone -d your-domain.com
   cp /etc/letsencrypt/live/your-domain.com/fullchain.pem deploy/nginx/ssl/
   cp /etc/letsencrypt/live/your-domain.com/privkey.pem deploy/nginx/ssl/
   docker compose -f deploy/docker/docker-compose.prod.yml restart nginx
   ```

---

## 性能对比

| 指标 | Windows Docker Desktop | Linux 生产服务器 |
|------|----------------------|----------------|
| 启动时间 | ~45 秒（6 个容器） | ~30 秒（相同配置） |
| 内存占用 | ~2.5 GB | ~1.8 GB |
| API 响应时间 | ~50ms | ~40ms |
| 数据库查询 | ~10ms | ~8ms |

> 差异主要来自 Windows 系统开销和 WSL2 虚拟化层，但功能完全一致。

---

## 下一步

1. **完成本地验证**：确保所有核心功能正常
2. **记录测试结果**：在项目文档中标记"Windows Docker Desktop 已验证"
3. **推送到 Linux**：一次性部署成功
4. **配置监控**：设置 Prometheus + Grafana

---

## 附录：完整端口映射

```
┌─────────────────────────────────────────────────────────────┐
│                    Windows 宿主机                             │
├─────────────────────────────────────────────────────────────┤
│  Port 8080 ──→ fulla-frontend:80 (Vue 用户前端)              │
│  Port 8081 ──→ fulla-admin:80 (Vue 管理后台)                 │
│  Port 5555 ──→ fulla-backend:5555 (C++ API)                │
│  Port 5433 ──→ fulla-postgres:5432 (PostgreSQL)            │
│  Port 6380 ──→ fulla-redis:6379 (Redis)                    │
│  Port 9090 ──→ fulla-prometheus:9090 (监控)                │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│                  Docker 内部网络 (oauth2-net)                 │
│                                                               │
│  所有容器通过内部 DNS 互相访问：                              │
│  - fulla-backend → fulla-postgres:5432                     │
│  - fulla-backend → fulla-redis:6379                        │
│  - fulla-frontend → fulla-backend:5555                     │
│  - fulla-admin → fulla-backend:5555                        │
└─────────────────────────────────────────────────────────────┘
```

---

## 总结

✓ **可行**：Windows Docker Desktop 可以完全验证部署流程（除域名和 SSL）  
✓ **推荐**：本地验证 → 推送代码 → Linux 部署，大幅减少调试时间  
✓ **一致性**：数据库模式、API 接口、前端逻辑与生产环境 100% 一致  

**适用场景**：
- ✓ 验证代码更改
- ✓ 测试数据库迁移
- ✓ 调试 API 端点
- ✓ 验证前端路由
- ✓ 测试 OAuth2 流程

**不适用场景**：
- ✗ TLS/SSL 测试（使用自签名证书可部分替代）
- ✗ 性能压测（使用 Linux 服务器）
- ✗ 高可用配置（需要多台服务器）
