# Windows Docker Desktop Deployment Validation Guide

This guide explains how to validate the deployment of the full fulla stack on Windows Docker Desktop. **Apart from domain and SSL, every other feature is fully identical to the Linux production environment**.

---

## Why validate on Windows Docker Desktop?

✓ **Fully simulates the production environment**: the same Docker Compose configuration, the same container images, the same network topology  
✓ **Fast feedback loop**: modify code locally → validate immediately → push to the Linux server only after everything checks out  
✓ **Saves time**: avoids the long "push → server pull → restart services → discover the problem" loop every single time  
✓ **Full coverage of core functionality**: database migrations, API endpoints, frontend routing, and OAuth2 flows are all testable  

**Differences from the Linux production environment**:
| Feature | Windows Docker Desktop | Linux production |
|------|------------------------|----------------|
| PostgreSQL | ✓ Identical | ✓ |
| Redis | ✓ Identical | ✓ |
| Backend API | ✓ Identical | ✓ |
| Frontend | ✓ Identical | ✓ |
| Admin console | ✓ Identical | ✓ |
| Nginx reverse proxy | ⚠ Simplified configuration (no TLS) | ✓ |
| Domain access | ✗ localhost only | ✓ |
| SSL/TLS | ✗ Not enabled | ✓ |

---

## Prerequisites

### Software requirements

1. **Windows 10/11 Pro or Enterprise** (Home edition requires manual WSL2 configuration)
2. **Docker Desktop for Windows** (latest stable version)
   - Download: https://www.docker.com/products/docker-desktop/
   - Enable the WSL2 backend (recommended) or Hyper-V during installation
3. **Git** (for cloning the project)
   - Download: https://git-scm.com/download/win
4. **OpenSSL** (for generating JWT keys, optional)
   - Windows: download from https://slproweb.com/products/Win32OpenSSL.html
   - Or use the OpenSSL bundled with Git Bash

### Verify the Docker Desktop installation

Open PowerShell or Windows Terminal:

```powershell
# Check the Docker version (20.10+ required)
docker --version

# Check the Docker Compose version (v2+ required)
docker compose version

# Check that Docker is running properly
docker ps
```

Example expected output:
```
Docker version 24.0.7, build afdd53b
Docker Compose version v2.23.0
CONTAINER ID   IMAGE     COMMAND   CREATED   STATUS    PORTS     NAMES
```

---

## Quick start (5 steps)

### 1. Clone the project

```powershell
# Clone the repository (replace with the actual URL)
git clone <repo-url>
cd fulla

# Check the branch
git branch
```

### 2. Generate JWT keys

**Option A: use Git Bash (recommended)**

```bash
# Run from the project root
cd /path/to/repo-root

# Generate the JWT signing key
chmod +x scripts/generate-jwt-keys.sh
./scripts/generate-jwt-keys.sh

# Verify key generation
ls -la deploy/keys/
# You should see signing.pem and signing.pub
```

**Option B: use PowerShell + OpenSSL**

```powershell
# After installing OpenSSL for Windows
openssl genrsa -out deploy\keys\signing.pem 2048
openssl rsa -in deploy\keys\signing.pem -pubout -out deploy\keys\signing.pub

# Verify
dir deploy\keys
```

**Option C: skip key generation (testing only)**

If you are only validating the deployment workflow, you can skip this step for now and the backend will use its built-in test keys (⚠ real keys must be generated for production).

### 3. Configure environment variables

```powershell
# Copy the environment variable template
Copy deploy\env\docker.env.example .env.docker

# Edit the file (with VS Code or Notepad)
notepad .env.docker
```

**Edit `.env.docker` and set local test passwords**:

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

# Domain (ignored for local testing)
DOMAIN=localhost
```

> **Note**: environment files on Windows use CRLF line endings; Docker Compose handles them automatically.

#### Email service configuration (optional)

The backend email service has two modes (decided by `getEmailService()` in [EmailService.cc](https://github.com/voidvec/fulla/blob/master/libs/drogon/src/utils/EmailService.cc)):

| Mode | Trigger | Behavior |
|------|---------|------|
| **Console mode** (default) | `FULLA_SMTP_*` not set | Verification emails are only written to the backend log; nothing is actually sent |
| **SMTP mode** | `FULLA_SMTP_HOST` + `FULLA_SMTP_USER` + `FULLA_SMTP_PASSWORD` set | Emails are actually sent via SMTP |

**Default Console mode (recommended for local validation)**:

No configuration required. After clicking "Send verification email", the email content (including the verification link) is written to the backend log; copy the link from there to verify:

```bash
docker logs fulla-backend --tail 50 2>&1 | grep -A 5 -iE "verify|email"
```

**Enable real SMTP delivery (163 Mail example)**:

Append the following to the end of `.env.docker`:

```env
# Email / SMTP
FULLA_SMTP_HOST=smtp.163.com
FULLA_SMTP_PORT=465
FULLA_SMTP_USER=your-email@163.com
FULLA_SMTP_PASSWORD=your-authorization-code   # 163 authorization code, not the login password
FULLA_SMTP_FROM_NAME=OAuth2 Platform
FULLA_SMTP_SSL=true                            # Port 465 requires SSL
```

> **Obtaining a 163 authorization code**: log in to the 163 Mail web interface → Settings → POP3/SMTP/IMAP → enable the SMTP service → generate an authorization code.

Restart the backend after the change for it to take effect:

```bash
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker up -d fulla-backend

# Verify the switch to SMTP mode (you should see "Email service: SMTP (smtp.163.com:465)")
docker logs fulla-backend 2>&1 | grep -i "Email service"
```

> **Note**: verification links inside emails use `FULLA_FRONTEND_URL` (`http://localhost:8080` locally), so they stop working when opened from another machine — this is an expected limitation of a local deployment.

### 4. Adjust the Docker Compose configuration

Since the local environment does not need HTTPS, we create a simplified Compose file:

**Option A: use the existing development configuration (recommended)**

```powershell
# Use docker-compose.yml directly (local ports already configured)
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker up -d --build
```

**Option B: create a custom configuration**

If you need more control, create `deploy/docker/docker-compose.windows.yml`:

```yaml
# Based on docker-compose.yml, with external auth and TLS-related configuration removed
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
      - ../../deploy/keys:/app/keys:ro  # JWT keys
      - ../../apps/server/migrations:/app/sql/migrations:ro
      - ../../apps/server/seed:/app/sql/seed:ro

  # Other services unchanged...
```

### 5. Start the services

```powershell
# Start all services
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker up -d --build

# Watch the startup logs
docker compose -f deploy/docker/docker-compose.yml logs -f
```

Expected output (services started successfully):
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

## Validating the deployment

### 1. Check container status

```powershell
docker compose -f deploy/docker/docker-compose.yml ps
```

All containers are expected to be `Up`:

```
NAME                STATUS          PORTS
fulla-admin        Up              0.0.0.0:8081->80/tcp
fulla-backend      Up              0.0.0.0:5555->5555/tcp
fulla-frontend     Up              0.0.0.0:8080->80/tcp
fulla-postgres     Up              0.0.0.0:5433->5432/tcp
fulla-prometheus   Up              0.0.0.0:9090->9090/tcp
fulla-redis        Up              0.0.0.0:6380->6379/tcp
```

### 2. Verify backend health

```powershell
curl http://localhost:5555/health
```

Expected response:
```json
{"status":"healthy","timestamp":"2024-06-23T10:30:00Z"}
```

### 3. Verify database migration

```powershell
# Enter the postgres container
docker exec -it fulla-postgres psql -U fulla_user -d fulla_db -c "\dt"

# Expect to see the OAuth2-related tables
# clients, users, tokens, authorization_codes, etc.
```

### 4. Verify frontend access

Open in a browser:
- **User frontend**: http://localhost:8080
- **Admin console**: http://localhost:8081/admin/
- **Prometheus**: http://localhost:9090

### 5. Create the administrator account

```Git Bash
# Run the seed scripts
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_admin_user.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_admin_console_client.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_vue_client.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_backend_client.sql
```

```powershell
# Run the admin user seed
Get-Content apps\server\seed\dev_admin_user.sql | docker exec -i fulla-postgres psql -U fulla_user -d fulla_db

# Run the admin console client seed
Get-Content apps\server\seed\dev_admin_console_client.sql | docker exec -i fulla-postgres psql -U fulla_user -d fulla_db

# Run the Vue client seed
Get-Content apps\server\seed\dev_vue_client.sql | docker exec -i fulla-postgres psql -U fulla_user -d fulla_db

# Run the backend-svc client seed
Get-Content apps\server\seed\dev_backend_client.sql | docker exec -i fulla-postgres psql -U fulla_user -d fulla_db
```

Verify the administrator account:

**Method 1: use Git Bash (recommended)**
```bash
docker exec -it fulla-postgres psql -U fulla_user -d fulla_db -c "SELECT username, email FROM users WHERE username = 'admin';"
```

**Method 2: use PowerShell**
```powershell
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "SELECT username, email FROM users WHERE username = 'admin';"
```

**Expected output**:
```
 username |       email       
----------+-------------------
 admin    | admin@example.com
```

**Verify the administrator role**:
```bash
docker exec -it fulla-postgres psql -U fulla_user -d fulla_db -c "SELECT u.username, u.email, r.name FROM users u LEFT JOIN user_roles ur ON u.id = ur.user_id LEFT JOIN roles r ON ur.role_id = r.id WHERE u.username = 'admin';"
```

**Expected output**:
```
 username |       email       | name  
----------+-------------------+-------
 admin    | admin@example.com | admin
```

---

## Running endpoint tests

The project ships a complete endpoint test suite for validating core OAuth2 functionality and the admin console API.

### Recommended: Git Bash

**Advantages**: native shell-script support, correct path handling, and parity with the Linux environment

#### 1. Run the core OAuth2 endpoint tests

```bash
# Enter the project directory
cd /path/to/repo-root

# Make sure the test scripts are executable
chmod +x scripts/backend/test-oauth2-endpoints.sh

# Run the tests (55 tests)
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
```

**Test coverage**:
- Health check, JWKS endpoint
- OAuth2 login, token exchange, refresh token
- Token introspection, revocation
- User registration, login, profile
- Password reset and change
- MFA setup, verification, disable
- Dynamic client registration (RFC 7591)
- WebAuthn authentication
- Device authorization flow
- External authentication (GitHub, Google, WeChat)

#### 2. Run the admin console API tests

```bash
chmod +x scripts/backend/test-admin-endpoints.sh
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

**Test coverage**:
- Admin login, dashboard statistics
- User management (CRUD operations)
- Client application management
- Scope management
- Token management
- Authorized user management
- Role and permission management

### Alternative: WSL2 Ubuntu

```bash
# 1. Start WSL2
wsl

# 2. Enter the project directory (mind the path translation)
cd /path/to/repo-root

# 3. Run the tests
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

### PowerShell hybrid approach

```powershell
# Invoke the tests via Git Bash from PowerShell
bash ./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
bash ./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

### Interpreting test results

#### Expected output

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

#### Troubleshooting failures

**All endpoint tests are expected to pass (current tally: 59 on the OAuth2 side, 52 on the Admin side)**. If any fail, check the following known environment dependencies:

1. **Test 10 fails**: `no access_token`
   - **Cause**: the `backend-svc` test client is missing
   - **Fix**: run `dev_backend_client.sql` to create the test client

2. **Test 20/20b fails**: `missing field: .client_id` or `Expected HTTP 400, got 403`
   - **Cause**: RBAC access control is working correctly; dynamic client registration requires special configuration
   - **Impact**: none (this is expected behavior)

3. **Cascading failures**: `skipped: no token`
   - **Cause**: an earlier test revoked the token, leaving later tests without one
   - **Impact**: none (the test scripts are designed this way)

#### Success criteria

**Deployment validation counts as successful only when all 59/52 tests pass**. If individual failures appear, first check whether they match the known script environment dependencies above (database reset, seed data, port conflicts); do not treat "partially passing" as the success criterion for a deployment.

### Pre-test preparation

#### 1. Make sure the database has seed data

```bash
# Run the required seed scripts
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_admin_user.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_admin_console_client.sql
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_vue_client.sql

# Optional: create the test client (improves the pass rate)
docker exec -i fulla-postgres psql -U fulla_user -d fulla_db < apps/server/seed/dev_backend_client.sql
```

#### 2. Verify the base services

```bash
# Check container status
docker ps

# Check backend health
curl http://localhost:5555/health

# Check the database connection
docker exec fulla-postgres pg_isready -U fulla_user
```

### Quick validation command

```bash
# Run all tests in one go
cd /path/to/repo-root && \
chmod +x scripts/backend/*.sh && \
echo "[+] Running OAuth2 core tests..." && \
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555 && \
echo "" && \
echo "[+] Running admin console API tests..." && \
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

---

## Functional test checklist

### Core OAuth2 flows

| Test item | Method | Expected result |
|--------|---------|---------|
| User registration | Frontend registration page | Registration succeeds and the user can log in |
| User login | POST /oauth2/login (first step of the authorization-code + PKCE flow) | Returns an authorization code |
| Refresh token | POST /oauth2/token (refresh_token grant) | Returns a new access_token |
| Token validation | POST /oauth2/introspect | Returns valid token information |
| Token revocation | POST /oauth2/revoke | Returns 200 OK |
| Authorization-code flow | /oauth2/authorize → /callback | Complete OAuth2 flow |
| Client management | Create/delete clients in the Admin Console | Operations succeed |

### API endpoint tests

#### Method 1: use the existing test scripts (recommended)

The project includes complete endpoint test scripts; running them via Git Bash is recommended:

**Run the core OAuth2 endpoint tests (59 tests)**:
```bash
# 1. Enter the project directory (Git Bash)
cd /path/to/repo-root

# 2. Make sure the test scripts are executable
chmod +x scripts/backend/test-oauth2-endpoints.sh

# 3. Run the tests
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
```

**Run the admin console API tests (52 tests)**:
```bash
chmod +x scripts/backend/test-admin-endpoints.sh
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

**Example expected output**:
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

**Success criteria**: all pass (59/52). For individual failures, first cross-check the environment dependencies per "Troubleshooting failures" above; do not treat "partially passing" as the success criterion for a deployment.

#### Method 2: test with the PowerShell script

`fulla-admin-console` is a PUBLIC client (PKCE enforced, no secret, no password grant), so constructing the token flow by hand is rather tedious; using the repository's bundled PowerShell test script is recommended (it implements PKCE login internally):

```powershell
# Run the admin console endpoint tests (PKCE login + 52 assertions)
.\scripts\backend\test-admin-endpoints.ps1 -BaseUrl "http://localhost:5555"

# If you already have an access_token from another source, call protected APIs directly to verify:
$headers = @{ Authorization = "Bearer <access_token>" }
Invoke-RestMethod -Uri "http://localhost:5555/api/admin/users" -Headers $headers
# Expected: user list JSON
```

#### Method 3: use WSL2 Ubuntu (recommended)

```bash
# 1. Start WSL2
wsl

# 2. Enter the project directory
cd /path/to/repo-root

# 3. Run the tests
./scripts/backend/test-oauth2-endpoints.sh http://localhost:5555
./scripts/backend/test-admin-endpoints.sh http://localhost:5555
```

### Frontend routing tests

| Path | Expected page |
|------|---------|
| http://localhost:8080/ | User login page |
| http://localhost:8080/register | User registration page |
| http://localhost:8080/profile | Profile page (login required) |
| http://localhost:8080/callback | OAuth2 callback page |
| http://localhost:8081/admin/ | Admin console (login required) |
| http://localhost:8081/admin/apps | Application management page |

---

## Troubleshooting common issues

### Docker Desktop fails to start

**Symptom**: `docker ps` reports "Cannot connect to the Docker daemon"

**Solution**:
1. Check whether Docker Desktop is running (system tray icon)
2. Restart Docker Desktop
3. Check whether Hyper-V or WSL2 is enabled:
   ```powershell
   # WSL2
   wsl --list --verbose
   
   # Hyper-V
   dism /Online /Get-FeatureInformation /FeatureName:Microsoft-Hyper-V
   ```

### Port conflicts

**Symptom**: containers fail to start and the log shows "port is already allocated"

**Find the process holding the port**:
```powershell
# Check port 8080
netstat -ano | findstr :8080

# Check port 5433
netstat -ano | findstr :5433
```

**Solution**:
1. Stop the conflicting service
2. Or change the port mapping in `docker-compose.yml` (e.g. to `8082:80`)

### Database connection failure

**Symptom**: the backend log shows "Connection refused" or "Host unreachable"

**Diagnosis**:
```powershell
# 1. Check the postgres container status
docker ps | findstr fulla-postgres

# 2. Check the postgres logs
docker logs fulla-postgres

# 3. Test the database connection
docker exec fulla-postgres pg_isready -U fulla_user

# 4. Test network connectivity from the backend container
docker exec fulla-backend curl -s http://fulla-postgres:5432 2>&1
docker exec fulla-backend curl -s http://fulla-redis:6379 2>&1
```

### Build failures

**Symptom**: `docker compose build` fails with "failed to solve"

**Solution**:
```powershell
# Clean the build cache
docker builder prune -a

# Rebuild
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker build --no-cache

# If it still fails, check disk space
docker system df
```

### Windows path issues

**Symptom**: volume mounts fail with the error "invalid mount config"

**Cause**: Windows path-translation problems (`C:\` → `/c/`)

**Solution**:
1. Run Docker commands from Git Bash (paths are translated automatically)
2. Or run them from WSL2:
   ```powershell
   wsl docker compose -f deploy/docker/docker-compose.yml up -d
   ```

---

## Mapping to the Linux production deployment

### Configuration file mapping

| Windows local | Linux production | Notes |
|-------------|-----------|------|
| `.env.docker` | `/root/fulla/.env.docker` | Environment variables are identical |
| `deploy/keys/signing.pem` | `/root/fulla/deploy/keys/signing.pem` | JWT keys |
| `docker-compose.yml` | `docker-compose.prod.yml` | Differences in ports and TLS configuration |

### Deployment command mapping

| Operation | Windows Docker Desktop | Linux production |
|------|----------------------|-----------|
| Start | `docker compose --env-file .env.docker up -d` | `docker compose --env-file .env.docker -f docker-compose.prod.yml up -d` |
| View logs | `docker compose logs -f` | `docker compose -f docker-compose.prod.yml logs -f` |
| Rebuild | `docker compose up -d --build` | `docker compose -f docker-compose.prod.yml up -d --build` |
| Stop | `docker compose down` | `docker compose -f docker-compose.prod.yml down` |

### Access URL mapping

| Service | Windows local | Linux production |
|------|-------------|-----------|
| User frontend | http://localhost:8080 | https://your-domain.com/ |
| Admin console | http://localhost:8081 | https://your-domain.com/admin/ |
| Backend API | http://localhost:5555 | https://your-domain.com/api/ |
| Prometheus | http://localhost:9090 | http://your-server:9090 |

---

## From local validation to production deployment

### Deploy to Linux after validation passes

1. **Make sure local validation succeeded**:
   ```powershell
   # Run the full test suite
   .\scripts\backend\test-oauth2-endpoints.ps1
   .\scripts\backend\test-admin-endpoints.ps1
   ```

2. **Commit the code**:
   ```powershell
   git add .
   git commit -m "feat: XXX (tested on Windows Docker Desktop)"
   git push
   ```

3. **Deploy on the Linux server**:
   ```bash
   # Pull the code
   git pull

   # Copy the environment variables (one-time only)
   cp deploy/env/docker.env.example .env.docker
   # Edit .env.docker to set production passwords

   # Start with the production configuration
   docker compose -f deploy/docker/docker-compose.prod.yml --env-file .env.docker up -d --build
   ```

4. **Configure TLS** (the only extra step on Linux):
   ```bash
   # Use Let's Encrypt
   sudo certbot certonly --standalone -d your-domain.com
   cp /etc/letsencrypt/live/your-domain.com/fullchain.pem deploy/nginx/ssl/
   cp /etc/letsencrypt/live/your-domain.com/privkey.pem deploy/nginx/ssl/
   docker compose -f deploy/docker/docker-compose.prod.yml restart nginx
   ```

---

## Performance comparison

| Metric | Windows Docker Desktop | Linux production server |
|------|----------------------|----------------|
| Startup time | ~45 s (6 containers) | ~30 s (same configuration) |
| Memory footprint | ~2.5 GB | ~1.8 GB |
| API response time | ~50ms | ~40ms |
| Database query | ~10ms | ~8ms |

> The differences mainly come from Windows system overhead and the WSL2 virtualization layer, but functionality is fully identical.

---

## Next steps

1. **Finish local validation**: make sure all core functionality works
2. **Record test results**: mark "Verified on Windows Docker Desktop" in the project documentation
3. **Push to Linux**: succeed with a one-shot deployment
4. **Configure monitoring**: set up Prometheus + Grafana

---

## Appendix: complete port mapping

```
┌─────────────────────────────────────────────────────────────┐
│                       Windows host                           │
├─────────────────────────────────────────────────────────────┤
│  Port 8080 ──→ fulla-frontend:80 (Vue user frontend)        │
│  Port 8081 ──→ fulla-admin:80 (Vue admin console)           │
│  Port 5555 ──→ fulla-backend:5555 (C++ API)                 │
│  Port 5433 ──→ fulla-postgres:5432 (PostgreSQL)             │
│  Port 6380 ──→ fulla-redis:6379 (Redis)                     │
│  Port 9090 ──→ fulla-prometheus:9090 (monitoring)           │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│             Docker internal network (oauth2-net)             │
│                                                              │
│  All containers reach each other via internal DNS:          │
│  - fulla-backend → fulla-postgres:5432                       │
│  - fulla-backend → fulla-redis:6379                          │
│  - fulla-frontend → fulla-backend:5555                       │
│  - fulla-admin → fulla-backend:5555                          │
└─────────────────────────────────────────────────────────────┘
```

---

## Summary

✓ **Feasible**: Windows Docker Desktop can validate the entire deployment workflow (except domain and SSL)  
✓ **Recommended**: validate locally → push code → deploy on Linux; dramatically reduces debugging time  
✓ **Consistency**: database schema, API surface, and frontend logic are 100% identical to production  

**Suitable scenarios**:
- ✓ Verifying code changes
- ✓ Testing database migrations
- ✓ Debugging API endpoints
- ✓ Verifying frontend routing
- ✓ Testing OAuth2 flows

**Unsuitable scenarios**:
- ✗ TLS/SSL testing (self-signed certificates can partially substitute)
- ✗ Performance/load testing (use a Linux server)
- ✗ High-availability configurations (multiple servers required)
