# Production Deployment Guide

This guide explains how to deploy the full OAuth2 stack (user frontend + admin console + backend API) to a production environment.

---

## Architecture Overview

```
                    Internet
                       │
                ┌──────┴──────┐
                │   Nginx     │  :80 → :443 (TLS)
                │   reverse   │
                │   proxy     │
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

**Routing rules (Nginx)**:
- `/api/*`, `/oauth2/*`, `/.well-known/*`, `/health` → Backend
- `/admin/*` → Admin Console
- `/*` (everything else) → User Frontend

---

## Prerequisites

### Hardware requirements
- **CPU**: 2 cores or more
- **Memory**: 4GB or more (8GB recommended)
- **Disk**: 20GB or more of free space
- **Network**: public IP, with a domain resolved to the server

### Supported operating systems

- Ubuntu 20.04 / 22.04 / 24.04 LTS
- Debian 11 / 12
- CentOS Stream 8 / 9
- Rocky Linux 8 / 9

### Installing software dependencies

#### 1. Install Docker

**Ubuntu/Debian**:
```bash
# Update the package index
sudo apt update

# Install required dependencies
sudo apt install -y ca-certificates curl gnupg lsb-release

# Add Docker's official GPG key
sudo mkdir -p /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg

# Set up the Docker repository
echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu \
  $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

# Install Docker Engine
sudo apt update
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# Start the Docker service
sudo systemctl start docker
sudo systemctl enable docker

# Verify the installation
docker --version
docker compose version
```

**CentOS/Rocky Linux**:
```bash
# Install required dependencies
sudo yum install -y yum-utils device-mapper-persistent-data lvm2

# Add the Docker repository
sudo yum-config-manager --add-repo https://download.docker.com/linux/centos/docker-ce.repo

# Install Docker
sudo yum install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# Start the Docker service
sudo systemctl start docker
sudo systemctl enable docker

# Verify the installation
docker --version
docker compose version
```

#### 2. Configure the Docker group (optional but recommended)

```bash
# Create the docker group (if it does not exist)
sudo groupadd docker

# Add the current user to the docker group
sudo usermod -aG docker $USER

# Log out and back in, or run the following command for the group membership to take effect
newgrp docker

# Verify: run docker without sudo
docker ps
```

#### 2.5. Configure Docker registry mirrors (required in mainland China)

Docker Hub is unstable to reach from mainland China and image pulls will time out (typical error: `dial tcp registry-1.docker.io:443: i/o timeout`); you must configure registry mirrors.

The following mirror addresses were verified working on Alibaba Cloud servers as of 2026-06:

**Create or modify the Docker configuration file**:

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

> Note: with multiple mirrors configured, Docker tries them in order; a pull succeeds as soon as any one of them works.

**Restart the Docker service to apply the configuration**:

```bash
sudo systemctl daemon-reload
sudo systemctl restart docker
sudo systemctl status docker
```

**Verify the registry mirror configuration**:

```bash
# Check that the configuration was loaded (should show the registry-mirrors list above)
docker info | grep -A 5 "Registry Mirrors"

# Test image pulls (all images required by this project)
docker pull postgres:17-alpine
docker pull redis:7-alpine
docker pull nginx:stable-alpine
docker pull prom/prometheus:latest
docker pull ubuntu:24.04
```

If a mirror reports an error (such as `502` or `i/o timeout`), Docker automatically tries the next one; if all of them fail, see the troubleshooting notes below.

**Troubleshooting**:

1. **All mirrors failed**: visit [dongyubin/DockerHub](https://github.com/dongyubin/DockerHub) for the latest working list, replace the addresses in `daemon.json`, and restart Docker.

2. **Use a dedicated Alibaba Cloud mirror** (requires an Alibaba Cloud account; most stable):
   - Log in to [Alibaba Cloud Container Registry](https://cr.console.aliyun.com/) → Image Tools → Image Accelerator
   - Obtain your dedicated mirror address (of the form `https://<your_code>.mirror.aliyuncs.com`)
   - Put that address at the head of the `registry-mirrors` array in `daemon.json`

3. **Pull through a proxy** (if you have a usable proxy server):

   ```bash
   # Configure a proxy for the Docker daemon
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

#### 3. Install Git

**Ubuntu/Debian**:
```bash
sudo apt install -y git
```

**CentOS/Rocky Linux**:
```bash
sudo yum install -y git
```

#### 4. Install OpenSSL (for key generation)

**Ubuntu/Debian**:
```bash
sudo apt install -y openssl
```

**CentOS/Rocky Linux**:
```bash
sudo yum install -y openssl
```

#### 5. Install Certbot (for obtaining Let's Encrypt certificates)

**Ubuntu/Debian**:
```bash
sudo apt install -y certbot
```

**CentOS/Rocky Linux**:
```bash
sudo yum install -y certbot
```

### Verify the dependency installation

```bash
# Check the Docker version (24+ required)
docker --version

# Check the Docker Compose version (v2 required)
docker compose version

# Check Git
git --version

# Check OpenSSL
openssl version

# Check Certbot
certbot --version
```

### Domain and DNS configuration

1. **Domain resolution**: make sure the A record of your domain (e.g. `your-domain.example.com`) points to the server's public IP
2. **Verify DNS propagation**:
   ```bash
   # Check that the domain resolves correctly
   dig +short your-domain.example.com
   nslookup your-domain.example.com
   ```
3. **Firewall configuration**: make sure the following ports are reachable:
   - `80/tcp` (HTTP)
   - `443/tcp` (HTTPS)

### Firewall configuration

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

## Quick deployment (5 steps)

### 1. Clone the project

```bash
git clone <repo-url>
cd fulla
```

### 2. Generate keys

```bash
# Generate the JWT signing key
chmod +x scripts/generate-jwt-keys.sh
./scripts/generate-jwt-keys.sh

# Generate a temporary self-signed TLS certificate (needed for nginx to start)
chmod +x scripts/generate-certs.sh
./scripts/generate-certs.sh
```

> The self-signed certificate is a bootstrap placeholder — nginx requires cert files to exist before it can start. You will replace it with a Let's Encrypt certificate in step 6 below.

**Using Let's Encrypt in production** (after step 4 starts the services):
```bash
# 1. Stop the nginx container to free port 80
docker compose -f deploy/docker/docker-compose.prod.yml stop nginx

# 2. Obtain the certificate (port 80 must be free for standalone challenge)
sudo certbot certonly --standalone -d your-domain.com

# 3. Copy the certificates
cp /etc/letsencrypt/live/your-domain.com/fullchain.pem deploy/nginx/ssl/
cp /etc/letsencrypt/live/your-domain.com/privkey.pem deploy/nginx/ssl/

# 4. Restart nginx with the real certificate
docker compose -f deploy/docker/docker-compose.prod.yml start nginx
```

### 3. Configure environment variables

```bash
# Check that the template file exists
[ -f deploy/env/docker.env.example ] && echo "Template file exists" || echo "Error: template file missing"

cp deploy/env/docker.env.example .env.docker
```

Edit `.env.docker` to set strong passwords and the HTTPS-domain-related configuration:

```env
# Image version tag for ghcr.io/voidvec/fulla-* images (default: latest)
FULLA_VERSION=latest

# Run mode (production enforces HTTPS issuer / strong passwords; must be paired with FULLA_ISSUER=https://)
FULLA_ENV=production
FULLA_ISSUER=https://your-domain.com

# JWT signing key (required in production; without it tokens are invalidated on every restart)
FULLA_JWT_KEY_PATH=/app/keys/signing.pem

# ⚠ POSTGRES_PASSWORD and FULLA_DB_PASSWORD must be identical
POSTGRES_USER=fulla_user
POSTGRES_PASSWORD=<generate a strong password>
POSTGRES_DB=fulla_db
FULLA_DB_HOST=fulla-postgres
FULLA_DB_PORT=5432
FULLA_DB_NAME=fulla_db
FULLA_DB_USER=fulla_user
FULLA_DB_PASSWORD=<same as POSTGRES_PASSWORD>

# ⚠ REDIS_PASSWORD and FULLA_REDIS_PASSWORD must be identical
REDIS_PASSWORD=<generate a strong password>
FULLA_REDIS_HOST=fulla-redis
FULLA_REDIS_PORT=6379
FULLA_REDIS_PASSWORD=<same as REDIS_PASSWORD>

# CORS / OAuth callbacks (HTTPS domain required, otherwise browser requests are blocked)
FULLA_FRONTEND_URL=https://your-domain.com
FULLA_CORS_ALLOW_ORIGINS=https://your-domain.com
# ⚠ FULLA_VUE_REDIRECT_URI and VITE_REDIRECT_URI must be identical
FULLA_VUE_REDIRECT_URI=https://your-domain.com/callback
FULLA_VUE_CLIENT_SECRET=<generate a strong password>
FULLA_GOOGLE_REDIRECT_URI=https://your-domain.com/callback

# Error verbosity (false recommended in production; do not expose field-level validation errors)
DETAILED_VALIDATION_ERRORS=false

# External Auth (optional)
FULLA_GITHUB_CLIENT_ID=
FULLA_GITHUB_CLIENT_SECRET=
FULLA_GOOGLE_CLIENT_ID=
FULLA_GOOGLE_CLIENT_SECRET=
FULLA_WECHAT_APPID=
FULLA_WECHAT_SECRET=

# Email service (SMTP) — must be configured in production
FULLA_SMTP_HOST=smtp.example.com
FULLA_SMTP_PORT=465
FULLA_SMTP_USER=noreply@example.com
FULLA_SMTP_PASSWORD=<SMTP authorization code, not the mailbox login password>
FULLA_SMTP_FROM_NAME=Fulla
FULLA_SMTP_SSL=true

# Frontend build variables (injected at Vite build time)
# VITE_API_BASE_URL must be left empty in production → the SPA uses relative paths (same-origin reverse proxying via nginx)
VITE_API_BASE_URL=
VITE_CLIENT_ID=fulla-portal
VITE_REDIRECT_URI=https://your-domain.com/callback
VITE_GITHUB_CLIENT_ID=
```

> **Critical coupling**: `FULLA_ENV=production` and `FULLA_ISSUER=https://...` must be set together. Setting production without an HTTPS issuer makes backend startup validation fail (the prod-mode check in `ConfigManager` rejects non-https issuers). Likewise, the DB/Redis passwords must not be the defaults `123456` / `password`, or the prod validation will also refuse to start.

Generate strong passwords:
```bash
openssl rand -base64 32
```

#### Email service (SMTP) configuration notes

The backend email service has two modes (chosen automatically by `getEmailService()` based on environment variables):

| Mode | Trigger | Behavior |
|------|---------|------|
| **Console mode** | `FULLA_SMTP_HOST` / `USER` / `PASSWORD` not set | Email content is only written to the backend log; **nothing is actually sent** |
| **SMTP mode** | All three variables above are set and non-empty | Emails are actually sent via SMTP |

> **SMTP must be configured in production**, otherwise emails for features such as email verification and password reset are never actually delivered to users (they only land in the server logs).

**Common email provider configuration reference**:

| Provider | SMTP host | Port | SSL | Credential notes |
|--------|----------|------|-----|---------|
| 163 Mail | `smtp.163.com` | 465 | true | Authorization code (not the login password) |
| QQ Mail | `smtp.qq.com` | 465 | true | Authorization code |
| Gmail | `smtp.gmail.com` | 465 | true | App password (2FA must be enabled) |
| Tencent Exmail | `smtp.exmail.qq.com` | 465 | true | Mailbox password |
| Alibaba Cloud enterprise mail | `smtp.qiye.aliyun.com` | 465 | true | Mailbox password |
| SendGrid | `smtp.sendgrid.net` | 587 | false | Username `apikey`, password is the API key |

**Obtaining an authorization code (163 example)**:
1. Log in to the 163 Mail web interface
2. Settings → POP3/SMTP/IMAP
3. Enable the SMTP service
4. Follow the prompts to generate an authorization code (a 16-character string)

Restart the backend after configuring for the change to take effect:

```bash
docker compose -f deploy/docker/docker-compose.prod.yml --env-file .env.docker up -d fulla-backend

# Verify the switch to SMTP mode (should print "Email service: SMTP (...)")
docker compose -f deploy/docker/docker-compose.prod.yml logs fulla-backend | grep "Email service"
```

### 4. Start the services

```bash
# --build compiles images from source (required for first deployment from a cloned repo)
docker compose -f deploy/docker/docker-compose.prod.yml --env-file .env.docker up -d --build
```

> Subsequent restarts (after config changes only, no code changes) can omit `--build`. After code updates via `git pull`, always include `--build` to pick up the changes.

### 5. Verify the deployment

```bash
# Check the status of all containers
docker compose -f deploy/docker/docker-compose.prod.yml ps

# Check backend health
curl -k https://localhost/health

# Check the frontend
curl -k https://localhost/

# Check the admin console
curl -k https://localhost/admin/
```

---

## Service details

### User frontend (OAuth2Frontend)

| Item | Value |
|------|-----|
| Container name | fulla-frontend |
| Build | Dockerfile (target: frontend-runtime) |
| Base image | nginx:stable-alpine |
| Internal port | 80 |
| Access path | `https://your-domain.com/` |
| Features | Login, registration, profile, security settings, OAuth2 authorization |

### Admin console (OAuth2Admin)

| Item | Value |
|------|-----|
| Container name | fulla-admin |
| Build | frontends/admin/Dockerfile |
| Base image | nginx:alpine |
| Internal port | 80 |
| Access path | `https://your-domain.com/admin/` |
| Features | Application management, user management, role/scope/token management |

### Backend API (fulla-server)

| Item | Value |
|------|-----|
| Container name | fulla-backend |
| Build | Dockerfile (target: backend-runtime) |
| Base image | ubuntu:24.04 (minimal) |
| Internal port | 5555 |
| Access path | `https://your-domain.com/api/*`, `/oauth2/*` |
| Database migration | One-shot `migrate` service (compose profile `migrate`; `FULLA_AUTO_MIGRATE=false`) |

### Infrastructure

| Service | Image | Purpose |
|------|------|------|
| fulla-postgres | postgres:17-alpine | Primary database |
| fulla-redis | redis:7-alpine | Token cache |
| oauth2-nginx | nginx:stable-alpine | TLS termination + reverse proxy |
| fulla-prometheus | prom/prometheus | Monitoring metrics collection |

---

## Configuration reference

### Backend configuration (config.prod.json)

The backend overrides configuration-file values with environment variables (precedence: `.env` file > system environment variables > `config.prod.json` defaults):

| Environment variable | Purpose | Default |
|----------|------|--------|
| `FULLA_ENV` | Run mode (`production` enables strict HTTPS issuer + strong-password validation) | development |
| `FULLA_ISSUER` | JWT issuer (must be `https://` in production) | http://localhost:5555 |
| `FULLA_JWT_KEY_PATH` | Path to the JWT signing key file | /app/keys/signing.pem |
| `FULLA_SIGNING_KEY` | JWT key PEM content (either this or `JWT_KEY_PATH`) | (optional) |
| `FULLA_DB_HOST` | PostgreSQL host | postgres |
| `FULLA_DB_PORT` | PostgreSQL port | 5432 |
| `FULLA_DB_NAME` | Database name | fulla_db_prod |
| `FULLA_DB_USER` | Database user | fulla_user |
| `FULLA_DB_PASSWORD` | Database password | (must be set) |
| `FULLA_REDIS_HOST` | Redis host | redis |
| `FULLA_REDIS_PORT` | Redis port | 6379 |
| `FULLA_REDIS_PASSWORD` | Redis password | (must be set) |
| `FULLA_LISTEN_PORT` | Backend listen port | 5555 |
| `FULLA_FRONTEND_URL` | Frontend URL (used for redirects etc.) | http://localhost:5173 |
| `FULLA_CORS_ALLOW_ORIGINS` | CORS allowed origins (comma-separated; overrides the JSON array) | localhost list from config |
| `FULLA_PORTAL_REDIRECT_URI` | fulla-portal OAuth callback URI (legacy alias: `FULLA_VUE_REDIRECT_URI`) | localhost value from config |
| `FULLA_GOOGLE_REDIRECT_URI` | Google OAuth callback URI | localhost value from config |
| `FULLA_PORTAL_CLIENT_SECRET` | fulla-portal secret (legacy alias: `FULLA_VUE_CLIENT_SECRET`) | 123456 |
| `FULLA_ADMIN_CONSOLE_REDIRECT_URI` | fulla-admin-console OAuth callback URI | localhost value from config |
| `FULLA_MFA_TOTP_ISSUER` | Issuer name shown by authenticator apps for TOTP entries | `Fulla` |
| `FULLA_AUTO_MIGRATE` | Run database migrations automatically | false (use the one-shot `migrate` service) |
| `DETAILED_VALIDATION_ERRORS` | Whether to return field-level validation errors (false recommended in production) | false |
| `FULLA_GITHUB_CLIENT_ID` / `FULLA_GITHUB_CLIENT_SECRET` | GitHub OAuth (optional) | (empty) |
| `FULLA_GOOGLE_CLIENT_ID` / `FULLA_GOOGLE_CLIENT_SECRET` | Google OAuth (optional) | (empty) |
| `FULLA_WECHAT_APPID` / `FULLA_WECHAT_SECRET` | WeChat OAuth (optional) | (empty) |
| `FULLA_SMTP_HOST` | SMTP server host (unset means email stays in Console mode) | (optional) |
| `FULLA_SMTP_PORT` | SMTP port | 465 |
| `FULLA_SMTP_USER` | SMTP username (full email address) | (optional) |
| `FULLA_SMTP_PASSWORD` | SMTP authorization code (not the mailbox login password) | (optional) |
| `FULLA_SMTP_FROM_NAME` | Sender display name | Fulla |
| `FULLA_SMTP_SSL` | Whether to enable SSL | true |

> **Email mode note**: real SMTP sending is enabled only when all three of `FULLA_SMTP_HOST` + `FULLA_SMTP_USER` + `FULLA_SMTP_PASSWORD` are non-empty; otherwise email is only written to the backend log. See "Email service (SMTP) configuration notes" above.
>
> **CORS array override**: `FULLA_CORS_ALLOW_ORIGINS` is a comma-separated string (e.g. `https://a.com,https://b.com`) that the backend splits into a JSON array at startup to override `custom_config.cors.allow_origins` from `config.prod.json`. The CORS validation code requires this field to be an array, so you **must** use the comma-separated form — never write it as a JSON array literal.

### Nginx configuration

`deploy/nginx/nginx.conf` includes:
- Automatic HTTP → HTTPS redirection
- TLS 1.2/1.3 configuration
- Rate limiting rules (login: 5 requests/min/IP; API: 30 requests/s/IP)
- `/metrics` endpoint restricted to internal-network access
- HSTS headers

### Frontend configuration

The frontend (the user-facing OAuth2Frontend) is configured through Vite environment variables that are **injected at image build time** into the SPA bundle (they are not read at runtime). `fulla-frontend.build.args` in `docker-compose.prod.yml` passes these variables through from `.env.docker`, and the `frontend-builder` stage of the `Dockerfile` exposes them to Vite via `ARG`/`ENV`.

| Variable | Purpose | Production value |
|------|------|--------|
| `VITE_API_BASE_URL` | API base URL | **(empty)** — the SPA uses same-origin relative paths; setting a value breaks the nginx reverse-proxy routing |
| `VITE_CLIENT_ID` | OAuth2 Client ID | fulla-portal |
| `VITE_REDIRECT_URI` | OAuth2 callback URI | https://your-domain.com/callback |
| `VITE_GITHUB_CLIENT_ID` | GitHub "Sign in with GitHub" button (optional) | (button hidden when empty) |

> **The admin console (OAuth2Admin) needs no configuration**: its source code reads no `import.meta.env` at all; every API call uses the relative path `/api/admin/*`, which nginx reverse-proxies to the backend. When changing domains you only need to keep the nginx `/admin/` route correct — no admin image rebuild required.
>
> **Changing the domain requires rebuilding the frontend image**: because VITE variables are baked in at build time, after switching domains you must run `docker compose ... up -d --build fulla-frontend` (the admin console is unaffected).

---

## Database initialization

On first deployment the one-shot `migrate` service creates all required tables (the compose stack keeps `FULLA_AUTO_MIGRATE=false`; run `docker compose -f docker-compose.prod.yml --env-file <your-env> --profile migrate run --rm --build migrate` after `up` of the postgres/redis services — `--build` is REQUIRED on a clean checkout, otherwise compose pulls the released image, which may predate your local migrations). The **administrator account is bootstrapped automatically** on first start (see next step); OAuth2 clients are not — create them manually.

> The `dev_*.sql` files in `apps/server/seed/` use hard-coded passwords and localhost redirect URIs. **Do not use them in production.** Follow the steps below instead.

### 1. Create the administrator account

Generate a secure password hash and create the admin user:

```bash
# Generate a random password for the admin user
ADMIN_PASSWORD=$(openssl rand -base64 24)
echo "Admin password: $ADMIN_PASSWORD"
echo "Save this password — you will need it to log in to the admin console."
# The account is flagged must_change_password (#145): the first login forces a
# password change via the admin console's change-password form before any
# authorization is granted, so the printed/initial password cannot linger.

# Preferred: let the server bootstrap the admin on first start (random
# PBKDF2 password printed ONCE to the container log):
#   docker compose logs backend | grep Bootstrap
# Or set it explicitly before first start: FULLA_BOOTSTRAP_ADMIN_PASSWORD=...
# Manual fallback (PBKDF2-SHA256, 310k iterations, same format as the
# server). Note: the users.salt column stays empty — it is only used by the
# retired legacy SHA-256 verification path; PBKDF2 embeds the salt in the
# hash string:
ADMIN_HASH=$(python3 -c "import hashlib,os;pw=os.environ['ADMIN_PASSWORD'];salt=os.urandom(16);print('\$pbkdf2-sha256\$310000\$'+salt.hex()+'\$'+hashlib.pbkdf2_hmac('sha256',pw.encode(),salt,310000,32).hex())")

# Create the admin user (must_change_password=true mirrors the bootstrapper:
# the first login forces a password change, #145)
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db <<EOF
INSERT INTO users (username, password_hash, salt, email, must_change_password)
VALUES ('admin', '${ADMIN_HASH}', '', 'admin@your-domain.com', true)
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

### 2. OAuth2 clients (automatic)

The two first-party clients — `fulla-portal` (user frontend) and `fulla-admin-console` (admin console) — are **seeded automatically at server startup** (#204): the bootstrap seeder reads the `clients` block of the OAuth2Plugin config and inserts any missing rows (idempotent `ON CONFLICT DO NOTHING`, so clients edited through the admin API are never clobbered by a restart).

You only provide the redirect URIs through `.env.docker`; the server applies them to the config before seeding:

```bash
FULLA_PORTAL_REDIRECT_URI=https://your-domain.com/callback
FULLA_ADMIN_CONSOLE_REDIRECT_URI=https://your-domain.com/admin/callback
```

The `fulla-portal` client_id must match `VITE_CLIENT_ID` in `.env.docker` (default: `fulla-portal`), and `VITE_REDIRECT_URI` must equal `FULLA_PORTAL_REDIRECT_URI` (the SPA bakes its redirect in at build time).

Nothing to run manually — continue with the stack startup below. First-boot log lines to expect:

```
ClientSeeder: seeded client 'fulla-portal'
ClientSeeder: seeded client 'fulla-admin-console'
ClientSeeder: config clients seeded/verified
```

> **Upgrading from a pre-1.3.0 deployment** (clients named `vue-client` / `admin-console`, or rows created by the old manual SQL): rename the existing rows once so consents and tokens keep pointing at a live client, then let the seeder add anything missing:
>
> ```sql
> UPDATE oauth2_clients SET client_id = 'fulla-portal' WHERE client_id = 'vue-client';
> UPDATE oauth2_client_scopes SET client_id = 'fulla-portal' WHERE client_id = 'vue-client';
> UPDATE oauth2_clients SET client_id = 'fulla-admin-console' WHERE client_id = 'admin-console';
> UPDATE oauth2_client_scopes SET client_id = 'fulla-admin-console' WHERE client_id = 'admin-console';
> ```

---

## Performance tuning (recommended configuration)

> This section is the officially recommended production performance baseline (the analysis draws on the maintainers' benchmark archives; key conclusions and measured data are incorporated directly in this section). Benchmarks treat the configuration in this section as authoritative — whatever is written here is the "official configuration".

### 1. Enable the Redis L2 cache (recommended for the throughput tier; requires enlarging the Redis connection pool accordingly)

On the read path (token and client lookups for introspect / userinfo), requests hit Redis instead of falling through to PostgreSQL. `config.prod.json` ships with the cache disabled (`cache.enabled: false`) — before enabling it you **must** also enlarge `redis_clients[0].number_of_connections` (see the measured data below):

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

Semantics: the token cache TTL never exceeds 60s and revocations take effect immediately (negative cache included); the client cache TTL is 300s. It requires Redis to be available inside the deployment (the production compose already includes fulla-redis). Note for multi-instance deployments: the Redis DEL issued by write-path invalidation takes effect immediately across all instances, but the in-process piggyback memo for userinfo (a 2s one-shot) is only synchronously cleared on the instance that handled the write request; other instances lag by at most 2s (the TTL self-heals, which is acceptable).

**Measured (2026-08-18 benchmark environment, 10s quick test)**: with the cache on and a Redis pool of 20, S6 actually regressed by -18% (pool queuing); after enlarging the Redis pool to 64, S2 +39%, S3 +59%, S6 +6%. Conclusion: **cache gains presuppose a Redis pool ≥ the expected concurrency**; the factory default of disabled plus the guidance in this section is the safe posture. Also note: the introspect positive cache is constrained by the N2 discriminator (it is only backfilled after a token has gone through the issuance/validation path), so the S3 gains come mainly from the client cache and the PG tuning.

### 2. PostgreSQL instance tuning

The factory defaults (`shared_buffers=128MB`, `checkpoint_timeout=5min`, `max_wal_size=1GB`) target small-memory machines and produce periodic checkpoint flush spikes under high write rates. Recommended tuning for a 16GB / 8 vCPU host (scale `shared_buffers` proportionally with memory, ≈ 25% of RAM):

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

This is the exact form used in the measured benchmark environment; a complete runnable example lives in `benchmarks/fulla/docker-compose.bench.yml` (a bench overlay layered on top of `deploy/docker/docker-compose.yml`). Pure conf-level tuning has no compatibility impact on existing data volumes and can be enabled or rolled back at any time. **Note**: the bench overlay has since lowered `shared_buffers` to 1GB (verified in the 2026-08-22 three-arm A/B test to be equivalent to 4GB); the 4GB value above remains the officially recommended PG starting point for 16GB hosts.

**Version and upgrade note**: since 2026-08-18 the deploy compose uses `postgres:17-alpine` (aligned with the client-side libpq 17.x; benchmarks were measured on 17). **Existing data volumes from version 15 cannot start directly on 17** (major-version data-directory incompatibility) — run `pg_dump`/`pg_restore` or use `pg_upgrade` before upgrading; fresh deployments can skip this step.

### 3. Session retention (session_timeout) — size it to your API traffic

**Key point**: with `enable_session: true`, the Drogon framework layer creates a Session for every request that arrives without a session cookie and holds it until `session_timeout` expires (upstream [drogon#278](https://github.com/an-tao/drogon/issues/278) behavior, verified by measurement in this repo on 2026-08-22). API traffic (which never carries a cookie) pays per request: **about 750 B retained per request** (`API_QPS × session_timeout × 750 B`), plus a throughput tax of roughly -54% across all endpoints.

The full **retention formula, sizing quick-reference table, and tier-specific guidance** (interactive workloads under 100 QPS keep the default 3600s; API workloads lower it per the table; the benchmark tier uses 30s) lives in [Session management · sizing quick reference](../domains/session-management.md) — that document is the single source of truth on this topic, and this section keeps only the operational essentials.

### 4. Docker network topology (optional on the native engine; unavailable under Docker Desktop)

If you run the **native Docker Engine** (installed directly on a Linux server), you can put backend + PG + redis in `network_mode: host`: backend↔PG/Redis traffic goes over loopback, eliminating the per-packet veth traversal.

**Do not use this under Docker Desktop (WSL2 integration)** (measured 2026-08-18): `host` is the engine VM's netns, not the distro's netns, so ports listened on in host mode are completely unreachable from the distro (127.0.0.1, the shared eth0 IP, and host.docker.internal all time out; only published ports are forwarded). The benchmark environment therefore kept the bridge + published-ports topology, identical across all four products (fairness is unaffected).

Cross-machine deployments (nginx fronting, standalone DB) are unaffected by this item.

---

## Operational tasks

### View logs

```bash
# All services
docker compose -f deploy/docker/docker-compose.prod.yml logs -f

# A single service
docker compose -f deploy/docker/docker-compose.prod.yml logs -f fulla-backend
docker compose -f deploy/docker/docker-compose.prod.yml logs -f nginx
```

### Restart services

```bash
# Restart a single service
docker compose -f deploy/docker/docker-compose.prod.yml restart fulla-backend

# Rebuild and restart (after code updates)
docker compose -f deploy/docker/docker-compose.prod.yml up -d --build fulla-backend
docker compose -f deploy/docker/docker-compose.prod.yml up -d --build fulla-frontend
docker compose -f deploy/docker/docker-compose.prod.yml up -d --build fulla-admin
```

### Update the deployment

```bash
git pull
docker compose -f deploy/docker/docker-compose.prod.yml up -d --build
```

### Database backup

```bash
# Backup
docker exec fulla-postgres pg_dump -U fulla_user fulla_db > backup_$(date +%Y%m%d).sql

# Restore
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < backup_20260526.sql
```

### TLS certificate renewal

Let's Encrypt certificates expire after 90 days. Set up automatic renewal:

```bash
# Test the renewal process (dry run)
sudo certbot renew --dry-run

# Add a systemd timer for automatic renewal (most distros already do this on certbot install)
sudo systemctl enable certbot.timer
sudo systemctl start certbot.timer

# Or use a cron job as an alternative
(crontab -l 2>/dev/null; echo "0 3 1 * * sudo certbot renew --quiet && cp /etc/letsencrypt/live/your-domain.com/fullchain.pem /path/to/repo/deploy/nginx/ssl/ && cp /etc/letsencrypt/live/your-domain.com/privkey.pem /path/to/repo/deploy/nginx/ssl/ && docker compose -f /path/to/repo/deploy/docker/docker-compose.prod.yml restart nginx") | sudo crontab -
```

> The cron job copies the renewed certs to the nginx SSL directory and restarts the nginx container. Adjust paths to match your deployment location.

### Monitoring

- Prometheus: `http://your-server:9090`
- Backend metrics: `https://your-domain.com/metrics` (internal network only)
- Health check: `https://your-domain.com/health`

### audit_logs 分区维护（#83）

`audit_logs` 自 V025 起按月分区（`[当前月-12, 当前月+25]` 的窗口 + 一个 DEFAULT 兜底分区）。
**分区窗口不再需要手工 cron**：postgres 存储部署下，后端的清理服务
（`cleanup_interval_seconds`，默认 3600s）每个周期会在分布式锁内调用
`ensure_audit_partitions()`，自动创建缺失的月分区并把落入 DEFAULT 分区的行迁出。

排查命令：

```sql
-- 窗口内各分区的行量与 DEFAULT 分区是否在堆积（堆积 = 维护没在跑）
SELECT tableoid::regclass AS partition, count(*) FROM audit_logs GROUP BY 1 ORDER BY 1;

-- 手工触发一次维护（与自动路径相同的幂等函数；ahead/behind 可调）
SELECT ensure_audit_partitions();          -- 默认 ahead 24 / behind 12
SELECT ensure_audit_partitions(36, 12);    -- 扩大预建窗口
```

注意：

- 该函数幂等但非并发安全——自动路径已在清理锁内串行化；手工调用请避开整点清理时刻。
- memory/redis 存储模式没有 audit 分区（该功能仅 postgres）。
- 维护失败只会 `LOG_ERROR` 并在下个清理周期重试，不影响清理主流程。

### 社交账号绑定（#71）Redis 依赖

社交账号绑定功能的 link state（绑定流程的临时状态）存储在 Redis 中（`SET NX EX 600` / `GETDEL`，TTL 10 分钟）。

- **有 Redis 时**：绑定发起端点正常工作，link state 存入 Redis 并在回调时校验。
- **无 Redis 时**：绑定发起端点 fail-closed，返回 `NotConfigured` 类错误（HTTP 503）。
- **登录不受影响**：社交登录（已绑定账号的直接登录）不依赖 Redis link state，无需 Redis 也可正常工作。

生产部署确保 `fulla-redis` 容器运行即可（默认 compose 已包含）。

---

## Troubleshooting

### Container fails to start

```bash
# View container status
docker compose -f deploy/docker/docker-compose.prod.yml ps

# View the failed container's logs
docker compose -f deploy/docker/docker-compose.prod.yml logs fulla-backend
```

### Database connection failure

```bash
# Check whether postgres is ready
docker exec fulla-postgres pg_isready -U fulla_user

# Check network connectivity
docker exec fulla-backend curl -s http://fulla-postgres:5432 || echo "Cannot reach postgres"
```

### Certificate problems

```bash
# Check that the certificates exist
ls -la deploy/nginx/ssl/

# Check the certificate validity period
openssl x509 -in deploy/nginx/ssl/fullchain.pem -noout -dates
```

### Frontend 404

If frontend pages return 404 after a refresh, check the SPA fallback configuration in nginx.conf:
- OAuth2Frontend: `try_files $uri $uri/ /index.html`
- OAuth2Admin: `try_files $uri $uri/ /admin/index.html`

---

## Security checklist

- [ ] All passwords use strong random values (`openssl rand -base64 32`)
- [ ] TLS certificates are valid and auto-renew (certbot timer or cron job configured)
- [ ] `.env.docker` file permissions set to 600
- [ ] `deploy/keys/signing.pem` permissions set to 600
- [ ] Default admin password changed after the first deployment
- [ ] Prometheus port 9090 not exposed to the public (or protected by authentication)
- [ ] Database backed up regularly
- [ ] Disk space monitored (logs, database)
