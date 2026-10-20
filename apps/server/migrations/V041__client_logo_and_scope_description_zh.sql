-- V041 (P1-1, #279): branding consumption chain.
--
-- oauth2_clients.logo_uri: client branding for the login/consent screens
-- (authorize-context / consent-context extensions). Write paths (admin
-- client edit, portal My Applications) validate the scheme in the same
-- PR — https-only + loopback exemption, NO userinfo/fragment, and it does
-- NOT inherit the allow_http_redirect_uri exemption (a brand image is not
-- a redirect target; see BrandValidator).
--
-- oauth2_scopes.description_zh: the locale dimension of consent-screen
-- scope descriptions. The server picks description vs description_zh by
-- the page locale; the frontend dictionary remains only as a missing-key
-- fallback. Idempotent backfill for the seeded scope set (UPDATE only —
-- M4 forbids top-level DELETE FROM; ON CONFLICT DO NOTHING on the ALTER
-- path is not needed since ALTER TABLE ADD COLUMN IF NOT EXISTS guards).

ALTER TABLE oauth2_clients ADD COLUMN IF NOT EXISTS logo_uri VARCHAR(512);

ALTER TABLE oauth2_scopes ADD COLUMN IF NOT EXISTS description_zh VARCHAR(512);

-- zh-CN backfill for the seeded scopes (guarded: only fills NULL rows, so
-- admin-edited descriptions survive replay).
UPDATE oauth2_scopes SET description_zh = CASE name
    WHEN 'openid' THEN 'OpenID Connect 身份验证'
    WHEN 'profile' THEN '访问用户基本信息（用户名、邮箱）'
    WHEN 'email' THEN '访问用户邮箱地址'
    WHEN 'admin' THEN '管理员权限'
    WHEN 'read' THEN '只读访问'
    WHEN 'write' THEN '写入权限'
    WHEN 'org' THEN '组织成员信息与组织管理'
    WHEN 'audit:read' THEN '读取审计日志'
    WHEN 'clients:read' THEN '查看 OAuth2 客户端'
    WHEN 'clients:write' THEN '创建与修改 OAuth2 客户端'
    WHEN 'roles:read' THEN '读取角色与作用域目录'
    WHEN 'roles:write' THEN '创建与修改角色和作用域'
    WHEN 'tokens:read' THEN '查看令牌'
    WHEN 'tokens:write' THEN '吊销与管理令牌'
    WHEN 'users:read' THEN '读取用户账号与角色'
    WHEN 'users:write' THEN '创建与修改用户'
    ELSE NULL
END
WHERE description_zh IS NULL
  AND name IN ('openid', 'profile', 'email', 'admin', 'read', 'write', 'org',
               'audit:read', 'clients:read', 'clients:write', 'roles:read',
               'roles:write', 'tokens:read', 'tokens:write', 'users:read', 'users:write');
