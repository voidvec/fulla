-- V036: Tenant semantics groundwork (v1.5.0 M0)
-- See docs-local/productization-evolution/v1.5.0-real-tenant-design.md
-- §1.2/§2.1/§2.2/§2.5/§3.4 and the stage-two implementation plan M0.
--
-- Everything here is schema + seed only: M0 plants the skeleton, M1+
-- wire the pipelines. All statements idempotent (db-reset replays the
-- whole chain); no CONCURRENTLY (SchemaManager runs migrations in a
-- single transaction).

-- 1) Org admin consent (design §2.2): one row per (org, client, scope),
--    same shape as oauth2_user_consents so the ScopeDecision merge in M2
--    is a union. CASCADE on all three anchors: a deleted org/client/scope
--    takes its consents along (org consent tracks a live relationship,
--    not history -- revocation history lives in audit_logs).
CREATE TABLE IF NOT EXISTS organization_consents (
    id SERIAL PRIMARY KEY,
    organization_id INTEGER NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    client_id VARCHAR(50) NOT NULL REFERENCES oauth2_clients(client_id) ON DELETE CASCADE,
    scope_name VARCHAR(100) NOT NULL REFERENCES oauth2_scopes(name) ON DELETE CASCADE,
    granted_by INTEGER NOT NULL REFERENCES users(id),
    granted_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    revoked_at TIMESTAMPTZ,
    UNIQUE (organization_id, client_id, scope_name)
);
-- Partial index for the M2 consent check (active rows only).
CREATE INDEX IF NOT EXISTS idx_org_consents_active
    ON organization_consents(client_id, scope_name) WHERE revoked_at IS NULL;

-- 2) Org MFA enforcement, core minimal set (design §2.5). Enforcement
--    point is M1's org parameter validity check; M0 only plants the flag.
ALTER TABLE organizations ADD COLUMN IF NOT EXISTS require_mfa BOOLEAN NOT NULL DEFAULT FALSE;

-- 3) Authorization-chain org context columns (design §2.1 item 4):
--    active org is per-authorization, not per-session. authorize picks it,
--    the code row carries it, token exchange inherits along the chain,
--    refresh reuses it. M1 wires the pipeline; M0 only plants the columns.
--    No indexes (deliberate): rows carry the value, nothing ever searches
--    by org (V9 ratified no bulk revocation) -- token-table write
--    amplification is the red line here.
ALTER TABLE oauth2_codes          ADD COLUMN IF NOT EXISTS org_id INTEGER REFERENCES organizations(id) ON DELETE SET NULL;
ALTER TABLE oauth2_access_tokens  ADD COLUMN IF NOT EXISTS org_id INTEGER REFERENCES organizations(id) ON DELETE SET NULL;
ALTER TABLE oauth2_refresh_tokens ADD COLUMN IF NOT EXISTS org_id INTEGER REFERENCES organizations(id) ON DELETE SET NULL;

-- 4) Audit org dimension (design §3.4, ruling V6). No FK (deliberate):
--    audit_logs is append-only partitioned history -- an org soft delete
--    must not rewrite it (FK SET NULL would), and a per-row FK check on
--    the hot audit path is not payable. Writers must keep the invariant.
ALTER TABLE audit_logs ADD COLUMN IF NOT EXISTS org_id INTEGER;

-- 5) Dead-column cleanup (ruling V7): the V017 clients-side org anchor
--    and its index. Zero code consumers (every getOrgId() call site is on
--    owner/member models; the open platform's org_id fields read the
--    owners table). The true source of org anchoring is
--    organization_members + oauth2_client_owners (V034/V035).
DROP INDEX IF EXISTS idx_clients_org;
ALTER TABLE oauth2_clients DROP COLUMN IF EXISTS org_id;

-- 6) org scope seed (design §2.1 item 1), modeled on the V006/V023
--    idempotent seed pattern. self_service=TRUE puts it on the
--    self-registered-application allowlist from day one (the org claim is
--    a protocol semantic, core and free per D3).
INSERT INTO oauth2_scopes (name, description) VALUES
  ('org', 'Organization context (org_ctx: active org id/name/roles for this authorization)')
  ON CONFLICT (name) DO NOTHING;
UPDATE oauth2_scopes SET self_service = TRUE WHERE name = 'org';
