# OpenID Connect (OIDC) Integration Guide

This guide describes how to integrate this OAuth2 service into your application as an OIDC Provider.

## 1. Discovery Endpoint

The OIDC discovery endpoint provides all configuration information about the Provider:

```
GET /.well-known/openid-configuration
```

Example response:

```json
{
  "issuer": "https://your-domain.com",
  "authorization_endpoint": "https://your-domain.com/oauth2/authorize",
  "token_endpoint": "https://your-domain.com/oauth2/token",
  "userinfo_endpoint": "https://your-domain.com/oauth2/userinfo",
  "jwks_uri": "https://your-domain.com/.well-known/jwks.json",
  "response_types_supported": ["code"],
  "subject_types_supported": ["public"],
  "id_token_signing_alg_values_supported": ["RS256"],
  "scopes_supported": ["openid", "profile", "email"]
}
```

## 2. JWKS Endpoint

The JSON Web Key Set endpoint provides the public keys used to verify `id_token` signatures:

```
GET /.well-known/jwks.json
```

Example response:

```json
{
  "keys": [
    {
      "kty": "RSA",
      "use": "sig",
      "alg": "RS256",
      "kid": "default-key-id",
      "n": "<base64url-encoded modulus>",
      "e": "AQAB"
    }
  ]
}
```

## 3. id_token Format

The `id_token` is an RS256-signed JWT containing the following standard claims:

| Claim | Description | Example |
|-------|------|------|
| `iss` | Issuer | `https://your-domain.com` |
| `sub` | Unique user identifier (UUID public_sub) | `550e8400-e29b-41d4-a716-446655440000` |
| `aud` | Audience (Client ID) | `your-client-id` |
| `exp` | Expiration time (Unix timestamp) | `1700000000` |
| `iat` | Issued-at time (Unix timestamp) | `1699996400` |
| `nonce` | The nonce value sent in the request | `abc123` |

Depending on the requested scopes, it may also contain:

- **profile scope**: `name`, `preferred_username`
- **email scope**: `email`, `email_verified`
- **org scope with an organization context** (v1.5.0): `org_ctx` — an object
  `{ org_id, org_name, roles[] }` describing the active organization bound to
  this authorization and the caller's current membership roles in it. Present
  only when the authorize request carried a valid `org_id` and the client
  requested the `org` scope; membership is re-checked at issuance, so the
  claim disappears once membership ends. The same `org_ctx` object appears in
  the userinfo response.

## 4. Verifying the id_token

### 4.1 Verification Steps

1. **Decode the JWT header**: extract `kid` (Key ID) and `alg` (should be RS256)
2. **Fetch the public key**: retrieve the public key matching the `kid` from the JWKS endpoint
3. **Verify the signature**: verify the JWT signature with the RSA public key
4. **Verify the claims**:
   - `iss` must match your configured Issuer URL
   - `aud` must include your Client ID
   - `exp` must be in the future
   - `nonce` must match the value you sent in the authorization request

### 4.2 Security Considerations

- **Always verify the signature**; never trust an unverified JWT
- **Cache the JWKS**, but with a sensible refresh interval (24 hours is recommended, or follow the Cache-Control header)
- **Check the `alg` header** and reject the `none` algorithm (prevents algorithm downgrade attacks)

## 5. Supported Scopes and Claims

| Scope | Returned Claims |
|-------|---------------|
| `openid` | `sub` (required scope; enables OIDC) |
| `profile` | `name`, `preferred_username` |
| `email` | `email`, `email_verified` |
| `org` | `org_ctx` (`{org_id, org_name, roles[]}`) — only when the authorization carried a valid organization context (v1.5.0) |

## 6. Integration Examples

### 6.1 Using a Standard OIDC Client Library (Node.js)

```javascript
const { Issuer } = require('openid-client');

// Discover the Provider configuration automatically
const issuer = await Issuer.discover('https://your-domain.com');

const client = new issuer.Client({
  client_id: 'your-client-id',
  client_secret: 'your-client-secret',
  redirect_uris: ['http://localhost:3000/callback'],
  response_types: ['code'],
});

// Generate the authorization URL
const authUrl = client.authorizationUrl({
  scope: 'openid profile email',
  state: 'random-state-value',
  nonce: 'random-nonce-value',
});

// Handle the callback
const params = client.callbackParams(req);
const tokenSet = await client.callback('http://localhost:3000/callback', params, {
  state: 'random-state-value',
  nonce: 'random-nonce-value',
});

console.log('ID Token claims:', tokenSet.claims());
console.log('Access Token:', tokenSet.access_token);
```

### 6.2 Using Python (authlib)

```python
from authlib.integrations.requests_client import OAuth2Session

client = OAuth2Session(
    client_id='your-client-id',
    client_secret='your-client-secret',
    redirect_uri='http://localhost:8000/callback',
    scope='openid profile email'
)

# Build the authorization URL
uri, state = client.create_authorization_url(
    'https://your-domain.com/oauth2/authorize'
)

# Handle the callback and exchange for tokens
token = client.fetch_token(
    'https://your-domain.com/oauth2/token',
    authorization_response=callback_url
)

# Fetch user information
userinfo = client.get('https://your-domain.com/oauth2/userinfo').json()
```

### 6.3 Using Go (coreos/go-oidc)

```go
provider, err := oidc.NewProvider(ctx, "https://your-domain.com")

oauth2Config := oauth2.Config{
    ClientID:     "your-client-id",
    ClientSecret: "your-client-secret",
    RedirectURL:  "http://localhost:8080/callback",
    Endpoint:     provider.Endpoint(),
    Scopes:       []string{oidc.ScopeOpenID, "profile", "email"},
}

// Verify the id_token
verifier := provider.Verifier(&oidc.Config{ClientID: "your-client-id"})
idToken, err := verifier.Verify(ctx, rawIDToken)
```

## 7. FAQ

**Q: How long is the id_token valid?**
A: The same as the access token — 1 hour by default.

**Q: How should key rotation be handled?**
A: Refresh the public keys from the JWKS endpoint periodically. When verification fails, refresh the JWKS first, then retry the verification.

**Q: Is PKCE supported?**
A: Yes. PKCE (RFC 7636) is implemented and enforced by default for PUBLIC clients (both `plain` and `S256` are supported). SPA and mobile clients should use `S256`.
