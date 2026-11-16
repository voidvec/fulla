-- P2-1: OIDC-in generic upstream federation registry.
--
-- One row = one externally-issued OIDC identity provider this deployment
-- offers on the login page. Provider rows are administered at runtime via
-- /api/admin/upstream-providers; the login flow resolves `provider_key`
-- rows through oauth2_subject_mappings.provider (the same column the
-- google/wechat/github links use), so account linking, the auto-create
-- gate and the social-link/unlink surface stay unchanged.
--
-- Idempotent (CREATE TABLE IF NOT EXISTS): db-reset replays the whole
-- chain, and the in-process seeder runs this file through
-- SchemaManager::migrate too.

CREATE TABLE IF NOT EXISTS upstream_providers (
    id SERIAL PRIMARY KEY,
    provider_key VARCHAR(64) UNIQUE NOT NULL,
    display_name VARCHAR(128) NOT NULL,
    issuer VARCHAR(512) NOT NULL,
    client_id VARCHAR(256) NOT NULL,
    client_secret VARCHAR(512) NOT NULL,
    scopes VARCHAR(512) NOT NULL DEFAULT 'openid profile email',
    claims_mapping JSONB NOT NULL DEFAULT '{}'::jsonb,
    endpoint_overrides JSONB NOT NULL DEFAULT '{}'::jsonb,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT upstream_providers_key_format CHECK (
        provider_key ~ '^[a-z0-9][a-z0-9-]{1,62}[a-z0-9]$'
    )
);

CREATE INDEX IF NOT EXISTS idx_upstream_providers_enabled
    ON upstream_providers (enabled) WHERE enabled;

COMMENT ON TABLE upstream_providers IS
    'P2-1 OIDC-in upstream provider registry (login-page federation sources)';
COMMENT ON COLUMN upstream_providers.provider_key IS
    'Slug used in URLs and oauth2_subject_mappings.provider; lowercase [a-z0-9-]';
COMMENT ON COLUMN upstream_providers.claims_mapping IS
    '{"sub":"sub","email":"email","username":"preferred_username","name":"name"} style claim sources';
COMMENT ON COLUMN upstream_providers.endpoint_overrides IS
    'Optional discovery bypass: {"authorize":url,"token":url,"userinfo":url}';
