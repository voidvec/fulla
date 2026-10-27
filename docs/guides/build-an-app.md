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

### 1a. Programmatic registration (RFC 7591)

Automation and Terraform-style provisioning can register clients directly
via `POST /oauth2/register`. The endpoint is **admin-gated**: the Bearer
token must belong to a user holding the `admin` role (open registration is a
deliberate non-goal). This is the same call the admin console's
*Register application (RFC 7591)* form makes.

```bash
# 1) Obtain an admin Bearer token. On a fresh dev install the seeded
#    fulla-portal PUBLIC client and the bootstrap admin work end to end:
VERIFIER=$(openssl rand -base64 48 | tr '+/' '-_' | tr -d '=')
CHALLENGE=$(printf %s "$VERIFIER" | openssl dgst -sha256 -binary | base64 | tr '+/' '-_' | tr -d '=')
CODE=$(curl -s -X POST 'http://localhost:5555/oauth2/login?json=true' \
  -d "username=admin&password=ADMIN_PASSWORD" \
  -d "client_id=fulla-portal&redirect_uri=http://localhost:5173/callback" \
  -d "scope=openid&state=dcr&code_challenge=$CHALLENGE&code_challenge_method=S256" \
  | jq -r .code)
TOKEN=$(curl -s -X POST http://localhost:5555/oauth2/token \
  -d "grant_type=authorization_code&code=$CODE" \
  -d "client_id=fulla-portal&redirect_uri=http://localhost:5173/callback" \
  -d "code_verifier=$VERIFIER" | jq -r .access_token)

# 2) Register the client:
curl -X POST http://localhost:5555/oauth2/register \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "client_name": "my-terraform-managed-app",
    "client_type": "CONFIDENTIAL",
    "token_endpoint_auth_method": "client_secret_basic",
    "redirect_uris": ["https://app.example.com/callback"],
    "grant_types": ["authorization_code", "refresh_token"],
    "post_logout_redirect_uris": ["https://app.example.com/"]
  }'
```

Request fields:

| Field | Required | Notes |
|---|---|---|
| `client_name` | yes | Display name shown on login/consent screens. |
| `client_type` | no | `CONFIDENTIAL` (default) or `PUBLIC`. |
| `token_endpoint_auth_method` | no | `none` / `client_secret_basic` / `client_secret_post`. Defaults by type: PUBLIC → `none` (only option), CONFIDENTIAL → `client_secret_basic`. Enforced at the token endpoint afterwards (F-017). |
| `redirect_uris` | CONFIDENTIAL | Array. `https://` required outside development (loopback exempt; `auth.allow_http_redirect_uri` overrides in dev). Exact string match at authorize time. |
| `grant_types` | no | Subset of `authorization_code`, `refresh_token`, `client_credentials`; defaults to `["authorization_code"]`. |
| `post_logout_redirect_uris` | no | Dedicated logout targets (same scheme policy). Non-empty is **authoritative** for `end_session` — a login-valid URI not registered for logout is rejected; empty falls back to the login redirect list. |

The `201 Created` response echoes the registration and adds the generated
credentials: `client_id`, `client_secret`, `client_id_issued_at`, and
`client_secret_expires_at` (`0` — secrets do not auto-expire; rotate them
explicitly). **The `client_secret` is shown exactly once** — store it in a
secret manager immediately; there is no read-back path, only
reset/rotation. The registration is audited as `client_registered`
(success and failure).

Management of existing registrations (RFC 7592 read/update/delete) is not
implemented — use the admin clients API (`/api/admin/clients`) instead.

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
