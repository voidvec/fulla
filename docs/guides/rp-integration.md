# Integrating Fulla as an OIDC Provider (RP guide)

Audience: teams wiring an external relying party (RP) — WeKnora, Nextcloud,
an internal SSO app — to Fulla as the identity provider. This guide collects
the integration-critical facts that are otherwise spread across
[build-an-app.md](build-an-app.md), the [configuration guide](../operate/configuration-guide.md),
and [deployment.md](../operate/deployment.md). It was written after the
WeKnora v0.9.0 integration (2026-11), and every gotcha in it is a defect that
actually bit an integrator.

## 0. Deployment prerequisites (read first)

- **Tell the backend where the login and consent pages are when the SPA is
  on another origin.** `/oauth2/authorize` sends an unauthenticated user to
  the login page and a first-time authorization to the consent page. By
  default both targets are backend-relative (`/login`, `/consent`), which
  reaches the SPA only where a gateway fronts backend and SPA on one origin
  (the production stack, Helm). Without such a gateway — the development
  compose files, or any deployment where the SPA has its own host or port —
  set `FULLA_OAUTH2_LOGIN_URL` and `FULLA_OAUTH2_CONSENT_URL` to the SPA's
  pages; `deploy/docker/docker-compose.yml` already does. Left unset there,
  the user lands on the backend's own login page and, when consent is
  required, on a 404 (configuration-guide §4c).
- **Serve HTTPS in production.** Plain HTTP on a non-localhost host is a
  non-secure browser context: `crypto.subtle`/`crypto.randomUUID` are
  unavailable there. The bundled SPAs degrade gracefully (pure-JS SHA-256 for
  PKCE, `getRandomValues`-based UUIDs) so login still works on LAN-IP HTTP
  deployments, but session cookies cross the wire in cleartext — treat HTTP
  as a LAN-only convenience, never a production posture.
- **Same-origin reverse proxying**: the SPAs call the backend through
  relative paths (`/oauth2/…`, `/api/…`, `/.well-known/…`) proxied by nginx.
  Keep `VITE_API_BASE_URL` empty; setting it breaks routing
  (deployment.md, "Frontend configuration").

## 1. Register your RP client

Use the admin console (Applications) or `POST /oauth2/register` /
`POST /api/admin/clients` — see [build-an-app.md §1](build-an-app.md).
Integration-critical fields:

| Field | Watch out for |
|---|---|
| `redirect_uris` | Exact string match at authorize time — scheme, host, port, path must all match. `localhost` and `127.0.0.1` are different hosts. Mismatch ⇒ 3027 `VALIDATION_CLIENT_REDIRECT_URI_NOT_REGISTERED`. |
| `client_type` | `PUBLIC` (SPA / mobile, no secret) or `CONFIDENTIAL` (server-side). |
| `token_endpoint_auth_method` | `CONFIDENTIAL` defaults to `client_secret_basic`. If your OIDC library sends the secret in the form body (common: WeKnora, many PHP/Java libs), declare `client_secret_post` at registration — or change it later via `PUT /api/admin/clients/{id}`. F-017 enforces the declared method strictly at token/introspect/revoke; a mismatch answers `401 invalid_client` even with correct credentials. |

**Client lifecycle**: the database is authoritative at runtime;
`config.json` `clients` entries are seed-only (`ON CONFLICT DO NOTHING` —
edits there never reach an existing row, and restarts never revert admin-API
edits). See deployment.md, "Client lifecycle".

## 2. Wire the flow

Point your OIDC library at discovery:

```
{issuer}/.well-known/openid-configuration
```

Authorization code + PKCE is the supported sign-in flow. PKCE is mandatory
for every authorization-code client by default (`auth.require_pkce`). The
key was renamed from `require_pkce_for_public` and the old name is no longer
read: a deployment that disabled PKCE for a legacy RP under the old key must
rename it, otherwise PKCE is enforced again. `state` is **required**, 8–512
characters, no URL delimiters — the server 400s otherwise before any session
check.

- Not signed in ⇒ 302 to the SPA login, your authorize context carried in
  the query; after login the SPA resumes `/oauth2/authorize`, and first-time
  access stops at the SPA consent screen.
- **The IdP session is a separate lifetime from your users' tokens.** Fulla's
  single sign-on session lives for `session_timeout` / `session_max_age`
  (one hour by default) and is not extended when tokens are refreshed. Once
  it has ended — or when your request carries `prompt=login` or a short
  `max_age` — the user signs in once more and the authorize request resumes
  by itself, so a returning user sees the IdP login page instead of a silent
  redirect back to your callback. Raise both settings if that is too frequent
  for your deployment ([Session Management](../domains/session-management.md)).
- The consent page submits `POST /oauth2/consent` **with `json=true`** and
  navigates itself from the JSON `location` field. This is deliberate: a
  native form POST whose 302 lands on a cross-origin RP callback is killed
  by Chrome's `form-action` enforcement (Chrome applies `form-action` to the
  redirect following a form submission, contrary to the CSP spec —
  w3c/webappsec-csp#8). If you build a custom consent frontend, do the same;
  do not loosen the CSP.

## 3. Legacy semantics: direct `/oauth2/login` mints codes without consent

`POST /oauth2/login` is the SPA's two-step login endpoint. On success it
**issues an authorization code directly** — it never evaluates whether
consent is required. This is intentional legacy 2-step semantics (the
portal's consent state resolves downstream; ADR-0014 / the handler comment
document the tiering contract). The SPA's own browser flow never bypasses
consent: it posts to `/oauth2/login` only for the session leg, then resumes
`/oauth2/authorize`, which performs the consent evaluation.

If you drive `/oauth2/login` directly (curl scripts, legacy RPs), you are
using this legacy contract: first-time access will NOT stop at a consent
screen. Do not point an interactive RP at `/oauth2/login`; use
`/oauth2/authorize`.

## 4. Troubleshooting quick table

| Symptom | Cause | Fix |
|---|---|---|
| `401 invalid_client` at token exchange with correct credentials | Declared `token_endpoint_auth_method` ≠ how you send the secret | Align method ↔ transport (§1 table) |
| Login fails 3027 "回调地址未注册" | `redirect_uri` not exactly registered | Register the exact URI (§1); the SPA banner shows the URI it sent |
| Browser console: consent POST blocked by CSP `form-action` | Chrome form-action-on-redirect behavior (§2) | Use the bundled SPA consent page (json=true) — do not loosen CSP |
| SPA login says "client-side error" / PKCE fails on LAN HTTP | Non-secure context (§0) | Works via fallback; serve HTTPS for production |
| Stuck at `authorizing…`, no navigation | Old SPA bundle (pre-2026-11) doing a native form POST + 302 | Rebuild/deploy the current SPA image |
| A later login from the RP lands on the IdP home page instead of calling back the RP | The IdP session has ended, and an old SPA bundle bounced the still-signed-in portal user off `/login` to the dashboard, dropping the authorize parameters | Rebuild/deploy the current SPA image; the user then signs in once and the authorization resumes (§2) |
| Consent POST answers 400 `VALIDATION_INVALID_INPUT` "consent_csrf" | The one-shot CSRF nonce expired (TTL ~10 min) or was already consumed (double submit / reload) | Restart the authorize flow |
| Code issued but token exchange rejects `invalid_grant` | PKCE verifier mismatch, or the code was consumed/expired | Send the verifier paired with the flow's `code_challenge` (RFC 7636 §4.6) |

## 5. Checklists

**Before go-live**

- [ ] Backend and SPA share one origin behind a gateway, or `FULLA_OAUTH2_LOGIN_URL` / `FULLA_OAUTH2_CONSENT_URL` point at the SPA's pages
- [ ] Issuer is `https://` (configuration-guide §4)
- [ ] RP client registered with exact redirect URIs and the right auth method
- [ ] Secrets stored in a secret manager (client_secret shown exactly once)
- [ ] HTTPS end to end; HSTS at the proxy

**When the RP cannot sign in**

1. Check the exact error code in the response envelope (3027 vs `invalid_client` vs `invalid_grant`).
2. Compare the failing `redirect_uri` byte-for-byte against the client row (`GET /api/admin/clients/{id}`).
3. Confirm the transport of the client secret matches the declared method.
4. grep the server log for the request_id returned in the envelope.
