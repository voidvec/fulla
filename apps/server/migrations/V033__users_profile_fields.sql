-- V033: User profile fields (v1.4.0 profile minimal set)
-- Adds display_name + avatar_url to users (see docs-local/productization-
-- evolution/v1.4.0-profile-orgs-automation-design.md §A).
-- Both nullable = "not set"; no NOT NULL and no index (no lookup path).
-- display_name: 0-100 chars, free-form (multi-language nicknames; length is
--   enforced at the service layer).
-- avatar_url: https-only URL, max 2048 chars, served verbatim to clients
--   (never fetched server-side, so no SSRF surface; protocol + length are
--   enforced at the service layer).
ALTER TABLE users ADD COLUMN IF NOT EXISTS display_name VARCHAR(100);
ALTER TABLE users ADD COLUMN IF NOT EXISTS avatar_url TEXT;
