# Deployment Verification Checklist

This document provides complete deployment verification procedures to ensure the fulla full-stack system runs correctly on Windows Docker Desktop or in a Linux production environment.

---

## Quick Verification (5 Minutes)

### 1. Check All Container Statuses

```powershell
# Windows
docker compose -f deploy/docker/docker-compose.yml ps

# Linux
docker compose -f deploy/docker/docker-compose.prod.yml --env-file .env.docker ps
```

**Expected result**: all containers show a status of `Up` or `Up (healthy)`

| Container | Status | Port Mapping |
|--------|------|---------|
| fulla-frontend | Up | 8080:80 |
| fulla-admin | Up | 8081:80 |
| fulla-backend | Up (healthy) | 5555:5555 |
| fulla-postgres | Up (healthy) | 5433:5432 |
| fulla-redis | Up | 6380:6379 |
| fulla-prometheus | Up | 9090:9090 |

### 2. Health Check

```powershell
# Backend health endpoint
curl http://localhost:5555/health

# Expected output
{"status":"healthy","timestamp":"2026-08-26T10:30:00Z"}
```

### 3. Database Connection Test

```powershell
# Enter the postgres container
docker exec -it fulla-postgres psql -U fulla_user -d fulla_db -c "\dt"

# Expected output: list of OAuth2-related tables
# oauth2_clients, oauth2_codes, oauth2_access_tokens, oauth2_refresh_tokens,
# oauth2_scopes, users, roles, user_roles, organizations, audit_logs, etc. (21 tables in total after V026)
```

### 4. Frontend Access Test

Open the following in a browser:
- **User frontend**: http://localhost:8080 or https://your-domain.com
- **Admin console**: http://localhost:8081 or https://your-domain.com/admin

**Expected result**: pages load normally with no 404 or 502 errors

---

## Full Verification (30 Minutes)

## Phase 1: Infrastructure Verification

### 1.1 PostgreSQL Verification

```powershell
# Connection test
docker exec fulla-postgres pg_isready -U fulla_user

# Expected output: /var/run/postgresql:5432 - accepting connections

# Table structure check
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "
SELECT table_name 
FROM information_schema.tables 
WHERE table_schema = 'public' 
ORDER BY table_name;
"

# Expected table list (V002-V026 actual schema, all with the oauth2_ prefix):
# - oauth2_access_tokens, oauth2_refresh_tokens, oauth2_codes
# - oauth2_clients, oauth2_scopes, oauth2_client_scopes
# - oauth2_user_consents, oauth2_subject_mappings, oauth2_device_codes
# - users, roles, permissions, user_roles, role_permissions
# - organizations, audit_logs, webauthn_credentials, etc.

# Database version check
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "SELECT version();"

# Expected: PostgreSQL 17.x (deploy compose defaults to postgres:17-alpine;
#           existing deployments explicitly pinned to 15 should show 15.x here, see docs/operate/postgresql-major-upgrade.md)
```

### 1.2 Redis Verification

```powershell
# Enter the redis container
docker exec -it fulla-redis redis-cli -a redis_secret_pass ping

# Expected output: PONG

# Test read/write
docker exec fulla-redis redis-cli -a redis_secret_pass SET test_key "hello"
docker exec fulla-redis redis-cli -a redis_secret_pass GET test_key

# Expected output: "hello"

# Check memory usage
docker exec fulla-redis redis-cli -a redis_secret_pass INFO memory

# Expected: used_memory_human shows a reasonable amount of memory usage
```

### 1.3 Network Connectivity Verification

```powershell
# Test database connectivity from the backend container
docker exec fulla-backend ping -c 3 fulla-postgres

# Expected: 3 packets transmitted, 3 received, 0% packet loss

# Test Redis connectivity from the backend container
docker exec fulla-backend ping -c 3 fulla-redis

# Expected: 3 packets transmitted, 3 received, 0% packet loss

# Check DNS resolution
docker exec fulla-backend nslookup fulla-postgres

# Expected: returns the container IP address of fulla-postgres (e.g., 172.x.x.x)
```

---

## Phase 2: Database Initialization Verification

### 2.1 Check Seed Data

```powershell
# Check the admin user (roles are linked through user_roles; the users table itself has no role column)
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "
SELECT u.username, u.email, r.name AS role, u.created_at
FROM users u
LEFT JOIN user_roles ur ON ur.user_id = u.id
LEFT JOIN roles r ON r.id = ur.role_id
WHERE u.username = 'admin';
"

# Expected output:
# username |       email       | role  |         created_at
# ----------+-------------------+-------+----------------------------
# admin    | admin@example.com | admin | 2026-xx-xx xx:xx:xx

# Check the default clients (the table name carries the oauth2_ prefix; the name column is name)
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "
SELECT client_id, name, client_type, token_endpoint_auth_method
FROM oauth2_clients
WHERE client_id IN ('fulla-admin-console', 'fulla-portal');
"

# Expected output: both fulla-admin-console and fulla-portal are PUBLIC (token_endpoint_auth_method = none)

# Check the default scopes (the scope name column is name)
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "
SELECT name, description
FROM oauth2_scopes
LIMIT 5;
"

# Expected output: standard scopes such as openid, profile, email, admin
```

### 2.2 Verify Database Migrations

```powershell
# Check the migrations table (if present)
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "\d schema_migrations"

# Or check table structure integrity
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "
SELECT COUNT(*) AS table_count 
FROM information_schema.tables 
WHERE table_schema = 'public' AND table_type = 'BASE TABLE';
"

# Expected: table_count >= 15 (measured 21 public tables after V026)
```

---

## Phase 3: Backend API Verification

### 3.1 Obtain an Admin Token

`fulla-admin-console` is a **PUBLIC client** (`token_endpoint_auth_method=none`, no client_secret, password grant not supported). Tokens must go through the two-step **authorization code + PKCE** flow (F-011: PKCE is mandatory for PUBLIC clients). The following is equivalent to the setup step in `scripts/backend/test-admin-endpoints.sh`:

```bash
# 1) Log in to obtain an authorization code (form-encoded; code_challenge = BASE64URL(SHA256(code_verifier)))
CODE_VERIFIER=$(head -c 32 /dev/urandom | basenc --base64url | tr -d '=' | tr -d '+/' | head -c 43)
CODE_CHALLENGE=$(printf '%s' "$CODE_VERIFIER" | openssl dgst -sha256 -binary | basenc --base64url | tr -d '=')

LOGIN_RESP=$(curl -s -X POST http://localhost:5555/oauth2/login \
  -d "username=admin&password=admin" \
  -d "client_id=fulla-admin-console&redirect_uri=http://localhost:5174/admin/callback" \
  -d "scope=openid+profile+admin&state=verify-state" \
  -d "code_challenge=$CODE_CHALLENGE&code_challenge_method=S256&json=true")
CODE=$(echo "$LOGIN_RESP" | jq -r '.code')

# 2) Exchange the authorization code for tokens (form-encoded; a PUBLIC client sends only client_id and must not include any secret)
curl -s -X POST http://localhost:5555/oauth2/token \
  -d "grant_type=authorization_code&code=$CODE" \
  -d "redirect_uri=http://localhost:5174/admin/callback" \
  -d "client_id=fulla-admin-console&code_verifier=$CODE_VERIFIER"

# Expected response (save the access_token):
{
  "access_token": "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9...",
  "token_type": "Bearer",
  "expires_in": 3600,
  "refresh_token": "tGzv3JH7xN1yQ9X2...",
  "scope": "openid profile admin"
}

# Set an environment variable (used by the tests below)
export TOKEN="eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9..."
```

> On Windows, use the repository-provided `scripts/backend/test-admin-endpoints.ps1` to run the same login + token flow.

### 3.2 Verify Token Introspection

```bash
# Introspect the token (RFC 7662, form-encoded)
curl -s -X POST http://localhost:5555/oauth2/introspect \
  -d "token=$TOKEN" \
  -d "token_type_hint=access_token" \
  -d "client_id=fulla-admin-console"

# Expected response:
{
  "active": true,
  "client_id": "fulla-admin-console",
  "username": "admin",
  "scope": "openid profile admin",
  "exp": 1719123456,
  "iat": 1719119856,
  "sub": "admin",
  "iss": "http://localhost:5555"
}

# Test an invalid token
curl -s -X POST http://localhost:5555/oauth2/introspect \
  -d "token=invalid_token" \
  -d "token_type_hint=access_token" \
  -d "client_id=fulla-admin-console"

# Expected response: {"active": false}
```

### 3.3 Refresh a Token

```bash
# Use the refresh_token to obtain a new access_token (form-encoded;
# a PUBLIC client sends only client_id — including a client_secret would actually be rejected by F-017)
curl -s -X POST http://localhost:5555/oauth2/token \
  -d "grant_type=refresh_token" \
  -d "refresh_token=tGzv3JH7xN1yQ9X2..." \
  -d "client_id=fulla-admin-console"

# Expected response: returns a new access_token and refresh_token
{
  "access_token": "new access token...",
  "token_type": "Bearer",
  "expires_in": 3600,
  "refresh_token": "new refresh token...",
  "scope": "openid profile admin"
}
```

### 3.4 Revoke a Token

```bash
# Revoke the token (RFC 7009, form-encoded; the client authentication method must match
# the registered token_endpoint_auth_method — a PUBLIC client uses only client_id)
curl -s -X POST http://localhost:5555/oauth2/revoke \
  -d "token=$TOKEN" \
  -d "token_type_hint=access_token" \
  -d "client_id=fulla-admin-console"

# Expected response: HTTP 200 OK (empty response body)

# Verify the token has been revoked
curl -s -X POST http://localhost:5555/oauth2/introspect \
  -d "token=$TOKEN" \
  -d "token_type_hint=access_token" \
  -d "client_id=fulla-admin-console"

# Expected response: {"active": false}
```

---

## Phase 4: Admin Console API Verification

### 4.1 User Management API

```powershell
# Get the user list
curl -X GET http://localhost:5555/api/admin/users \
  -H "Authorization: Bearer $TOKEN"

# Expected response: user list JSON
{
  "users": [
    {
      "user_id": 1,
      "username": "admin",
      "email": "admin@example.com",
      "role": "admin",
      "created_at": "2026-08-26T10:00:00Z",
      "updated_at": "2026-08-26T10:00:00Z"
    }
  ],
  "total": 1,
  "page": 1,
  "per_page": 20
}

# Create a new user
curl -X POST http://localhost:5555/api/admin/users \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "username": "testuser",
    "email": "test@example.com",
    "password": "TestPassword123!",
    "role": "user"
  }'

# Expected response: HTTP 201 Created
{
  "user_id": 2,
  "username": "testuser",
  "email": "test@example.com",
  "role": "user",
  "created_at": "2026-08-26T10:30:00Z"
}

# Get a single user's details
curl -X GET http://localhost:5555/api/admin/users/2 \
  -H "Authorization: Bearer $TOKEN"

# Expected response: shows the details of testuser
```

### 4.2 Client Management API

```powershell
# Get the client list
curl -X GET http://localhost:5555/api/admin/clients \
  -H "Authorization: Bearer $TOKEN"

# Expected response: client list (client secrets are never echoed back; only hashes are stored in the database)
{
  "clients": [
    {
      "client_id": "fulla-admin-console",
      "name": "Admin Console",
      "client_type": "PUBLIC",
      "token_endpoint_auth_method": "none",
      "redirect_uris": ["http://localhost:5174/admin/callback"],
      "allowed_grant_types": ["authorization_code", "refresh_token"],
      "scopes": ["openid", "profile", "admin"]
    }
  ],
  "total": 1
}

# Create a new client
curl -X POST http://localhost:5555/api/admin/clients \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "client_id": "test-client",
    "name": "Test Client",
    "client_type": "CONFIDENTIAL",
    "client_secret": "test-secret",
    "redirect_uris": ["http://localhost:8080/callback"],
    "allowed_grant_types": ["authorization_code", "refresh_token"],
    "scopes": ["openid", "profile", "email"]
  }'

# Expected response: HTTP 201 Created (the response contains the newly created client's metadata; the secret is not echoed back)
```

### 4.3 Scope Management API

```powershell
# Get all scopes
curl -X GET http://localhost:5555/api/admin/scopes \
  -H "Authorization: Bearer $TOKEN"

# Expected response: scope list (the scope name field is name, consistent with the oauth2_scopes table)
{
  "scopes": [
    {"name": "openid", "description": "OpenID Connect"},
    {"name": "profile", "description": "User profile"},
    {"name": "email", "description": "User email"},
    {"name": "admin", "description": "Administrative access"}
  ]
}

# Create a new scope
curl -X POST http://localhost:5555/api/admin/scopes \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "read",
    "description": "Read access to user resources"
  }'

# Expected response: HTTP 201 Created
```

---

## Phase 5: Frontend Feature Verification

### 5.1 User Frontend Verification

| Test Item | Steps | Expected Result |
|--------|---------|---------|
| Visit the home page | Open http://localhost:8080 | The login page is displayed |
| User registration | Fill in the registration form (username, email, password) | Registration succeeds and redirects to the login page |
| User login | Log in with the account just registered | Login succeeds and redirects to the profile page |
| View profile | Click the "Profile" menu | User information is displayed (username, email) |
| Change password | Enter the old and new passwords | The password change succeeds and the user must log in again |
| Log out | Click the "Log out" button | Logout succeeds and redirects to the login page |

### 5.2 Admin Console Verification

| Test Item | Steps | Expected Result |
|--------|---------|---------|
| Open the admin console | Open http://localhost:8081/admin | The admin console login page is displayed |
| Admin login | Log in with admin/admin | Login succeeds and the dashboard is displayed |
| App management | Click the "Apps" menu | The client list is displayed (contains at least fulla-admin-console) |
| Create an app | Click "New App" and fill in the form | The app is created successfully and appears in the list |
| User management | Click the "Users" menu | The user list is displayed (contains at least admin and the newly registered user) |
| Token management | Click the "Token" menu | The list of active tokens is displayed |

### 5.3 OAuth2 Authorization Code Flow Verification

```bash
# Step 1: Build the authorization URL (visit it in a browser)
# Note: the redirect_uri must exactly match the client's registered value
#       (fulla-portal's seed registration uses http://127.0.0.1:8080/callback — using localhost will be rejected)
# http://localhost:5555/oauth2/authorize?
#   response_type=code&
#   client_id=fulla-portal&
#   redirect_uri=http://127.0.0.1:8080/callback&
#   scope=openid profile email&
#   state=random_state_value

# Expected: redirect to the login page

# Step 2: User login
# Log in with a test account (e.g., testuser)

# Expected: the authorization consent page is displayed

# Step 3: User consent
# Click the "Authorize" button

# Expected: redirect to the redirect_uri carrying the authorization code
# http://127.0.0.1:8080/callback?code=xxx&state=random_state_value

# Step 4: Exchange the token (fulla-portal is a PUBLIC client → must include the PKCE code_verifier
#          and must not include a client_secret)
curl -s -X POST http://localhost:5555/oauth2/token \
  -d "grant_type=authorization_code" \
  -d "code=<code obtained from the callback>" \
  -d "redirect_uri=http://127.0.0.1:8080/callback" \
  -d "client_id=fulla-portal" \
  -d "code_verifier=<PKCE verifier used at login>"

# Expected response: returns an access_token and refresh_token
{
  "access_token": "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9...",
  "token_type": "Bearer",
  "expires_in": 3600,
  "refresh_token": "xxx",
  "scope": "openid profile email"
}
```

---

## Phase 5 Supplement: Email Service Verification

The email service has two modes, selected by the backend's `getEmailService()` based on the `FULLA_SMTP_*` environment variables.

### 5.4 Confirm the Email Service Mode

```powershell
# Check the email service mode in the backend startup logs
docker logs fulla-backend 2>&1 | grep -i "Email service"
```

**Expected output (one of the following)**:

- Console mode (SMTP not configured): `Email service: Console (set FULLA_SMTP_* env vars to enable SMTP)`
- SMTP mode (configured): `Email service: SMTP (smtp.163.com:465)`

### 5.5 Email Address Verification Message

**Steps**: log in to the user frontend → Profile page → click "Send Email Verification"

| Mode | Verification Method |
|------|---------|
| Console mode | The message content is written to the backend logs; copy the verification link from there |
| SMTP mode | A real message should arrive in the inbox |

**To view the verification link in Console mode**:

```powershell
docker logs fulla-backend --tail 50 2>&1 | grep -A 5 -iE "verify|email"
# Expected: a link containing "verify-email?token=xxx"
```

**To verify message delivery in SMTP mode**:

```powershell
# After triggering the send, check the backend for SMTP errors
docker logs fulla-backend --tail 50 2>&1 | grep -iE "smtp|email|curl"
# Expected: no ERROR-level logs; a "Verify Your Email" message arrives in the inbox
```

### 5.6 Password Reset Message

**Steps**: frontend "Forgot Password" page → enter the email address → submit

- **Expected response** (anti-enumeration): regardless of whether the email address exists, the same message is returned: `If the email exists, a reset link has been sent`
- In Console mode, the reset link is likewise written to the backend logs

### 5.7 Enable Real SMTP (Optional; see the deployment guide for details)

For real message delivery, set `FULLA_SMTP_HOST` / `FULLA_SMTP_USER` / `FULLA_SMTP_PASSWORD` in `.env.docker` and restart the backend:

```powershell
docker compose -f deploy/docker/docker-compose.yml --env-file .env.docker up -d fulla-backend
docker logs fulla-backend 2>&1 | grep -i "Email service"
# Expected: Email service: SMTP (...)
```

> **Note**: email verification links use `FULLA_FRONTEND_URL`. In a local deployment this is `http://localhost:8080`; clicking the link from another machine will not work.

---

## Phase 6: Security Verification

### 6.1 Error Response Verification

```bash
# Test an invalid client ID (token endpoint, form-encoded)
curl -s -X POST http://localhost:5555/oauth2/token \
  -d "grant_type=authorization_code&code=x" \
  -d "client_id=invalid-client&code_verifier=x"

# Expected response: HTTP 401 Unauthorized
{
  "error": "invalid_client",
  "error_description": "Client authentication failed"
}

# Test a wrong password (login endpoint — note: failed attempts trigger F-018 rate limiting, so do not exceed the threshold with repeated attempts)
curl -s -X POST http://localhost:5555/oauth2/login \
  -d "username=admin&password=wrong-password" \
  -d "client_id=fulla-admin-console&redirect_uri=http://localhost:5174/admin/callback" \
  -d "scope=openid&state=t&code_challenge=x&code_challenge_method=S256"

# Expected response: HTTP 401 Unauthorized (error codes go through the ErrorCatalog, ensuring a unified anti-enumeration posture)
{
  "error": "invalid_grant",
  "error_description": "Invalid username or password"
}

# Test a missing required parameter
curl -s -X POST http://localhost:5555/oauth2/token \
  -d "grant_type=authorization_code"

# Expected response: HTTP 400 Bad Request
{
  "error": "invalid_request",
  "error_description": "Missing required parameter: client_id"
}
```

### 6.2 Token Expiration Verification

```powershell
# Wait for the token to expire (3600 seconds), or change the backend configuration to a shorter expiration time for testing

# Or use an already-revoked token
curl -X GET http://localhost:5555/api/admin/users \
  -H "Authorization: Bearer revoked_token"

# Expected response: HTTP 401 Unauthorized
{
  "error": "invalid_token",
  "error_description": "The access token expired or has been revoked"
}
```

### 6.3 Scope Authorization Verification

```powershell
# Request a resource beyond the granted scope (if scope-based access control is implemented)
curl -X GET http://localhost:5555/api/admin/users \
  -H "Authorization: Bearer $token_with_limited_scope"

# Expected response: HTTP 403 Forbidden
{
  "error": "insufficient_scope",
  "error_description": "The request requires higher privileges than provided by the access token"
}
```

---

## Phase 7: Performance and Monitoring Verification

### 7.1 Prometheus Metrics Verification

```powershell
# Access the Prometheus UI
# Open in a browser: http://localhost:9090

# Example metric queries (the authoritative list is in docs/operate/observability.md):
# - oauth2_requests_total: total number of requests (by endpoint/status dimensions)
# - oauth2_latency_seconds: latency histogram for key steps
# - oauth2_active_tokens: current number of active tokens
# - oauth2_login_failures_total: number of login failures

# Expected: metrics are collected normally and data is present
```

### 7.2 Log Verification

```powershell
# View backend logs
docker logs fulla-backend --tail 50

# Expected: no ERROR-level logs, normal INFO/DEBUG logs

# View nginx logs (Linux production environment)
docker logs oauth2-nginx --tail 50

# Expected: normal access logs, no 5xx errors

# Follow logs in real time
docker compose -f deploy/docker/docker-compose.yml logs -f fulla-backend
```

### 7.3 Database Performance Verification

```powershell
# Check the number of database connections
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "
SELECT count(*) AS connections 
FROM pg_stat_activity 
WHERE datname = 'fulla_db';
"

# Expected: connections is a reasonable value (usually < 20)

# Check slow queries (if the pg_stat_statements extension is available)
docker exec fulla-postgres psql -U fulla_user -d fulla_db -c "
SELECT query, calls, total_time, mean_time 
FROM pg_stat_statements 
ORDER BY mean_time DESC 
LIMIT 5;
"

# Expected: no significant slow queries (mean_time < 100ms)
```

---

## Troubleshooting Checkpoints

### Problem 1: Containers fail to start

**Check steps**:

```powershell
# View container status
docker compose -f deploy/docker/docker-compose.yml ps

# View the failed container's logs
docker logs fulla-backend

# Check resource usage
docker stats

# Validate the configuration file
docker compose -f deploy/docker/docker-compose.yml config
```

### Problem 2: Database connection failures

**Check steps**:

```powershell
# Verify the postgres container health status
docker exec fulla-postgres pg_isready -U fulla_user

# Check network connectivity
docker exec fulla-backend ping fulla-postgres

# Verify environment variables
docker exec fulla-backend env | grep FULLA_DB

# View database logs
docker logs fulla-postgres
```

### Problem 3: Frontend cannot reach the backend API

**Check steps**:

```powershell
# Test the backend connection from the frontend container
docker exec fulla-frontend curl -s http://fulla-backend:5555/health

# Check the nginx configuration (production environment)
docker exec oauth2-nginx nginx -t

# View the backend CORS configuration
docker logs fulla-backend | grep -i cors
```

### Problem 4: Token verification failures

**Check steps**:

```powershell
# Verify that the JWT keys exist
docker exec fulla-backend ls -la /app/keys/

# Check the token signature
# Copy the access_token to https://jwt.io to decode and verify it

# View authentication errors in the backend logs
docker logs fulla-backend | grep -i "auth\|token"
```

---

## Automated Verification Script

### Full Verification Script (PowerShell)

The script ships in the repository:
[`scripts/backend/verify-deployment.ps1`](https://github.com/voidvec/fulla/blob/master/scripts/backend/verify-deployment.ps1).
Run it from the repo root — it executes the eight checks below (container
status, backend health, DB connection, table completeness, seed admin,
Redis, OIDC discovery, frontend access) and exits non-zero on any failure.

**Usage**:

```powershell
# Basic verification
.\scripts\backend\verify-deployment.ps1

# Custom endpoints
.\scripts\backend\verify-deployment.ps1 -BackendUrl "https://your-domain.com" -FrontendUrl "https://your-domain.com"
```

---

## Verification Report Template

After completing verification, fill in the following report template:

```markdown
## fulla Deployment Verification Report

**Verification date**: YYYY-MM-DD
**Verification environment**: Windows Docker Desktop / Linux production server
**Verified by**: [Name]

### Verification Results Summary

| Phase | Status | Notes |
|------|------|------|
| Infrastructure verification | Passed | All containers running normally |
| Database initialization verification | Passed | 21 tables (V026), admin account created |
| Backend API verification | Passed | Token endpoint, introspection, and revocation working normally |
| Admin console API verification | Passed | User, client, and scope management working normally |
| Frontend feature verification | Passed | User login, registration, and profile features working normally |
| Security verification | Passed | Error handling and token verification working normally |
| Performance and monitoring verification | Partially passed | Prometheus normal; slow queries need optimization |

### Issues Found

1. **Issue description**: [specific issue]
   - **Impact scope**: [affected functionality]
   - **Resolution**: [how it was resolved]
   - **Status**: [Open | Resolved]

### Optimization Suggestions

1. [Suggestion 1]
2. [Suggestion 2]

### Next Actions

- [ ] Deploy to production
- [ ] Configure Let's Encrypt certificates
- [ ] Set up monitoring alerts
- [ ] Run performance load tests

### Sign-off

Verified by: __________  Date: __________
Reviewed by: __________  Date: __________
```

---

## Summary

This verification checklist covers all core functionality of the fulla system:

- **Infrastructure**: Docker containers, networking, storage volumes
- **Data layer**: PostgreSQL database, Redis cache
- **Business layer**: OAuth2 core flows, admin console APIs
- **Presentation layer**: Vue.js user frontend, admin console
- **Security**: authentication, authorization, token management
- **Observability**: logs, metrics, health checks

**Pass criteria**:
- All containers have a status of `Up`
- Backend health check passes
- Database table structure is complete (21 public tables in total after V026)
- The admin account is usable
- OAuth2 core flows (authorization, tokens, introspection, revocation) work correctly
- Frontend pages are accessible and basic operations complete successfully

Once this checklist has been fully verified, the system is ready for production use.
