-- V038: Organization consent requests (v1.5.0 #236 entry half, plan B --
-- the member-files-manager-approves workflow). A member asks the org's
-- managers to grant an organization consent for a third-party application;
-- approving writes the organization_consents rows server-side (the gate's
-- condition-3 alternative, already wired in #241, then accepts member
-- authorizations with the org hint -- no bootstrap deadlock: the approval
-- endpoint IS the creation channel).
--
-- Table shape (rulings B2/B3/B8 of .zcode/plans/issues-batch-2/
-- 04-design-B-entry.md):
--   * one row per (org, client, requester) filing; scopes are NOT frozen
--     (B2): approval reads the client's registered scope set at decision
--     time and writes one organization_consents row per scope;
--   * at most ONE pending request per (org, client, requester) -- enforced
--     by the partial unique index below; re-filing is idempotent (200 with
--     the existing row);
--   * requested_by carries the users(id) FK; decided_by deliberately has
--     NO FK (V037 precedent: two users(id) foreign keys make drogon_ctl
--     emit two identical getUsers() declarations and the generated model
--     does not compile; the decider is always a live manager at decision
--     time and stays a plain audit anchor);
--   * CASCADE on the org and the client (the request dies with either --
--     a vanished client can never be authorized); user deletion is only a
--     test-time operation and test cleanup deletes requests before users;
--   * status is app-enforced ('pending'|'approved'|'rejected'), matching
--     the family's thin-schema style; decisions are never physical deletes
--     EXCEPT withdrawal (B7: a withdrawn request is a workflow artifact,
--     the decision trail lives in organization_consents + audit log).
--
-- Everything idempotent (db-reset replays the chain); no CONCURRENTLY
-- (SchemaManager runs migrations in a single transaction).

CREATE TABLE IF NOT EXISTS organization_consent_requests (
    id SERIAL PRIMARY KEY,
    organization_id INTEGER NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    client_id VARCHAR(50) NOT NULL REFERENCES oauth2_clients(client_id) ON DELETE CASCADE,
    requested_by INTEGER NOT NULL REFERENCES users(id),
    requested_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    status VARCHAR(16) NOT NULL DEFAULT 'pending',
    decided_by INTEGER,
    decided_at TIMESTAMPTZ,
    reject_reason TEXT
);

-- One PENDING request per (org, client, requester) (B3 partial unique
-- index; the file endpoint's ON CONFLICT DO NOTHING idempotency rides on
-- exactly this arbiter).
CREATE UNIQUE INDEX IF NOT EXISTS uq_org_consent_requests_pending
    ON organization_consent_requests(organization_id, client_id, requested_by)
    WHERE status = 'pending';

-- Manager list scan: every pending request of one org.
CREATE INDEX IF NOT EXISTS idx_org_consent_requests_org
    ON organization_consent_requests(organization_id)
    WHERE status = 'pending';
