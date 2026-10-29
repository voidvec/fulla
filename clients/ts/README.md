# fulla-oauth2 (TypeScript/JavaScript SDK)

Typed SDK for [Fulla](../../README.md): a generated API surface from the
single OpenAPI source of truth (`apps/server/openapi.yaml`) plus a
**handwritten auth layer** for the flows an SDK can own safely. **Zero
runtime dependencies** — Node >= 18.17 and every evergreen browser provide
`fetch` and Web Crypto natively.

```bash
npm install fulla-oauth2
```

## The two surfaces

| Import | What | Origin |
|---|---|---|
| `AuthorizationCodeFlow`, `PkcePair`, `M2MClient`, ... | Auth flows (PKCE S256 + state/nonce, refresh rotation, client-credentials) | Handwritten (`src/auth.ts`, client-sdk design D5) |
| `postOauth2Token`, `getOauth2Userinfo`, `getApiMe`, ... | Every endpoint, fully typed | Generated (`src/generated/`, pinned `@hey-api/openapi-ts` 0.99.0) |

## Authorization-code flow with PKCE (browser apps / SPAs)

```ts
import { AuthorizationCodeFlow, PkcePair, randomToken } from 'fulla-oauth2';

const flow = new AuthorizationCodeFlow('http://localhost:5555', 'fulla-portal', {
  redirectUri: 'http://localhost:8080/callback',
  scopes: ['openid', 'profile'],
});

// On your login page: keep state + PKCE in the user's session (cookie or
// sessionStorage), then redirect.
const pkce = await PkcePair.generate();
const state = randomToken();
window.location.href = flow.buildAuthorizeUrl({ state, nonce: randomToken(), pkce });

// On your callback page:
const tokens = await flow.completeAuthorizationFlow(window.location.href, { state, pkce });
// ^ throws FullaAuthError('state_mismatch') BEFORE any network call if the
//   callback state doesn't match; exchanges the code with the SAME
//   redirect_uri and the flow's PKCE verifier.
```

Rotating refresh: Fulla rotates the refresh token on every use — always
persist the new `refresh_token` from the response (`flow.refresh(old)`).

## Machine-to-machine (client_credentials)

```ts
import { M2MClient } from 'fulla-oauth2';

const m2m = new M2MClient('http://localhost:5555', 'backend-svc', process.env.FULLA_CLIENT_SECRET!, {
  scopes: ['tokens:read'],
});

// Bearer injection + proactive refresh + exactly ONE 401-triggered retry:
const resp = await m2m.authedFetch('http://localhost:5555/api/admin/tokens');
```

One-shot variant for scripts: `fetchClientCredentialsToken(baseUrl, id, secret, { scopes })`.

## Typed endpoints (generated)

```ts
import { client, getOauth2Userinfo } from 'fulla-oauth2';

client.setConfig({ baseUrl: 'http://localhost:5555' });
const { data } = await getOauth2Userinfo({
  headers: { authorization: `Bearer ${accessToken}` },
});
```

The generated tree is committed and CI-gated: `python tools/clients/regen_clients.py
--check --ts-only` fails on drift from `apps/server/openapi.yaml`. Regenerate
with `npm run generate` (or the regen script without `--check`). Generator
upgrades go through a dedicated PR (same policy as the Python/Go clients).

## Security notes

- Confidential clients authenticate with HTTP Basic (F-017); the secret
  never enters a form body.
- `completeAuthorizationFlow` enforces the state check client-side; the
  server independently enforces PKCE, redirect_uri and scope allowlists.
- This SDK does NOT verify id_token signatures (no JOSE dependency by
  design). `decodeJwtPayload` is for nonce/logout hints only — for strict
  id_token validation use `openid-client` or verify against
  `/.well-known/jwks.json` yourself.

## Development

```bash
cd clients/ts
npm install
npm run check   # tsc build + vitest
npm test        # vitest only
```

Version linkage: `package.json` `version` must equal `cmake/Version.cmake`
(enforced by `tools/clients/regen_clients.py --version-only` in release.yml).
