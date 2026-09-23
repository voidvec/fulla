-- V037: Ownership succession nominations (v1.5.0 M3, real-tenant design
-- §1.3 item 2). The nominate-and-accept workflow: an owner nominates a
-- successor (two-step confirmation -- the nominee must accept), the
-- acceptance swaps the seat in ONE transaction, and an owner deleting
-- their account auto-effects any pending nomination.
--
-- Table shape per ruling R-M3-2:
--   * at most ONE pending (accepted_at IS NULL) nomination per org --
--     enforced by the partial unique index below (a new nomination
--     overwrites the previous pending row via upsert);
--   * nominee may be any live user (membership upserted at acceptance --
--     aligning with the #228 admin transfer semantics);
--   * CASCADE on the org; plain FK on the nominee (no ON DELETE) -- the
--     delete paths handle nominations BEFORE deleting users.
--   * nominated_by deliberately carries NO FK (ruling deviation,
--     documented in the M3 PR): drogon_ctl emits one relationship getter
--     per foreign key NAMED AFTER the target table, so two users(id)
--     foreign keys produce two identical getUsers() declarations and the
--     generated model does not compile (models are untouchable output).
--     The nominator is always the live owner at nominate time; the
--     column stays a plain audit anchor.
--
-- Everything idempotent (db-reset replays the chain); no CONCURRENTLY
-- (SchemaManager runs migrations in a single transaction).

CREATE TABLE IF NOT EXISTS organization_succession_nominations (
    id SERIAL PRIMARY KEY,
    organization_id INTEGER NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    nominee_user_id INTEGER NOT NULL REFERENCES users(id),
    nominated_by INTEGER NOT NULL,
    created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    accepted_at TIMESTAMPTZ
);

-- One pending nomination per organization (R-M3-2 partial unique index).
CREATE UNIQUE INDEX IF NOT EXISTS uq_succession_pending_per_org
    ON organization_succession_nominations(organization_id)
    WHERE accepted_at IS NULL;
