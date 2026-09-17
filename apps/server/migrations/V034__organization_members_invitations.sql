-- V034: Organization membership + invitations (v1.4.0 org mainline)
-- See docs-local/productization-evolution/v1.4.0-profile-orgs-automation-
-- design.md §B.
--
-- organization_members: M:N user<->org with an org-scoped role
-- (owner|admin|member). Org roles are ORTHOGONAL to global RBAC
-- (user_roles): an org admin is NOT a system admin. Owner uniqueness is
-- enforced at the service layer (creator becomes owner; transfers run in
-- one transaction).
--
-- organization_invitations: single-use, 72h-expiring email invitations.
-- Email is stored lowercase-normalized (aligned with V031 users.email
-- normalization) so the accept path can't miss on case.
CREATE TABLE IF NOT EXISTS organization_members (
    id SERIAL PRIMARY KEY,
    organization_id INTEGER NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role VARCHAR(20) NOT NULL DEFAULT 'member',
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (organization_id, user_id)
);

CREATE TABLE IF NOT EXISTS organization_invitations (
    id SERIAL PRIMARY KEY,
    organization_id INTEGER NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    email VARCHAR(255) NOT NULL,
    role VARCHAR(20) NOT NULL DEFAULT 'member',
    token VARCHAR(64) NOT NULL UNIQUE,
    invited_by INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at TIMESTAMP WITH TIME ZONE NOT NULL,
    accepted_at TIMESTAMP WITH TIME ZONE,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Only ONE pending invitation per (org, email); accepted/revoked rows don't
-- block re-inviting the same person later.
CREATE UNIQUE INDEX IF NOT EXISTS uq_org_invitations_pending
  ON organization_invitations(organization_id, email)
  WHERE accepted_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_org_members_user ON organization_members(user_id);
CREATE INDEX IF NOT EXISTS idx_org_invitations_email ON organization_invitations(email);
