-- Migration: V039__post_logout_redirect_uris
-- Created: 2026-10-11
-- Purpose: P0-7b (#261) — dedicated post-logout redirect registration.
--          end_session previously validated post_logout_redirect_uri against
--          the client's LOGIN redirect_uris (login and logout redirect
--          semantics conflated; no dedicated registration surface existed).
--          The new column holds a comma-joined list using the same storage
--          convention as redirect_uris / allowed_grant_types. NULL/empty
--          preserves the legacy fallback: validate against redirect_uris.

-- === UP ===
ALTER TABLE oauth2_clients
    ADD COLUMN IF NOT EXISTS post_logout_redirect_uris TEXT;

-- === DOWN ===
-- Forward-compatible rollback: applications keep validating against
-- redirect_uris when the column is absent, so dropping it is safe.
-- ALTER TABLE oauth2_clients DROP COLUMN IF EXISTS post_logout_redirect_uris;
