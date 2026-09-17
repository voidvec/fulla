-- DEV ONLY: Admin Console OAuth2 client (PUBLIC, uses PKCE)
-- Used by the OAuth2Admin management console for login
-- DO NOT use in production without changing redirect_uris!

INSERT INTO oauth2_clients (client_id, client_type, client_secret, salt, name, redirect_uris, allowed_grant_types, token_endpoint_auth_method)
VALUES (
    'fulla-admin-console',
    'PUBLIC',
    'not-used-public-client',
    '',
    'Admin Console',
    'http://127.0.0.1:5174/admin/callback,http://localhost:5174/admin/callback,http://127.0.0.1:8081/admin/callback,http://localhost:8081/admin/callback',
    'authorization_code,refresh_token',
    'none'
)
ON CONFLICT (client_id) DO NOTHING;

-- Grant scopes to fulla-admin-console
INSERT INTO oauth2_client_scopes (client_id, scope_name)
SELECT 'fulla-admin-console', name FROM oauth2_scopes WHERE name IN ('openid', 'profile', 'admin')
ON CONFLICT (client_id, scope_name) DO NOTHING;
