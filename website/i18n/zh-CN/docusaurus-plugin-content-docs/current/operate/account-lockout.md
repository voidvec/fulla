# 账号锁定机制说明

## 问题描述

OAuth2 系统实现了账号锁定机制以防止暴力破解攻击。当用户多次登录失败时，账号会被临时锁定。

## 锁定规则

根据 `AuthService.cc` 中的实现，锁定规则如下：

| 失败次数 | 锁定时长 |
|---------|---------|
| 5-9次   | 1分钟   |
| 10-14次 | 5分钟   |
| 15-19次 | 30分钟  |
| 20次以上 | 1小时   |

## 常见场景

### 场景1：测试脚本重复执行导致锁定

**症状**：
- 第一次运行测试脚本成功
- 第二次运行时所有测试失败
- 后端日志显示：`Account locked for user: admin until 1779441748`

**原因**：
测试脚本中某个测试用例登录失败（例如使用了错误的凭证），累积失败次数达到阈值。

**解决方案**：
测试脚本已自动在结束时重置账号锁定状态。如果仍然遇到问题，可以手动重置。

## 手动重置账号锁定

### 方法1：使用重置脚本（推荐）

#### 本地PostgreSQL数据库

```powershell
# 默认使用config.json中的配置（fulla_user/fulla_db/123456）
.\scripts\backend\reset-account-lockout.ps1

# 重置特定用户
.\scripts\backend\reset-account-lockout.ps1 -Username admin

# 自定义数据库连接
.\scripts\backend\reset-account-lockout.ps1 -DbHost localhost -DbUser fulla_user -DbPassword 123456
```

#### Docker数据库

```powershell
# 脚本会自动检测Docker容器
.\scripts\backend\reset-account-lockout.ps1

# 重置特定用户
.\scripts\backend\reset-account-lockout.ps1 -Username admin
```

### 方法2：重置admin密码

如果admin账号密码被意外修改或升级为PBKDF2后无法登录，使用此脚本重置为默认密码：

```powershell
# 重置admin密码为默认值 'admin'
.\scripts\backend\reset-admin-password.ps1
```

脚本直接把开发密码 `admin` 写成当前 `$pbkdf2-sha256$310000$...` 格式，并清空
旧 salt 与锁定字段。它不会写入 SHA-256 密码，也不依赖首次登录自动升级。旧哈希
默认被拒绝；单独配置的迁移窗口不是此处的恢复流程。

只在隔离的开发/测试数据库使用这种默认密码重置。脚本在存在匹配容器时会自动选取
第一个正在运行、名称含 PostgreSQL 的 Docker 容器，执行前必须核对所选数据库。
生产恢复应走获准的特权流程并使用新的秘密，不能套用这些公开开发凭据。重启服务
也不会重置已有管理员密码。

### 方法3：直接使用SQL

下面命令只清除锁定，不修改密码。开发密码重置使用方法 2。不要把 PBKDF2 摘要
嵌入会被 shell 展开的 SQL 字符串，`$` 是存储格式的一部分，必须保持字面值。

#### 本地PostgreSQL

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

#### Docker数据库

```bash
# 重置锁定状态
docker exec <container_name> psql -U fulla_user -d fulla_db -c "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username='admin';"

```

### 方法4：查看锁定状态

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

## 测试脚本自动清理

`test-admin-endpoints.ps1` 已经在测试结束时自动重置admin账号的锁定状态：

```powershell
# 测试脚本会在结束时执行：
# 1. 尝试连接Docker容器
# 2. 如果没有Docker，尝试连接本地PostgreSQL
# 3. 重置admin账号的 failed_login_count 和 locked_until
```

**注意**：如果使用本地PostgreSQL，需要在脚本中配置数据库密码：

```powershell
# 编辑 test-admin-endpoints.ps1，找到这一行：
$env:PGPASSWORD = "your_password"  # 修改为你的数据库密码
```

## 预防措施

### 1. 测试环境使用专用账号

不要在测试中使用生产环境的admin账号。创建专门的测试账号：

```sql
INSERT INTO users (username, password_hash, salt, email, email_verified)
VALUES ('test_admin', '<hash>', '', 'test@example.com', true);

INSERT INTO user_roles (user_id, role_id)
SELECT u.id, r.id FROM users u, roles r 
WHERE u.username = 'test_admin' AND r.name = 'admin';
```

### 2. 测试后自动清理

在所有测试脚本的末尾添加清理代码：

```powershell
# Cleanup
try {
    # 重置测试账号
    psql -U fulla_user -d fulla_db -h localhost -c "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username='test_admin';"
} catch {
    Write-Host "Warning: Failed to reset test account" -ForegroundColor Yellow
}
```

### 3. 使用正确的凭证

确保测试脚本中使用的用户名和密码与数据库中的一致：

```powershell
# 检查数据库中的用户
psql -U fulla_user -d fulla_db -h localhost -c "SELECT username FROM users;"

# 如果需要重置密码（使用PBKDF2）
# 需要通过应用程序的注册接口或直接调用PasswordHasher
```

## 生产环境建议

### 1. 监控锁定事件

在生产环境中监控账号锁定事件：

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

### 2. 设置告警

当关键账号（如admin）被锁定时发送告警：

```sql
-- 可以通过定时任务检查
SELECT COUNT(*) FROM users 
WHERE username IN ('admin', 'superuser') 
AND locked_until > EXTRACT(EPOCH FROM NOW());
```

### 3. 审计日志

后端日志会记录所有锁定事件：

```
WARN  Account locked for user: admin until 1779441748
INFO  [METRIC] oauth2_login_failures_total reason=bad_credentials
```

建议将这些日志发送到集中式日志系统（如ELK、Grafana Loki）进行分析。

## 安全注意事项

1. **不要禁用锁定机制**：这是防止暴力破解的重要安全措施
2. **不要在代码中硬编码数据库密码**：使用环境变量或密钥管理系统
3. **限制重置权限**：只有管理员应该能够重置账号锁定状态
4. **记录重置操作**：在生产环境中，所有重置操作都应该被审计

## 相关文件

- `libs/drogon/src/AuthService.cc` - 账号锁定逻辑实现
- `scripts/backend/test-admin-endpoints.ps1` - 测试脚本（含自动清理）
- `scripts/backend/reset-account-lockout.ps1` - 手动重置脚本
- 数据库表：`users` (字段: `failed_login_count`, `locked_until`, `last_failed_login`)
