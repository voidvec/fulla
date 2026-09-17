-- V035: Open platform — client ownership, soft delete, self-service scopes
-- (v1.4.0; see docs-local/productization-evolution/v1.4.0-open-platform-
-- design.md §2/§4).
--
-- oauth2_client_owners: ownership & governance metadata for self-registered
-- clients. Kept as a side table so the V002 core table is untouched.
--   creator_user_id  — immutable audit anchor (who created it; NOT the
--                      permission source).
--   org_id           — optional management anchor; NULL = personal app.
--                      Management permission: org owner/admin members when
--                      set, the creator when NULL. ON DELETE SET NULL turns
--                      an org app into a personal one if the org row is ever
--                      hard-deleted.
--   status           — active|suspended; suspended clients fail client
--                      validation (signing/token) immediately.
-- Admin-seeded clients (fulla-portal etc.) have no row here = admin-owned.
CREATE TABLE IF NOT EXISTS oauth2_client_owners (
    client_id VARCHAR(50) NOT NULL PRIMARY KEY REFERENCES oauth2_clients(client_id) ON DELETE CASCADE,
    creator_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    org_id INTEGER REFERENCES organizations(id) ON DELETE SET NULL,
    status VARCHAR(20) NOT NULL DEFAULT 'active',
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_client_owners_creator ON oauth2_client_owners(creator_user_id);
CREATE INDEX IF NOT EXISTS idx_client_owners_org ON oauth2_client_owners(org_id);

-- Soft delete for oauth2_clients (aligned with users V024): NULL = live.
-- Soft-deleted clients fail client validation and are excluded from the
-- owner quota count.
ALTER TABLE oauth2_clients ADD COLUMN IF NOT EXISTS deleted_at TIMESTAMP WITH TIME ZONE;

-- Self-service scope flag on the registry: only scopes marked TRUE may be
-- selected by self-registered applications. Defaults to FALSE (fail-closed);
-- an operator widens the allowlist explicitly.
ALTER TABLE oauth2_scopes ADD COLUMN IF NOT EXISTS self_service BOOLEAN NOT NULL DEFAULT FALSE;

-- The OIDC basic trio is flagged here (the platform promise: an enabled open
-- platform lets an app do a standard code+PKCE login); everything else stays
-- FALSE. Creation itself remains gated by open_platform.enabled=false, so
-- this flag alone opens nothing.
UPDATE oauth2_scopes SET self_service = TRUE
WHERE name IN ('openid', 'profile', 'email');
