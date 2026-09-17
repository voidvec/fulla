# Build an app on Fulla (OIDC sign-in)

Let your web/mobile/CLI application use Fulla as its identity provider:
standard OpenID Connect, any standards-compliant library, no custom protocol.

## 1. Register your application

- **Where enabled**, self-register: Portal → **My Applications** → *Register
  application*. Choose:
  - `PUBLIC` for browser/mobile apps (authorization code + PKCE, no secret);
  - `CONFIDENTIAL` for server-side apps (client secret, shown exactly once —
    store it in a secret manager).
- Otherwise ask your Fulla administrator to create the client.
- Register your exact redirect URI(s) (`https://` required outside
  development).
- Pick scopes from the self-service allowlist (`openid`, `profile`, `email`
  by default).

## 2. Point your OIDC library at Fulla

Discovery does the rest:

```
https://your-fulla.example/.well-known/openid-configuration
```

Standard settings: authorization endpoint `/oauth2/authorize`, token endpoint
`/oauth2/token`, JWKS `/.well-known/jwks.json`, scopes `openid profile email`.

## 3. Sign users in

Authorization code + PKCE (works for PUBLIC and CONFIDENTIAL clients):

```
GET /oauth2/authorize?
  client_id=YOUR_CLIENT_ID
  &redirect_uri=https://your-app.example/callback
  &response_type=code
  &scope=openid profile email
  &state=<random>
  &code_challenge=<S256(verifier)>
  &code_challenge_method=S256
```

Exchange the code at `/oauth2/token`, verify the `id_token` against the JWKS,
and read profile claims from `/oauth2/userinfo`.

## 4. Automation (no user present)

Scripts and service-to-service integrations do not sign users in — see
[Automating with Fulla](automation.md) for device flow and
client_credentials.

## 5. Operations

- Rotating a secret (Portal → My Applications → *Rotate secret*) invalidates
  the previous secret immediately.
- Deleting an application revokes its consents and stops token issuance.
- Administrators can suspend abusive applications; suspended clients fail
  client validation immediately.
