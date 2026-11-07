# Social Login Guide

Backend social login is implemented with a "provider adapter" pattern. Since #70 all three providers — GitHub, Google, WeChat — run the SAME closed loop: upstream code exchange → subject-mapping lookup → (first login) local account auto-create → first-party token pair issuance.

## Uniform account model (#70)

All three providers map `(provider, subject)` — GitHub numeric id / Google `sub` / WeChat `openid` — to a local user through `oauth2_subject_mappings`:

- **Existing mapping**: tokens are issued for the linked local user (soft-deleted or locked linked users are rejected with the generic auth error — no account-status leak).
- **No mapping + auto-create enabled** (default): a local account is created (username `gh_<login>` / `google_<sub12>` / `wx_<openid12>`; a collision retries once with a random suffix, then fails `VALIDATION_USERNAME_TAKEN`), the mapping row is written, the default `user` role is granted, and tokens are issued.
- **No mapping + auto-create disabled**: `403 AUTH_SOCIAL_ACCOUNT_NOT_LINKED` and NO account side effects. 403 (not 401) because "not linked" is an authorization state the client can act on (guide the user to the link flow); probing requires the target account's own upstream provider code, so there is no cross-user enumeration vector.

The auto-create gate is GLOBAL — `external_auth.auto_create_on_first_login` (default `true`) governs GitHub/Google/WeChat together; it is a social policy, not a per-provider toggle. Already-linked users are unaffected by the switch.

## First-party token issuance (#70)

The login endpoints mint an opaque access/refresh pair via the shared `SocialTokenIssuer`:

- Token rows store the **platform subject** (`users.public_sub`) — the same value every Bearer-authenticated handler resolves (`/api/me`, change-password, MFA, WebAuthn). (GitHub previously stored the internal numeric id, which made its tokens 404 on every authenticated endpoint — fixed with the issuer extraction.)
- Issued for the configured FIRST-PARTY client: `external_auth.social_token_client_id` (default `fulla-portal`). This key must never point at a third-party client — the issuance has no consent interaction, so doing so would hand that client tokens nobody agreed to.
- Scope `openid profile email`. Note: the `openid` scope value carries no OIDC semantics on this endpoint (no id_token is issued); it is kept for consistency with the first-party client's scope set.

### Why an audit event and not a consent row

Issuance records a `SOCIAL_LOGIN_TOKEN_ISSUED` audit action (provider, client, scope, internal id) rather than an `oauth2_user_consents` row: a consent row would silently satisfy the consent screen's Tier-3 check and pre-approve the configured client for scopes the user was never prompted for — fabricated consent evidence. An explicit social-consent interaction is a registered follow-up; the audit event is the honest record until then.

This is a first-party extension endpoint, not one of RFC 6749's four grants — the same position the GitHub flow has always had, now documented and audit-traced. The standards-track alternative (social login establishes a browser session, then the SPA runs authorization-code + PKCE with a consent-exempt first-party client) is registered as follow-up work.

## Provider tiers (v1.4.0)

Deployments differ in which external providers make sense (GitHub/Google are
unreachable from mainland China; WeChat is China-only). Availability is a
server-side gate with two switches under `custom_config` in `config.json` (env aliases
`FULLA_EXTERNAL_TIER_DOMESTIC` / `FULLA_EXTERNAL_TIER_INTERNATIONAL`):

```json
"external_auth": {
    "tiers": {
        "domestic": true,       // gates WeChat
        "international": true   // gates GitHub + Google
    }
}
```

Effective enablement = tier on AND credentials configured (a tier switch
composes with the credential gate, never replaces it). The login page
discovers the result at runtime via the public `GET /api/auth/providers`
endpoint. Each entry contains a provider name and a deprecated, empty
`authorize_url`. Starting login requires the transaction endpoint below.
Both tiers default to `true`. Missing credentials and `YOUR_` / `OVERRIDE_`
placeholders disable a provider.

## Browser-bound login protocol (security hardening)

1. Discover enabled providers using `GET /api/auth/providers`.
2. POST `{}` to `/api/auth/providers/{provider}/authorize` using an exact
   configured `Origin` and a credentialed cookie jar (`withCredentials: true`
   or fetch `credentials: 'include'`). No Bearer token is required.
3. Without a binding cookie the server returns **204**, sets the cookie and
   issues no state. Retry this initialization **once** with the cookie. A
   second 204 means cookies are unavailable; stop and ask the user to enable
   them or restart login. Never loop or navigate without a saved transaction.
4. The successful **200** body contains `authorize_url`, `state`, `redirect_uri`
   and `expires_in: 600`. Store a per-state sessionStorage record containing
   `purpose: 'login'`, provider, redirect URI and creation time, then navigate
   to the returned authorize URL. Do not build provider URLs in the browser.
5. Before router startup, clear code/state/error from the callback URL. Validate
   the returned state against that tab's unexpired local transaction, including
   provider and callback origin/path. An unsolicited callback must never
   exchange a code or fall through to the account-linking endpoint.
6. POST `{ "code": "...", "state": "..." }` to `/api/{provider}/login` with
   the same Origin and cookie jar. JSON and form-urlencoded bodies are accepted;
   query parameters and duplicate code/state fields are rejected. The backend
   validates and atomically consumes state before contacting the provider or
   modifying accounts. Never automatically retry this POST, including after a
   timeout or an upstream failure; begin a new transaction instead.

State and binding cookies are independently generated from 32 CSPRNG bytes
(64 lowercase hex characters). Redis stores only their SHA-256 digests, plus
provider, upstream client ID, fixed redirect URI and purpose `login`. State
uses `SET NX EX 600` and an atomic compare-before-delete Lua operation. A wrong
browser or provider cannot consume a legitimate transaction. Redis outages,
malformed records, randomness failures, expiry and replay fail closed; there is
no stateless or production-memory fallback. Redis operations have a five-second
completion bound; a command can still finish after timeout, so restart the flow.

| Failure | HTTP / error code |
| --- | --- |
| Missing code/state | 400 `VALIDATION_MISSING_REQUIRED_FIELD` |
| Invalid body/binding, expired, mismatched or replayed state | 400 `VALIDATION_INVALID_INPUT` |
| Missing/disallowed Origin or explicit cross-site fetch | 403 `AUTHZ_ACCESS_DENIED` |
| Unknown/disabled provider | 404 `VALIDATION_RESOURCE_NOT_FOUND` |
| State storage or randomness unavailable | 500 `INTERNAL_ERROR` |

### Deployment and callback configuration

Configure exact portal origins in `custom_config.cors.allow_origins`; wildcard
origins are not accepted for social transactions. The portal and API must be
same-site for the Lax cookie to accompany the callback exchange. Prefer serving
them behind one HTTPS origin. A cross-site portal/API setup is unsupported;
`withCredentials` does not override SameSite policy.

Production uses `__Host-fulla-social-binding; Secure; HttpOnly; SameSite=Lax;
Path=/; Max-Age=600` without Domain. Issuing another state renews the same binding
value, preserving other pending transactions. Only non-production with a
configured HTTP loopback portal may use `fulla-social-binding-dev` without
Secure. `FULLA_ENV=production` always selects the production cookie, including
when loaded from .env. Do not configure insecure public portals.

The callback is resolved once from `external_auth.{provider}.redirect_uri`, else
`frontend.url` + `/callback/{provider}`. Enabled providers must have distinct
canonical callback origin/path pairs; changing only query parameters does not
make callbacks distinct. Production callbacks require HTTPS. Encoded, dot-segment
and ambiguous callback paths are rejected at startup. Register exactly the same
URI with the upstream provider; Google authorization and token exchange use the
same resolved URI. This state defense does not claim full OAuth Security BCP,
PKCE, issuer validation or account-policy completion.

### Linking, tokens and logging

Account linking keeps its own authenticated server-side state protocol. Before
navigating from the security page, save a per-state `purpose: 'link'` context.
The shared callback restores the authenticated session before calling the link
endpoint. Login state never implies linking. Upgrades invalidate old in-flight
callbacks without local context; users restart the flow. Access tokens remain
in memory; neither state handling nor linking adds persistent access tokens.

Issuance, exchange and callback documents use `Cache-Control: no-store`.
The bundled user and gateway Nginx access logs omit query strings and Referer;
callback locations suppress error-log request dumps, and `Referrer-Policy:
no-referrer` is retained. Operators must independently remove callback queries,
request bodies, Cookie/Set-Cookie and tokens from external ingress, CDN, APM,
tracing and browser analytics. Do not enable raw request capture to debug login.

### Upgrade / rollback (next MINOR)

This deliberately tightens an existing endpoint for security. Deploy matching
backend and frontend together, purge old SPA caches, configure exact CORS origins,
Redis and HTTPS, then verify 204 → 200 → one successful exchange, replay rejection
and linking. Generated SDK consumers must retain cookies and send Origin, code
and state; discovery's empty deprecated field is not a navigation URL. A failed
rollout may disable social providers or roll forward with the paired build.
Do not restore stateless login. No database migration, tag or release is required
by this change.

## GitHub (fully wired, mainline)

- Backend route: `POST /api/github/login`; the `frontends/user` frontend renders the
  "Sign in with GitHub" button from the runtime discovery endpoint
  `GET /api/auth/providers` (v1.4.0 provider tiers — the button tracks the
  server-side `external_auth` gate, no frontend build variable involved).
- Callback: `/callback/github`.

## Google (wired, #70)

1. Create an OAuth 2.0 client on the Google Cloud side (Web application; authorized redirect URI = `<portal origin>/callback/google`).
2. Backend configuration (`config.json`):

```json
"external_auth": {
    "google": {
        "client_id": "<your-client-id>",
        "client_secret": "<your-client-secret>",
        "redirect_uri": "<portal origin>/callback/google"
    }
}
```

3. `POST /api/google/login` (`code` parameter) completes the exchange and returns the token pair (see above).
4. Frontend: the login page renders "Sign in with Google" when the discovery endpoint reports Google enabled (unconfigured = hidden); the `/callback/google` route (generalized `SocialCallbackPage`) stores the tokens and lands the user on the home page.

## WeChat (login wired; QR scan needs a mobile-agent surface)

1. Create a website application on the WeChat Open Platform (**localhost callbacks are not supported**; an ICP-registered domain is required).
2. Backend configuration follows the same structure (`external_auth.wechat`: appid / app_secret).
3. Backend route: `POST /api/wechat/login` — same closed loop; WeChat supplies no email, the created account mirrors GitHub's empty-email handling.
4. Frontend: `/callback/wechat` exists; the desktop SPA only surfaces a hint when the discovery endpoint reports WeChat enabled — i.e. the domestic tier (`external_auth.tiers.domestic`, default on) AND credentials configured (the QR-scan authorization flow requires a WeChat-enabled device/browser surface — the desktop button-and-redirect UX cannot complete it; the explicit desktop QR flow is a registered follow-up).
5. Local development tricks: point the callback domain to 127.0.0.1 via the hosts file / an Nginx reverse proxy / an intranet tunnel.

## General Security Notes

- The `state` on social callbacks must be verified (CSRF protection); the subject returned by a provider is trusted only
  from the server-side code exchange result — never trust user information submitted by the frontend.
- Unlinking a social account has "last login method" protection and a known limitation around concurrent-unlink races
  (see the social-link design archived under `docs/history` and the CHANGELOG #54/#69 fix records).

> Merged from the retired google-guide.md and wechat-guide.md (docs governance A2), and fixed the self-contradiction
> in the Google guide where "there was no frontend button, yet users were told to click the button". #70 closed the
> loop for Google/WeChat and rewrote this page around the uniform account/issuance model.
