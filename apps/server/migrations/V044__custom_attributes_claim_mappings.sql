-- V044 (P1-3, #280): custom attributes + claims mapping.
--
-- users.custom_attributes: self-service JSONB store (depth <= 2, <= 8 KB,
-- key regex enforced in the application layer).
--
-- claim_mappings: the Keycloak-protocol-mappers minimal equivalent. The
-- application layer enforces the security red lines at WRITE time
-- (deliverables/p1-fix-design-2026-10.md §P1-3 rev.1):
--   * claim_name reserved-word blacklist (mappings may NEVER shadow a
--     standard or already-published claim);
--   * source_path whitelist: custom_attributes.* prefix + the single
--     built-in user.display_name (org.role is CUT — cross-context leak);
--   * scope_name <> 'openid' (openid is always granted; mapping on it
--     would be zero-authorization publication).
-- Idempotent throughout (db-reset replays the chain).

ALTER TABLE users ADD COLUMN IF NOT EXISTS custom_attributes JSONB NOT NULL DEFAULT '{}';

CREATE INDEX IF NOT EXISTS idx_users_custom_attributes_gin
    ON users USING GIN (custom_attributes);

CREATE TABLE IF NOT EXISTS claim_mappings (
    id SERIAL PRIMARY KEY,
    scope_name VARCHAR(64) NOT NULL,
    claim_name VARCHAR(64) NOT NULL,
    source_path VARCHAR(128) NOT NULL,
    include_in VARCHAR(16) NOT NULL DEFAULT 'both',
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    CONSTRAINT uq_claim_mappings UNIQUE (scope_name, claim_name)
);
