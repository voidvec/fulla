# 生产化部署指南

本指南说明如何将 OAuth2 全栈系统（用户前端 + 管理后台 + 后端 API）部署到生产环境。

---

## 架构概览

```
                    Internet
                       │
                ┌──────┴──────┐
                │   Nginx     │  :80 → :443 (TLS)
                │  (反向代理)  │
                └──────┬──────┘
          ┌────────────┼────────────┐
          │            │            │
    ┌─────┴─────┐ ┌────┴────┐ ┌────┴────┐
    │ Frontend  │ │  Admin  │ │ Backend │
    │ (Vue SPA) │ │ (Vue)   │ │ (C++)   │
    │  :80      │ │  :80    │ │  :5555  │
    └───────────┘ └─────────┘ └────┬────┘
                                   │
                         ┌─────────┼─────────┐
                         │                   │
                   ┌─────┴─────┐     ┌───────┴───────┐
                   │ PostgreSQL│     │     Redis     │
                   │   :5432   │     │    :6379      │
                   └───────────┘     └───────────────┘
```

**路由规则（Nginx）**：
- `/api/*`, `/oauth2/*`, `/.well-known/*`, `/health` → Backend
- `/admin/*` → Admin Console
- `/*` (其他) → User Frontend

---

## 前置条件

### 硬件要求
- **CPU**: 2 核心以上
- **内存**: 4GB 以上（推荐 8GB）
- **磁盘**: 20GB 以上可用空间
- **网络**: 公网 IP，域名已解析到服务器

### 操作系统支持

- Ubuntu 20.04 / 22.04 / 24.04 LTS
- Debian 11 / 12
- CentOS Stream 8 / 9
- Rocky Linux 8 / 9

### 软件依赖安装

#### 1. 安装 Docker

**Ubuntu/Debian**:
```bash
# 更新包索引
sudo apt update

# 安装必要依赖
sudo apt install -y ca-certificates curl gnupg lsb-release

# 添加 Docker 官方 GPG 密钥
sudo mkdir -p /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg

# 设置 Docker 仓库
echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu \
  $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

# 安装 Docker Engine
sudo apt update
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# 启动 Docker 服务
sudo systemctl start docker
sudo systemctl enable docker

# 验证安装
docker --version
docker compose version
```

**CentOS/Rocky Linux**:
```bash
# 安装必要依赖
sudo yum install -y yum-utils device-mapper-persistent-data lvm2

# 添加 Docker 仓库
sudo yum-config-manager --add-repo https://download.docker.com/linux/centos/docker-ce.repo

# 安装 Docker
sudo yum install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# 启动 Docker 服务
sudo systemctl start docker
sudo systemctl enable docker

# 验证安装
docker --version
docker compose version
```

#### 2. 配置 Docker 用户组（可选但推荐）

```bash
# 创建 docker 组（如果不存在）
sudo groupadd docker

# 将当前用户添加到 docker 组
sudo usermod -aG docker $USER

# 重新登录或运行以下命令使组权限生效
newgrp docker

# 验证：无需 sudo 运行 docker
docker ps
```

#### 2.5. 配置 Docker 镜像加速器（中国大陆必需）

由于 Docker Hub 在中国大陆访问不稳定，拉取镜像会超时（典型错误：`dial tcp registry-1.docker.io:443: i/o timeout`），必须配置镜像加速器。

以下加速器地址经实测（2026-06）在阿里云服务器上验证可用：

**创建或修改 Docker 配置文件**:

```bash
sudo mkdir -p /etc/docker
sudo tee /etc/docker/daemon.json > /dev/null << 'EOF'
{
  "registry-mirrors": [
    "https://docker.1panel.live",
    "https://docker.awsl9527.cn",
    "https://docker.xuanyuan.me"
  ],
  "log-driver": "json-file",
  "log-opts": {
    "max-size": "100m",
    "max-file": "3"
  }
}
EOF
```

> 说明：配置多个加速器，Docker 会按顺序尝试，任一可用即拉取成功。

**重启 Docker 服务使配置生效**:

```bash
sudo systemctl daemon-reload
sudo systemctl restart docker
sudo systemctl status docker
```

**验证镜像加速器配置**:

```bash
# 检查配置是否被加载（应显示上述 registry-mirrors 列表）
docker info | grep -A 5 "Registry Mirrors"

# 测试拉取镜像（本项目需要的全部镜像）
docker pull postgres:17-alpine
docker pull redis:7-alpine
docker pull nginx:stable-alpine
docker pull prom/prometheus:latest
docker pull ubuntu:24.04
```

如果某个加速器报错（如 `502` 或 `i/o timeout`），Docker 会自动尝试下一个；若全部失败，参考下方故障排除。

**故障排除**:

1. **所有加速器均失败**：访问 [dongyubin/DockerHub](https://github.com/dongyubin/DockerHub) 获取最新可用列表，替换 `daemon.json` 中的地址后重启 Docker。

2. **使用阿里云专属加速器**（需要阿里云账号，最稳定）:
   - 登录 [阿里云容器镜像服务](https://cr.console.aliyun.com/) → 镜像工具 → 镜像加速器
   - 获取专属加速地址（形如 `https://<your_code>.mirror.aliyuncs.com`）
   - 将该地址置于 `daemon.json` 的 `registry-mirrors` 数组首位

3. **使用代理拉取**（如果有可用的代理服务器）:

   ```bash
   # 为 Docker 守护进程配置代理
   sudo mkdir -p /etc/systemd/system/docker.service.d
   sudo tee /etc/systemd/system/docker.service.d/http-proxy.conf > /dev/null << EOF
   [Service]
   Environment="HTTP_PROXY=http://your-proxy:port"
   Environment="HTTPS_PROXY=http://your-proxy:port"
   Environment="NO_PROXY=localhost,127.0.0.1"
   EOF

   sudo systemctl daemon-reload
   sudo systemctl restart docker
   ```

#### 3. 安装 Git

**Ubuntu/Debian**:
```bash
sudo apt install -y git
```

**CentOS/Rocky Linux**:
```bash
sudo yum install -y git
```

#### 4. 安装 OpenSSL（用于生成密钥）

**Ubuntu/Debian**:
```bash
sudo apt install -y openssl
```

**CentOS/Rocky Linux**:
```bash
sudo yum install -y openssl
```

#### 5. 安装 Certbot（用于获取 Let's Encrypt 证书）

**Ubuntu/Debian**:
```bash
sudo apt install -y certbot
```

**CentOS/Rocky Linux**:
```bash
sudo yum install -y certbot
```

### 验证依赖安装

```bash
# 检查 Docker 版本（要求 24+）
docker --version

# 检查 Docker Compose 版本（要求 v2）
docker compose version

# 检查 Git
git --version

# 检查 OpenSSL
openssl version

# 检查 Certbot
certbot --version
```

### 域名和 DNS 配置

1. **域名解析**：确保您的域名（如 `your-domain.example.com`）的 A 记录指向服务器公网 IP
2. **DNS 传播验证**：
   ```bash
   # 检查域名是否正确解析
   dig +short your-domain.example.com
   nslookup your-domain.example.com
   ```
3. **防火墙配置**：确保以下端口可访问：
   - `80/tcp` (HTTP)
   - `443/tcp` (HTTPS)

### 防火墙配置

**Ubuntu (UFW)**:
```bash
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw enable
```

**CentOS/Rocky Linux (firewalld)**:
```bash
sudo firewall-cmd --permanent --add-service=http
sudo firewall-cmd --permanent --add-service=https
sudo firewall-cmd --reload
```

---

## 快速部署（5 步）

### 1. 克隆项目

```bash
git clone <repo-url>
cd fulla
```

### 2. 生成密钥

```bash
# 生成 JWT 签名密钥
chmod +x scripts/generate-jwt-keys.sh
./scripts/generate-jwt-keys.sh

# 生成临时自签名 TLS 证书（nginx 启动需要证书文件存在）
chmod +x scripts/generate-certs.sh
./scripts/generate-certs.sh
```

> 自签名证书是启动引导用的临时占位——nginx 要求证书文件存在才能启动。后续第 6 步会替换为 Let's Encrypt 正式证书。

**生产环境使用 Let's Encrypt**（在第 4 步启动服务之后）：
```bash
# 1. 停止 nginx 容器以释放 80 端口
docker compose -f deploy/docker/docker-compose.prod.yml stop nginx

# 2. 获取证书（standalone 模式需要 80 端口空闲）
sudo certbot certonly --standalone -d your-domain.com

# 3. 复制证书
cp /etc/letsencrypt/live/your-domain.com/fullchain.pem deploy/nginx/ssl/
cp /etc/letsencrypt/live/your-domain.com/privkey.pem deploy/nginx/ssl/

# 4. 重启 nginx 加载正式证书
docker compose -f deploy/docker/docker-compose.prod.yml start nginx
```

### 3. 配置环境变量

```bash
# 检查模板文件是否存在
[ -f deploy/env/docker.env.example ] && echo "模板文件存在" || echo "错误：模板文件不存在"

cp deploy/env/docker.env.example .env.docker
```

编辑 `.env.docker`，设置强密码与 HTTPS 域名相关配置：

```env
# 镜像版本 tag（ghcr.io/voidvec/fulla-*，默认 latest）
FULLA_VERSION=latest

# 运行模式（生产强制校验 HTTPS issuer / 强密码；须配合 FULLA_ISSUER=https://）
FULLA_ENV=production
FULLA_ISSUER=https://your-domain.com

# JWT 签名密钥（生产必填；不设则每次重启 token 失效）
FULLA_JWT_KEY_PATH=/app/keys/signing.pem

# ⚠ POSTGRES_PASSWORD 与 FULLA_DB_PASSWORD 必须保持一致
POSTGRES_USER=fulla_user
POSTGRES_PASSWORD=<生成强密码>
POSTGRES_DB=fulla_db
FULLA_DB_HOST=fulla-postgres
FULLA_DB_PORT=5432
FULLA_DB_NAME=fulla_db
FULLA_DB_USER=fulla_user
FULLA_DB_PASSWORD=<与 POSTGRES_PASSWORD 相同>

# ⚠ REDIS_PASSWORD 与 FULLA_REDIS_PASSWORD 必须保持一致
REDIS_PASSWORD=<生成强密码>
FULLA_REDIS_HOST=fulla-redis
FULLA_REDIS_PORT=6379
FULLA_REDIS_PASSWORD=<与 REDIS_PASSWORD 相同>

# CORS / OAuth 回调（HTTPS 域名必填，否则浏览器请求被拦截）
FULLA_FRONTEND_URL=https://your-domain.com
FULLA_CORS_ALLOW_ORIGINS=https://your-domain.com
# ⚠ FULLA_VUE_REDIRECT_URI 与 VITE_REDIRECT_URI 必须保持一致
FULLA_VUE_REDIRECT_URI=https://your-domain.com/callback
FULLA_VUE_CLIENT_SECRET=<生成强密码>
FULLA_GOOGLE_REDIRECT_URI=https://your-domain.com/callback

# 错误详细度（生产建议 false，不暴露字段级校验错误）
DETAILED_VALIDATION_ERRORS=false

# 第三方登录（可选）
FULLA_GITHUB_CLIENT_ID=
FULLA_GITHUB_CLIENT_SECRET=
FULLA_GOOGLE_CLIENT_ID=
FULLA_GOOGLE_CLIENT_SECRET=
FULLA_WECHAT_APPID=
FULLA_WECHAT_SECRET=

# 邮件服务（SMTP）— 生产环境必须配置
FULLA_SMTP_HOST=smtp.example.com
FULLA_SMTP_PORT=465
FULLA_SMTP_USER=noreply@example.com
FULLA_SMTP_PASSWORD=<SMTP 授权码，非邮箱登录密码>
FULLA_SMTP_FROM_NAME=OAuth2 Platform
FULLA_SMTP_SSL=true

# 前端构建变量（Vite 构建期注入）
# VITE_API_BASE_URL 生产必须留空 → SPA 走相对路径（nginx 同源反代）
VITE_API_BASE_URL=
VITE_CLIENT_ID=fulla-portal
VITE_REDIRECT_URI=https://your-domain.com/callback
VITE_GITHUB_CLIENT_ID=
```

> **重要耦合**：`FULLA_ENV=production` 与 `FULLA_ISSUER=https://...` 必须同时设置。仅设 production 而不配 HTTPS issuer 会导致后端启动校验失败（`ConfigManager` 的 prod-mode 校验拒绝非 https issuer）。同理 DB/Redis 密码不能是默认的 `123456` / `password`，否则 prod 校验也会拒绝启动。

生成强密码：
```bash
openssl rand -base64 32
```

#### 邮件服务（SMTP）配置说明

后端邮件服务有两种模式（由 `getEmailService()` 根据环境变量自动选择）：

| 模式 | 触发条件 | 行为 |
|------|---------|------|
| **Console 模式** | 未设置 `FULLA_SMTP_HOST` / `USER` / `PASSWORD` | 邮件内容只输出到后端日志，**不真正发送** |
| **SMTP 模式** | 上述三个变量均已设置且非空 | 通过 SMTP 真正发送邮件 |

> **生产环境必须配置 SMTP**，否则邮箱验证、密码重置等功能的邮件不会真正发送给用户（只在服务器日志里）。

**常见邮箱服务商配置参考**：

| 服务商 | SMTP 主机 | 端口 | SSL | 凭据说明 |
|--------|----------|------|-----|---------|
| 163 邮箱 | `smtp.163.com` | 465 | true | 授权码（非登录密码） |
| QQ 邮箱 | `smtp.qq.com` | 465 | true | 授权码 |
| Gmail | `smtp.gmail.com` | 465 | true | 应用专用密码（需开两步验证） |
| 腾讯企业邮 | `smtp.exmail.qq.com` | 465 | true | 邮箱密码 |
| 阿里云企业邮 | `smtp.qiye.aliyun.com` | 465 | true | 邮箱密码 |
| SendGrid | `smtp.sendgrid.net` | 587 | false | 用户名 `apikey`，密码为 API Key |

**获取授权码（以 163 为例）**：
1. 登录 163 邮箱网页版
2. 设置 → POP3/SMTP/IMAP
3. 开启 SMTP 服务
4. 按提示生成授权码（16 位字符串）

配置完成后重启后端生效：

```bash
docker compose -f deploy/docker/docker-compose.prod.yml --env-file .env.docker up -d fulla-backend

# 验证已切换到 SMTP 模式（应输出 "Email service: SMTP (...)"）
docker compose -f deploy/docker/docker-compose.prod.yml logs fulla-backend | grep "Email service"
```

### 4. 启动服务

```bash
# --build 从源码编译镜像（从克隆仓库首次部署时必填）
docker compose -f deploy/docker/docker-compose.prod.yml --env-file .env.docker up -d --build
```

> 后续仅修改配置（无代码变更）的重启可省略 `--build`。`git pull` 更新代码后务必加上 `--build` 以拉取变更。

### 5. 验证部署

```bash
# 检查所有容器状态
docker compose -f deploy/docker/docker-compose.prod.yml ps

# 检查后端健康
curl -k https://localhost/health

# 检查前端
curl -k https://localhost/

# 检查管理后台
curl -k https://localhost/admin/
```

---

## 服务详情

### 用户前端 (OAuth2Frontend)

| 项目 | 值 |
|------|-----|
| 容器名 | fulla-frontend |
| 构建 | Dockerfile (target: frontend-runtime) |
| 基础镜像 | nginx:stable-alpine |
| 内部端口 | 80 |
| 访问路径 | `https://your-domain.com/` |
| 功能 | 登录、注册、个人资料、安全设置、OAuth2 授权 |

### 管理后台 (OAuth2Admin)

| 项目 | 值 |
|------|-----|
| 容器名 | fulla-admin |
| 构建 | frontends/admin/Dockerfile |
| 基础镜像 | nginx:alpine |
| 内部端口 | 80 |
| 访问路径 | `https://your-domain.com/admin/` |
| 功能 | 应用管理、用户管理、角色/Scope/Token 管理 |

### 后端 API (fulla-server)

| 项目 | 值 |
|------|-----|
| 容器名 | fulla-backend |
| 构建 | Dockerfile (target: backend-runtime) |
| 基础镜像 | ubuntu:24.04 (minimal) |
| 内部端口 | 5555 |
| 访问路径 | `https://your-domain.com/api/*`, `/oauth2/*` |
| 数据库迁移 | 一次性 `migrate` 服务（compose profile `migrate`；`FULLA_AUTO_MIGRATE=false`） |

### 基础设施

| 服务 | 镜像 | 用途 |
|------|------|------|
| fulla-postgres | postgres:17-alpine | 主数据库 |
| fulla-redis | redis:7-alpine | Token 缓存 |
| oauth2-nginx | nginx:stable-alpine | TLS 终止 + 反向代理 |
| fulla-prometheus | prom/prometheus | 监控指标采集 |

---

## 配置说明

### 后端配置 (config.prod.json)

后端通过环境变量覆盖配置文件中的值（优先级：`.env` 文件 > 系统环境变量 > `config.prod.json` 默认值）：

| 环境变量 | 用途 | 默认值 |
|----------|------|--------|
| `FULLA_ENV` | 运行模式（`production` 启用 HTTPS issuer + 强密码严格校验） | development |
| `FULLA_ISSUER` | JWT issuer（生产必须 `https://`） | http://localhost:5555 |
| `FULLA_JWT_KEY_PATH` | JWT 签名密钥文件路径 | /app/keys/signing.pem |
| `FULLA_SIGNING_KEY` | JWT 密钥 PEM 内容（与 `JWT_KEY_PATH` 二选一） | (可选) |
| `FULLA_DB_HOST` | PostgreSQL 主机 | postgres |
| `FULLA_DB_PORT` | PostgreSQL 端口 | 5432 |
| `FULLA_DB_NAME` | 数据库名 | fulla_db_prod |
| `FULLA_DB_USER` | 数据库用户 | fulla_user |
| `FULLA_DB_PASSWORD` | 数据库密码 | (必须设置) |
| `FULLA_REDIS_HOST` | Redis 主机 | redis |
| `FULLA_REDIS_PORT` | Redis 端口 | 6379 |
| `FULLA_REDIS_PASSWORD` | Redis 密码 | (必须设置) |
| `FULLA_LISTEN_PORT` | 后端监听端口 | 5555 |
| `FULLA_FRONTEND_URL` | 前端 URL（用于重定向等） | http://localhost:5173 |
| `FULLA_CORS_ALLOW_ORIGINS` | CORS 允许的源（逗号分隔，覆盖 JSON 数组） | config 中的 localhost 列表 |
| `FULLA_PORTAL_REDIRECT_URI` | fulla-portal OAuth 回调 URI（旧名别名：`FULLA_VUE_REDIRECT_URI`） | config 中的 localhost 值 |
| `FULLA_GOOGLE_REDIRECT_URI` | Google OAuth 回调 URI | config 中的 localhost 值 |
| `FULLA_PORTAL_CLIENT_SECRET` | fulla-portal 密钥（旧名别名：`FULLA_VUE_CLIENT_SECRET`） | 123456 |
| `FULLA_ADMIN_CONSOLE_REDIRECT_URI` | fulla-admin-console OAuth 回调 URI | config 中的 localhost 值 |
| `FULLA_MFA_TOTP_ISSUER` | 认证器应用中 TOTP 条目显示的发行方名称 | `Fulla` |
| `FULLA_AUTO_MIGRATE` | 自动执行数据库迁移 | false（改用一次性 `migrate` 服务） |
| `DETAILED_VALIDATION_ERRORS` | 是否返回字段级校验错误（生产建议 false） | false |
| `FULLA_GITHUB_CLIENT_ID` / `FULLA_GITHUB_CLIENT_SECRET` | GitHub OAuth（可选） | (空) |
| `FULLA_GOOGLE_CLIENT_ID` / `FULLA_GOOGLE_CLIENT_SECRET` | Google OAuth（可选） | (空) |
| `FULLA_WECHAT_APPID` / `FULLA_WECHAT_SECRET` | 微信 OAuth（可选） | (空) |
| `FULLA_SMTP_HOST` | SMTP 服务器主機（未设置则邮件走 Console 模式） | (可选) |
| `FULLA_SMTP_PORT` | SMTP 端口 | 465 |
| `FULLA_SMTP_USER` | SMTP 用户名（完整邮箱地址） | (可选) |
| `FULLA_SMTP_PASSWORD` | SMTP 授权码（非邮箱登录密码） | (可选) |
| `FULLA_SMTP_FROM_NAME` | 发件人显示名称 | OAuth2 Platform |
| `FULLA_SMTP_SSL` | 是否启用 SSL | true |

> **邮件模式说明**：仅当 `FULLA_SMTP_HOST` + `FULLA_SMTP_USER` + `FULLA_SMTP_PASSWORD` 三项均非空时启用真实 SMTP 发送；否则邮件只输出到后端日志。详见上文"邮件服务（SMTP）配置说明"。
>
> **CORS 数组覆盖**：`FULLA_CORS_ALLOW_ORIGINS` 是逗号分隔的字符串（如 `https://a.com,https://b.com`），后端启动时自动分割成 JSON 数组覆盖 `config.prod.json` 的 `custom_config.cors.allow_origins`。CORS 校验代码要求该字段是数组，因此**必须**用逗号分隔形式，不要写成 JSON 数组字面量。

### Nginx 配置

`deploy/nginx/nginx.conf` 包含：
- HTTP → HTTPS 自动重定向
- TLS 1.2/1.3 配置
- 限流规则（登录 5次/分钟/IP，API 30次/秒/IP）
- `/metrics` 端点限制内网访问
- HSTS 头

### 前端配置

前端（用户端 OAuth2Frontend）通过 Vite 环境变量配置，**在镜像构建时注入**到 SPA bundle（不是运行时读取）。`docker-compose.prod.yml` 的 `fulla-frontend.build.args` 从 `.env.docker` 透传这些变量，`Dockerfile` 的 `frontend-builder` 阶段用 `ARG`/`ENV` 暴露给 Vite。

| 变量 | 用途 | 生产值 |
|------|------|--------|
| `VITE_API_BASE_URL` | API 基础 URL | **(空)** — SPA 同域走相对路径，填值会破坏 nginx 反代路由 |
| `VITE_CLIENT_ID` | OAuth2 Client ID | fulla-portal |
| `VITE_REDIRECT_URI` | OAuth2 回调 URI | https://your-domain.com/callback |
| `VITE_GITHUB_CLIENT_ID` | GitHub "Sign in with GitHub" 按钮（可选） | (空则不显示按钮) |

> **管理后台（OAuth2Admin）无需配置**：源码不读取任何 `import.meta.env`，所有 API 调用走相对路径 `/api/admin/*`，由 nginx 反代到后端。改域名时只需保证 nginx `/admin/` 路由正确，无需重建 admin 镜像。
>
> **改域名需重建前端镜像**：由于 VITE 变量在构建期固化，更换域名后必须 `docker compose ... up -d --build fulla-frontend`（管理后台不受影响）。

---

## 数据库初始化

首次部署时，由一次性 `migrate` 服务创建所有必要的表（compose 栈保持 `FULLA_AUTO_MIGRATE=false`；在启动 postgres/redis 服务后执行 `docker compose -f docker-compose.prod.yml --env-file <your-env> --profile migrate run --rm --build migrate`——干净环境**必须带 `--build`**，否则 compose 会拉取已发布镜像，其中可能缺少你本地的新迁移）。但**不会自动创建种子数据**——需要手动创建管理员用户和 OAuth2 客户端。

> `apps/server/seed/` 下的 `dev_*.sql` 文件使用硬编码密码和 localhost 回调地址，**不要在生产环境使用**。请按以下步骤操作。

### 1. 创建管理员账号

生成安全密码并创建 admin 用户：

```bash
# 生成 admin 随机密码
ADMIN_PASSWORD=$(openssl rand -base64 24)
echo "Admin 密码: $ADMIN_PASSWORD"
echo "请保存此密码——登录管理后台时需要。"

# 生成密码哈希（SHA-256 + 随机盐）
ADMIN_SALT=$(openssl rand -hex 16)
ADMIN_HASH=$(echo -n "${ADMIN_PASSWORD}${ADMIN_SALT}" | sha256sum | cut -d' ' -f1)

# 创建 admin 用户
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db <<EOF
INSERT INTO users (username, password_hash, salt, email)
VALUES ('admin', '${ADMIN_HASH}', '${ADMIN_SALT}', 'admin@your-domain.com')
ON CONFLICT (username) DO NOTHING;

INSERT INTO user_roles (user_id, role_id)
SELECT u.id, r.id FROM users u, roles r
WHERE u.username = 'admin' AND r.name = 'admin'
ON CONFLICT DO NOTHING;

INSERT INTO oauth2_subject_mappings (subject, internal_user_id, provider)
SELECT u.id::text, u.id, 'local'
FROM users u WHERE u.username = 'admin'
ON CONFLICT (provider, subject) DO NOTHING;
EOF
```

### 2. OAuth2 客户端（自动播种）

两个一方客户端 —— `fulla-portal`（用户前端）和 `fulla-admin-console`（管理后台）—— **在服务器启动时自动播种**（#204）：启动播种器读取 OAuth2Plugin 配置的 `clients` 块，对缺失的行做幂等插入（`ON CONFLICT DO NOTHING`，因此管理台对 client 的修改不会被重启覆盖）。

你只需要在 `.env.docker` 提供回调地址，服务器会在播种前应用到配置：

```bash
FULLA_PORTAL_REDIRECT_URI=https://your-domain.com/callback
FULLA_ADMIN_CONSOLE_REDIRECT_URI=https://your-domain.com/admin/callback
```

`fulla-portal` 的 client_id 必须与 `.env.docker` 中的 `VITE_CLIENT_ID` 一致（默认值：`fulla-portal`），且 `VITE_REDIRECT_URI` 必须等于 `FULLA_PORTAL_REDIRECT_URI`（SPA 在构建期烧录回调地址）。

无需手工执行任何 SQL —— 继续下面的启动栈步骤。首次启动预期日志：

```
ClientSeeder: seeded client 'fulla-portal'
ClientSeeder: seeded client 'fulla-admin-console'
ClientSeeder: config clients seeded/verified
```

> **从 1.3.0 之前的部署升级**（client 旧名 `vue-client` / `admin-console`，或旧版手工 SQL 建的行）：先重命名存量行，让已有的授权记录和令牌继续指向存活的 client，播种器会补齐其余：
>
> ```sql
> UPDATE oauth2_clients SET client_id = 'fulla-portal' WHERE client_id = 'vue-client';
> UPDATE oauth2_client_scopes SET client_id = 'fulla-portal' WHERE client_id = 'vue-client';
> UPDATE oauth2_clients SET client_id = 'fulla-admin-console' WHERE client_id = 'admin-console';
> UPDATE oauth2_client_scopes SET client_id = 'fulla-admin-console' WHERE client_id = 'admin-console';
> ```

---

## 性能调优（推荐配置）

> 本节是官方推荐的生产性能基线（分析依据为维护者基准档案，关键结论与实测
> 数据已直接收录本节）。
> 基准测试以本节配置为准——写进本节的配置即"官方配置"。

### 1. 开启 Redis L2 缓存（吞吐档推荐，须配套扩容 Redis 连接池）

读路径（introspect / userinfo 的 token 查询、client 查询）命中 Redis，不再落到
PostgreSQL。`config.prod.json` 出厂保持关闭（`cache.enabled: false`）——开启前
**必须**同步扩容 `redis_clients[0].number_of_connections`（见下方实测数据）：

```json
"cache": {
    "enabled": true,
    "redis_client_name": "default",
    "ttl_seconds": {
        "client": 300,
        "access_token_max": 60
    }
}
```

语义说明：token 缓存 TTL 不超过 60s 且吊销即时失效（含负缓存）；client
缓存 TTL 300s。要求部署内 Redis 可用（生产 compose 已含 fulla-redis）。
多实例部署注意：写路径失效的 Redis DEL 对全实例即时生效，但 userinfo 的
进程内 piggyback memo（2s 一次性）只在处理写请求的那台实例被同步清除，
其它实例最长滞后 2s（TTL 自兜底，可接受）。

**实测（2026-08-18 基准环境，10s 快测）**：cache on + Redis 池 20 时 S6 反而
-18%（池排队）；Redis 池扩到 64 后 S2 +39%、S3 +59%、S6 +6%。结论：**cache
收益以 Redis 池 ≥ 预期并发为前提**，出厂默认关闭 + 本节指引是安全姿势。
另注意：introspect 的正向缓存受 N2 判别器约束（仅在 token 走过发放/校验
路径后才回填），S3 的收益主要来自 client 缓存与 PG 调优。

### 2. PostgreSQL 实例调优

出厂默认（`shared_buffers=128MB`、`checkpoint_timeout=5min`、
`max_wal_size=1GB`）面向小内存机器，在高频写入下产生周期性 checkpoint
刷盘尖峰。为 16GB / 8 vCPU 主机推荐的调优（按内存等比缩放
`shared_buffers` ≈ 25% RAM）：

```yaml
  fulla-postgres:
    command:
      - postgres
      - -c
      - shared_buffers=4GB
      - -c
      - effective_cache_size=12GB
      - -c
      - work_mem=16MB
      - -c
      - checkpoint_timeout=15min
      - -c
      - max_wal_size=4GB
      - -c
      - min_wal_size=1GB
      - -c
      - wal_compression=on
      - -c
      - checkpoint_completion_target=0.9
      - -c
      - autovacuum_vacuum_insert_scale_factor=0.02
      - -c
      - autovacuum_vacuum_scale_factor=0.02
```

该配置为基准环境实测采用的形态，完整可运行示例见
`benchmarks/fulla/docker-compose.bench.yml`（bench overlay，叠加在
`deploy/docker/docker-compose.yml` 之上）。纯 conf 调优对现有数据卷无
兼容性影响，可随时启用/回退。**注**：bench overlay 已将 `shared_buffers`
调至 1GB（2026-08-22 三臂 A/B 验证与 4GB 等效）；上表 4GB 仍为 16GB 主机的
PG 官方推荐起点。

**版本与升级注记**：deploy compose 自 2026-08-18 起使用 `postgres:17-alpine`
（与客户端 libpq 17.x 对齐，基准在 17 上实测）。**存量 15 版数据卷不能直接
在 17 上启动**（大版本数据目录不兼容）——升级前先 `pg_dump`/`pg_restore`
或用 `pg_upgrade`；全新部署无此步骤。

### 3. 会话留存（session_timeout）——按 API 流量调尺寸

**要点**：`enable_session: true` 时，Drogon 框架层为每个不带会话 cookie 的
请求创建 Session 并持有到 `session_timeout` 到期（上游
[drogon#278](https://github.com/an-tao/drogon/issues/278) 行为，本仓 2026-08-22
实测验证）。API 流量（从不带 cookie）按请求付费：**每请求约留存 750 B**
（`API_QPS × session_timeout × 750 B`），且对所有端点存在约 -54% 的吞吐税。

**留存公式、尺寸速查表与分档指引**（交互型低于 100 QPS 保持默认 3600s；API
型按表调低；基准档 30s）的完整版见
[会话管理 · 尺寸速查](../domains/session-management.md)——该文档是此主题的
单一出处，本节只保留运维要点。

### 4. Docker 网络拓扑（原生引擎可选；Docker Desktop 下不可用）

若使用**原生 Docker Engine**（Linux 服务器直装），可将 backend + PG + redis
置于 `network_mode: host`：backend↔PG/Redis 走 loopback，省去每包 veth 穿越。

**Docker Desktop（WSL2 集成）下不要使用**（2026-08-18 实测）：`host` 是引擎
VM 的 netns 而非发行版的 netns，host 模式监听端口对发行版完全不可达
（127.0.0.1、共享 eth0 IP、host.docker.internal 均超时；仅发布端口被转发）。
基准环境因此保持 bridge + 发布端口拓扑，四产品一致（公平性不受影响）。

跨机部署（nginx 前置、独立 DB）不受此项影响。

---

## 运维操作

### 查看日志

```bash
# 所有服务
docker compose -f deploy/docker/docker-compose.prod.yml logs -f

# 单个服务
docker compose -f deploy/docker/docker-compose.prod.yml logs -f fulla-backend
docker compose -f deploy/docker/docker-compose.prod.yml logs -f nginx
```

### 重启服务

```bash
# 重启单个服务
docker compose -f deploy/docker/docker-compose.prod.yml restart fulla-backend

# 重建并重启（代码更新后）
docker compose -f deploy/docker/docker-compose.prod.yml up -d --build fulla-backend
docker compose -f deploy/docker/docker-compose.prod.yml up -d --build fulla-frontend
docker compose -f deploy/docker/docker-compose.prod.yml up -d --build fulla-admin
```

### 更新部署

```bash
git pull
docker compose -f deploy/docker/docker-compose.prod.yml up -d --build
```

### 数据库备份

```bash
# 备份
docker exec fulla-postgres pg_dump -U fulla_user fulla_db > backup_$(date +%Y%m%d).sql

# 恢复
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < backup_20260526.sql
```

### TLS 证书续期

Let's Encrypt 证书 90 天过期。设置自动续期：

```bash
# 测试续期流程（dry run）
sudo certbot renew --dry-run

# 启用 systemd 定时器（多数发行版安装 certbot 时已自动创建）
sudo systemctl enable certbot.timer
sudo systemctl start certbot.timer

# 或使用 cron 定时任务
(crontab -l 2>/dev/null; echo "0 3 1 * * sudo certbot renew --quiet && cp /etc/letsencrypt/live/your-domain.com/fullchain.pem /path/to/repo/deploy/nginx/ssl/ && cp /etc/letsencrypt/live/your-domain.com/privkey.pem /path/to/repo/deploy/nginx/ssl/ && docker compose -f /path/to/repo/deploy/docker/docker-compose.prod.yml restart nginx") | sudo crontab -
```

> cron 任务将续期后的证书复制到 nginx SSL 目录并重启 nginx 容器。请根据实际部署路径调整上方命令中的路径。

### 监控

- Prometheus: `http://your-server:9090`
- 后端指标: `https://your-domain.com/metrics`（仅内网可访问）
- 健康检查: `https://your-domain.com/health`

---

## 故障排除

### 容器启动失败

```bash
# 查看容器状态
docker compose -f deploy/docker/docker-compose.prod.yml ps

# 查看失败容器日志
docker compose -f deploy/docker/docker-compose.prod.yml logs fulla-backend
```

### 数据库连接失败

```bash
# 检查 postgres 是否就绪
docker exec fulla-postgres pg_isready -U fulla_user

# 检查网络连通性
docker exec fulla-backend curl -s http://fulla-postgres:5432 || echo "Cannot reach postgres"
```

### 证书问题

```bash
# 检查证书是否存在
ls -la deploy/nginx/ssl/

# 检查证书有效期
openssl x509 -in deploy/nginx/ssl/fullchain.pem -noout -dates
```

### 前端 404

如果前端页面刷新后 404，检查 nginx.conf 中的 SPA fallback 配置：
- OAuth2Frontend: `try_files $uri $uri/ /index.html`
- OAuth2Admin: `try_files $uri $uri/ /admin/index.html`

---

## 安全清单

- [ ] 所有密码使用强随机值（`openssl rand -base64 32`）
- [ ] TLS 证书有效且自动续期（certbot timer 或 cron 任务已配置）
- [ ] `.env.docker` 文件权限设为 600
- [ ] `deploy/keys/signing.pem` 权限设为 600
- [ ] 首次部署后修改 admin 默认密码
- [ ] Prometheus 端口 9090 不对外暴露（或加认证）
- [ ] 定期备份数据库
- [ ] 监控磁盘空间（日志、数据库）
