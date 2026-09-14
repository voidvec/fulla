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

## GitHub (fully wired, mainline)

- Backend route: `POST /api/github/login`; the `frontends/user` frontend has a "Sign in with GitHub" button
  (the OAuth App's client id is injected via `VITE_GITHUB_CLIENT_ID`).
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
4. Frontend: the login page renders "Sign in with Google" when `VITE_GOOGLE_CLIENT_ID` is set (unconfigured = hidden); the `/callback/google` route (generalized `SocialCallbackPage`) stores the tokens and lands the user on the home page.

## WeChat (login wired; QR scan needs a mobile-agent surface)

1. Create a website application on the WeChat Open Platform (**localhost callbacks are not supported**; an ICP-registered domain is required).
2. Backend configuration follows the same structure (`external_auth.wechat`: appid / app_secret).
3. Backend route: `POST /api/wechat/login` — same closed loop; WeChat supplies no email, the created account mirrors GitHub's empty-email handling.
4. Frontend: `/callback/wechat` exists; the desktop SPA only surfaces a hint when `VITE_WECHAT_APPID` is configured (the QR-scan authorization flow requires a WeChat-enabled device/browser surface — the desktop button-and-redirect UX cannot complete it; the explicit desktop QR flow is a registered follow-up).
5. Local development tricks: point the callback domain to 127.0.0.1 via the hosts file / an Nginx reverse proxy / an intranet tunnel.

## General Security Notes

- The `state` on social callbacks must be verified (CSRF protection); the subject returned by a provider is trusted only
  from the server-side code exchange result — never trust user information submitted by the frontend.
- Unlinking a social account has "last login method" protection and a known limitation around concurrent-unlink races
  (see the social-link design archived under `docs/history` and the CHANGELOG #54/#69 fix records).

> Merged from the retired google-guide.md and wechat-guide.md (docs governance A2), and fixed the self-contradiction
> in the Google guide where "there was no frontend button, yet users were told to click the button". #70 closed the
> loop for Google/WeChat and rewrote this page around the uniform account/issuance model.
