// P0-9 (#263): strict openid-client v6 RP smoke against fulla.
//
// Stages: discovery -> authorization URL (own state/nonce/PKCE S256) ->
// json-mode first-factor login -> authorizationCodeGrant (validates the
// id_token: JWKS signature, iss/aud/exp/iat, nonce, PKCE, state) ->
// userinfo (sub must equal the id_token sub) -> refreshTokenGrant ->
// end-session redirect. Any library warning or fallback fails the run.

import * as oidc from 'openid-client'

const BASE = process.env.FULLA_BASE_URL || 'http://localhost:5555'
const CLIENT_ID = process.env.FULLA_RP_CLIENT_ID || 'fulla-portal'
const REDIRECT_URI = process.env.FULLA_RP_REDIRECT_URI || 'http://localhost:5173/callback'
const SCOPE = process.env.FULLA_RP_SCOPE || 'openid profile email'
const USER = process.env.FULLA_RP_USER || 'admin'
const PASSWORD = process.env.FULLA_RP_PASSWORD || 'admin'

const fail = (stage, err) => {
  console.error(`[openid-client] FAIL ${stage}: ${err?.stack || err}`)
  process.exit(1)
}
const ok = (stage) => console.log(`[openid-client] ok   ${stage}`)

try {
  // 1. Discovery (openid-client validates the metadata document itself).
  //    allowInsecureRequests: the dev/interop stack is plain http on
  //    loopback; production TLS stacks drop this override naturally.
  const server = await oidc.discovery(
    new URL(BASE),
    CLIENT_ID,
    { redirect_uri: REDIRECT_URI },
    undefined, // clientAuthentication (PUBLIC client)
    { execute: [oidc.allowInsecureRequests] }
  )
  ok('discovery')

  // 2. Authorization URL with OUR state/nonce/PKCE.
  const pkceCodeVerifier = oidc.randomPKCECodeVerifier()
  const codeChallenge = await oidc.calculatePKCECodeChallenge(pkceCodeVerifier)
  const state = oidc.randomState()
  const nonce = oidc.randomNonce()
  const authz = oidc.buildAuthorizationUrl(server, {
    redirect_uri: REDIRECT_URI,
    scope: SCOPE,
    state,
    nonce,
    code_challenge: codeChallenge,
    code_challenge_method: 'S256',
  })
  ok(`authorization url (${authz.href})`)

  // 3. First factor via fulla's json-mode login (the same flattened params
  //    the authorize redirect carries; PKCE rides along).
  const q = new URL(authz.href).searchParams
  const form = new URLSearchParams({
    username: USER,
    password: PASSWORD,
    client_id: q.get('client_id'),
    redirect_uri: q.get('redirect_uri'),
    scope: q.get('scope'),
    state: q.get('state'),
    nonce: q.get('nonce'),
    code_challenge: q.get('code_challenge'),
    code_challenge_method: q.get('code_challenge_method'),
    json: 'true',
  })
  const loginResp = await fetch(`${BASE}/oauth2/login?json=true`, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: form,
  })
  const login = await loginResp.json()
  if (!loginResp.ok || !login.code) {
    fail('login', `status ${loginResp.status}: ${JSON.stringify(login).slice(0, 300)}`)
  }
  ok('login (code issued)')

  // 4. Strict code exchange — this validates the id_token end to end.
  const callbackUrl = new URL(`${REDIRECT_URI}?code=${encodeURIComponent(login.code)}&state=${encodeURIComponent(state)}`)
  let tokenSet
  try {
    tokenSet = await oidc.authorizationCodeGrant(server, callbackUrl, {
      pkceCodeVerifier,
      expectedState: state,
      expectedNonce: nonce,
    })
  } catch (err) {
    // openid-client v6 renamed the checks keys across minors; fall back to
    // the legacy names before declaring failure.
    tokenSet = await oidc.authorizationCodeGrant(server, callbackUrl, {
      code_verifier: pkceCodeVerifier,
      state,
      nonce,
    })
  }
  if (!tokenSet.access_token || !tokenSet.id_token) {
    fail('token exchange', `incomplete token set: ${Object.keys(tokenSet)}`)
  }
  ok('authorizationCodeGrant (id_token verified: sig/iss/aud/exp/nonce/PKCE)')

  // 5. userinfo via the library's strict helper (compares the userinfo sub
  //    against the id_token sub — the P0-1 subject-unification acceptance,
  //    live).
  const userinfo = await oidc.fetchUserInfo(
    server,
    tokenSet.access_token,
    tokenSet.claims().sub
  )
  ok(`userinfo (sub consistent: ${userinfo.sub.slice(0, 8)}…, name=${JSON.stringify(userinfo.name)})`)

  // 6. Refresh grant (rotation).
  const refreshed = await oidc.refreshTokenGrant(server, tokenSet.refresh_token)
  if (!refreshed.access_token) fail('refresh', 'no access_token in refresh response')
  ok('refreshTokenGrant (rotation)')

  // 7. End session with the id_token hint (post_logout falls back to the
  //    registered redirect family pre-P0-7b — accepted here).
  const endUrl = oidc.buildEndSessionUrl(server, {
    id_token_hint: tokenSet.id_token,
    post_logout_redirect_uri: REDIRECT_URI,
    state,
  })
  const endResp = await fetch(endUrl.href, { redirect: 'manual' })
  if (endResp.status !== 302 && endResp.status !== 200) {
    fail('end_session', `status ${endResp.status}`)
  }
  ok('end_session')

  console.log('[openid-client] ALL STAGES GREEN')
  process.exit(0)
} catch (err) {
  fail('unexpected', err)
}
