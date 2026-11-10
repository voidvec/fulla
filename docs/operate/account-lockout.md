# Account Lockout Mechanism

## Problem Description

The OAuth2 system implements an account lockout mechanism to defend against brute-force attacks. After repeated failed logins, an account is temporarily locked.

## Lockout Rules

Per the implementation in `AuthService.cc`, the lockout rules are:

| Failed attempts | Lockout duration |
|---------|---------|
| 5-9    | 1 minute  |
| 10-14  | 5 minutes |
| 15-19  | 30 minutes|
| 20+    | 1 hour    |

## Common Scenarios

### Scenario 1: lockout caused by repeated test-script runs

**Symptoms**:
- The first run of the test script succeeds
- The second run fails all tests
- Backend logs show: `Account locked for user: admin until 1779441748`

**Cause**:
A test case in the script failed to log in (e.g. wrong credentials), accumulating failed attempts up to the threshold.

**Solution**:
The test script now automatically resets the account lockout state at the end. If the problem persists, reset manually.

## Manually Resetting Account Lockout

### Method 1: use the reset script (recommended)

#### Local PostgreSQL database

```powershell
# 默认使用config.json中的配置（fulla_user/fulla_db/123456）
.\scripts\backend\reset-account-lockout.ps1

# 重置特定用户
.\scripts\backend\reset-account-lockout.ps1 -Username admin

# 自定义数据库连接
.\scripts\backend\reset-account-lockout.ps1 -DbHost localhost -DbUser fulla_user -DbPassword 123456
```

#### Docker database

```powershell
# 脚本会自动检测Docker容器
.\scripts\backend\reset-account-lockout.ps1

# 重置特定用户
.\scripts\backend\reset-account-lockout.ps1 -Username admin
```

### Method 2: reset the admin password

If the admin password was changed accidentally, or login fails after the upgrade to PBKDF2, use this script to reset it to the default password:

```powershell
# 重置admin密码为默认值 'admin'
.\scripts\backend\reset-admin-password.ps1
```

The script writes the development password `admin` directly in the current
`$pbkdf2-sha256$310000$...` format and clears the legacy salt and lockout
fields. It does not write a SHA-256 password or rely on an automatic first-login
upgrade. Legacy hashes are rejected by default; a separately configured
migration window is not the recovery procedure.

Use this default-password reset only in an isolated development/test database.
The script automatically selects the first running PostgreSQL-named Docker
container when available; verify the selected database before proceeding.
Production recovery must use an approved privileged process and a new secret,
not these public development credentials. Restarting the server does not reset
an existing admin password.

### Method 3: direct SQL

The commands below clear lockout only; they do not change the password.
For the development password reset, use Method 2. Do not paste a PBKDF2 digest
into a shell-expanded SQL string: `$` is part of the stored format and must
remain literal.

#### Local PostgreSQL

```powershell
# Windows PowerShell - 重置锁定状态
$env:PGPASSWORD = "123456"
psql -U fulla_user -d fulla_db -h localhost -c "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username='admin';"
$env:PGPASSWORD = $null

```

```bash
# Linux/Mac - 重置锁定状态
PGPASSWORD=123456 psql -U fulla_user -d fulla_db -h localhost -c "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username='admin';"

```

#### Docker database

```bash
# 重置锁定状态
docker exec <container_name> psql -U fulla_user -d fulla_db -c "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username='admin';"

```

### Method 4: inspect lockout state

```sql
-- 查看所有用户的锁定状态
SELECT 
    username, 
    failed_login_count, 
    locked_until,
    CASE 
        WHEN locked_until > EXTRACT(EPOCH FROM NOW()) THEN 'LOCKED'
        ELSE 'UNLOCKED'
    END as status,
    CASE 
        WHEN locked_until > EXTRACT(EPOCH FROM NOW()) 
        THEN TO_TIMESTAMP(locked_until) - NOW()
        ELSE INTERVAL '0'
    END as remaining_time
FROM users
ORDER BY username;
```

## Test Script Auto-Cleanup

`test-admin-endpoints.ps1` already resets the admin account's lockout state when tests finish:

```powershell
# 测试脚本会在结束时执行：
# 1. 尝试连接Docker容器
# 2. 如果没有Docker，尝试连接本地PostgreSQL
# 3. 重置admin账号的 failed_login_count 和 locked_until
```

**Note**: when using a local PostgreSQL, configure the database password in the script:

```powershell
# 编辑 test-admin-endpoints.ps1，找到这一行：
$env:PGPASSWORD = "your_password"  # 修改为你的数据库密码
```

## Preventive Measures

### 1. Use a dedicated account for testing

Do not use the production admin account in tests. Create a dedicated test account:

```sql
INSERT INTO users (username, password_hash, salt, email, email_verified)
VALUES ('test_admin', '<hash>', '', 'test@example.com', true);

INSERT INTO user_roles (user_id, role_id)
SELECT u.id, r.id FROM users u, roles r 
WHERE u.username = 'test_admin' AND r.name = 'admin';
```

### 2. Automatic cleanup after tests

Add cleanup code at the end of every test script:

```powershell
# Cleanup
try {
    # 重置测试账号
    psql -U fulla_user -d fulla_db -h localhost -c "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username='test_admin';"
} catch {
    Write-Host "Warning: Failed to reset test account" -ForegroundColor Yellow
}
```

### 3. Use correct credentials

Make sure the usernames and passwords used in test scripts match the database:

```powershell
# 检查数据库中的用户
psql -U fulla_user -d fulla_db -h localhost -c "SELECT username FROM users;"

# 如果需要重置密码（使用PBKDF2）
# 需要通过应用程序的注册接口或直接调用PasswordHasher
```

## Production Recommendations

### 1. Monitor lockout events

Monitor account lockout events in production:

```sql
-- 查找最近被锁定的账号
SELECT 
    username, 
    failed_login_count,
    TO_TIMESTAMP(locked_until) as locked_until_time,
    TO_TIMESTAMP(last_failed_login) as last_failed_time
FROM users
WHERE locked_until > EXTRACT(EPOCH FROM NOW())
ORDER BY locked_until DESC;
```

### 2. Set up alerts

Raise an alert when critical accounts (such as admin) get locked:

```sql
-- 可以通过定时任务检查
SELECT COUNT(*) FROM users 
WHERE username IN ('admin', 'superuser') 
AND locked_until > EXTRACT(EPOCH FROM NOW());
```

### 3. Audit logs

The backend logs every lockout event:

```
WARN  Account locked for user: admin until 1779441748
INFO  [METRIC] oauth2_login_failures_total reason=bad_credentials
```

Consider shipping these logs to a centralized logging system (e.g. ELK, Grafana Loki) for analysis.

## Security Considerations

1. **Do not disable the lockout mechanism**: it is an important defense against brute-force attacks
2. **Do not hardcode database passwords in code**: use environment variables or a secret management system
3. **Restrict reset permissions**: only administrators should be able to reset account lockout state
4. **Record reset operations**: in production, every reset operation should be audited

## Related Files

- `libs/drogon/src/AuthService.cc` - account lockout logic implementation
- `scripts/backend/test-admin-endpoints.ps1` - test script (with auto-cleanup)
- `scripts/backend/reset-account-lockout.ps1` - manual reset script
- Database table: `users` (columns: `failed_login_count`, `locked_until`, `last_failed_login`)
