-- V042 (P1-4, #281): organizations soft delete.
--
-- Prerequisite for the admin DELETE /api/admin/organizations/{slug} cascade
-- (deliverables/p1-fix-design-2026-10.md §P1-4, rev.1; renumbered from the
-- planning-level V044 — P1-4 lands before P1-3 in the actual schedule).
--
-- The delete cascade itself lives in application code (OrganizationService):
-- soft-delete marker + slug placeholder rewrite, pending-invitation and
-- pending-succession voiding, org-consent revocation, owned-client
-- suspension, membership detachment — one transaction.

ALTER TABLE organizations ADD COLUMN IF NOT EXISTS deleted_at TIMESTAMPTZ;

-- Read-path filter companion: the partial index keeps org lookups by slug
-- fast once deleted rows carry the '#del-<id>' placeholder slug.
CREATE INDEX IF NOT EXISTS idx_organizations_slug_live
    ON organizations(slug) WHERE deleted_at IS NULL;
