-- V045 (P1-8, #285): RBAC enforcement foundation.
--
-- Backfills the permissions catalog with the V023 resource-scope names +
-- the admin super-permission, seeds the admin role with the full set, and
-- marks the four legacy V005 names retired (UPDATE only -- migration M4
-- forbids top-level DELETE FROM). Idempotent throughout (ON CONFLICT DO
-- NOTHING): db-reset replays the whole chain.
--
-- F3 (documented honestly): V005 seeded the USER role with the legacy
-- 'user:read' row. V045 does NOT touch it -- the enforcement layer reads
-- only the V023-shaped permission names, so the legacy row is inert
-- (equivalent to no permission). Operators migrating must re-grant with
-- the new names.

-- 1. Permission catalog: the nine V023 resource scopes + admin.
INSERT INTO permissions (name, description) VALUES
    ('users:read',  'Read user accounts and roles'),
    ('users:write', 'Create, update, and delete users; assign roles'),
    ('clients:read',  'View OAuth2 client registrations'),
    ('clients:write', 'Create, update, and delete OAuth2 clients'),
    ('tokens:read',  'View issued tokens'),
    ('tokens:write', 'Revoke and manage tokens'),
    ('roles:read',  'View roles and the scope catalog'),
    ('roles:write', 'Create and update roles, scopes, and their mappings'),
    ('audit:read',  'Read and export audit logs'),
    ('admin', 'Administrative super-permission (implies the full set)')
ON CONFLICT (name) DO NOTHING;

-- 2. Legacy V005 names: retired in place (semantics moved to the V023
-- names above). The description rewrite is the marker.
UPDATE permissions
SET description = description || ' [LEGACY: retired by V045 -- semantics moved to the V023 resource-scope permission names]'
WHERE name IN ('user:read', 'user:write', 'user:delete', 'admin:access')
  AND description NOT LIKE '%[LEGACY:%';

-- 3. Admin role: the full set (idempotent).
INSERT INTO role_permissions (role_id, permission_id)
SELECT r.id, p.id
FROM roles r, permissions p
WHERE r.name = 'admin'
  AND p.name IN ('users:read', 'users:write', 'clients:read', 'clients:write',
                 'tokens:read', 'tokens:write', 'roles:read', 'roles:write',
                 'audit:read', 'admin')
ON CONFLICT DO NOTHING;
