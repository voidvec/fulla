import http, { setTokens, clearTokens, getAccessToken } from './http'
import { generatePkcePair } from '../utils/pkce'
import type { LoginResult, TokenResponse } from '../types'

const CLIENT_ID = import.meta.env.VITE_CLIENT_ID || 'fulla-portal'
const REDIRECT_URI = import.meta.env.VITE_REDIRECT_URI || window.location.origin + '/callback'

/**
 * Session-storage key for the PKCE code_verifier of the SPA's own login
 * flow, stored as JSON `{ state, verifier }`.
 *
 * The plain POST-login flow (authService.login) keeps the verifier in a
 * closure across the two-step login+exchange, so it normally never needs to
 * persist. The exception is the MFA branch: the challenge may span a page
 * interaction boundary, so the verifier (paired with the login's `state`)
 * is stashed there for verifyMfa/exchangeCode.
 *
 * PR #180 review M6 (RFC 7636 §4.6): the verifier only redeems codes of the
 * flow it was minted for, so `state` is the correlation key — a stash left
 * behind by a failed/abandoned MFA login must NOT qualify a later
 * externally-initiated /callback landing.
 */
const PKCE_VERIFIER_KEY = 'pkce_code_verifier'

interface PkceStash {
  state: string
  verifier: string
}

function readPkceStash(): PkceStash | null {
  try {
    const raw = sessionStorage.getItem(PKCE_VERIFIER_KEY)
    if (!raw) return null
    const parsed = JSON.parse(raw) as Partial<PkceStash>
    if (typeof parsed.state === 'string' && typeof parsed.verifier === 'string') {
      return { state: parsed.state, verifier: parsed.verifier }
    }
    return null
  } catch {
    return null
  }
}

function writePkceStash(stash: PkceStash) {
  sessionStorage.setItem(PKCE_VERIFIER_KEY, JSON.stringify(stash))
}

function clearPkceStash() {
  sessionStorage.removeItem(PKCE_VERIFIER_KEY)
}

export const authService = {
  /**
   * U-4 (browser-e2e 2026-09-08): true when THIS SPA holds the PKCE
   * verifier FOR THIS FLOW — the stash's `state` must equal the `state` the
   * callback was reached with. PKCE is force-enabled server-side, so
   * redeeming a code without the matching verifier would only burn the
   * one-time code that belongs to the flow's real initiator (an external
   * app that drove the user through authorize). CallbackPage uses this to
   * decide exchange vs. "return to your app".
   */
  hasVerifierForState(state?: string | null): boolean {
    if (!state) return false
    const stash = readPkceStash()
    return stash !== null && stash.state === state
  },
  async login(username: string, password: string, scope = 'openid profile email'): Promise<LoginResult> {
    // PKCE (RFC 7636): generate a verifier/challenge pair so the backend's
    // `require_pkce_for_public` enforcement (F-011) does not reject the login.
    // The verifier stays in this closure — `json:'true'` makes /oauth2/login
    // return JSON (not a browser redirect), so the page does not reload and
    // the closure survives to the token-exchange step below.
    const pkce = await generatePkcePair()
    const state = crypto.randomUUID()

    const resp = await http.post('/oauth2/login', new URLSearchParams({
      username, password,
      client_id: CLIENT_ID,
      redirect_uri: REDIRECT_URI,
      scope,
      state,
      code_challenge: pkce.challenge,
      code_challenge_method: pkce.method,
      json: 'true',
    }))

    if (resp.data.mfa_required) {
      // MFA path: stash the verifier (bound to this flow's state) so
      // verifyMfa() can thread it through to the MFA code-exchange.
      writePkceStash({ state, verifier: pkce.verifier })
      return { mfaRequired: true, mfaToken: resp.data.mfa_token }
    }

    // #145: forced first-login password change. The backend refuses to issue
    // an authorization code while the account is flagged; the user changes
    // the password on the login session (POST /oauth2/password/change) and
    // signs in again.
    if (resp.data.password_change_required) {
      return { passwordChangeRequired: true }
    }

    const code = resp.data.code
    if (!code) throw new Error('No authorization code received')

    const tokenResp = await http.post<TokenResponse>('/oauth2/token', new URLSearchParams({
      grant_type: 'authorization_code',
      code,
      redirect_uri: REDIRECT_URI,
      client_id: CLIENT_ID,
      code_verifier: pkce.verifier,
    }))

    setTokens(tokenResp.data.access_token, tokenResp.data.refresh_token)
    return { success: true }
  },

  async verifyMfa(mfaToken: string, code: string): Promise<LoginResult> {
    // The PKCE verifier stashed by login()'s MFA branch — needed if the
    // backend MFA path threads the challenge through to the token exchange.
    // It is consumed only on success: clearing it on a wrong-code attempt
    // would make the retry fail PKCE (the token exchange rejects an empty
    // verifier whenever a challenge is bound to the code).
    const codeVerifier = readPkceStash()?.verifier ?? ''

    const params = new URLSearchParams({
      mfa_token: mfaToken,
      code,
      client_id: CLIENT_ID,
      redirect_uri: REDIRECT_URI,
    })
    if (codeVerifier) params.set('code_verifier', codeVerifier)

    const resp = await http.post<TokenResponse>('/oauth2/mfa/verify', params)
    if (resp.data.access_token) {
      clearPkceStash()
      setTokens(resp.data.access_token, resp.data.refresh_token)
      return { success: true }
    }
    return { error: 'MFA verification failed' }
  },

  /**
   * #145: forced first-login password change. Session-authenticated (the
   * login that returned password_change_required set the browser session);
   * no Bearer token exists at this point by design.
   */
  async changePasswordForced(oldPassword: string, newPassword: string): Promise<void> {
    await http.post('/oauth2/password/change', JSON.stringify({
      old_password: oldPassword,
      new_password: newPassword,
    }), { headers: { 'Content-Type': 'application/json' } })
  },

  /**
   * Exchange an authorization code for tokens (browser-redirect flow).
   * Called by CallbackPage after `/oauth2/authorize` redirects back with
   * `?code=...`. Only redeemable when the stashed verifier belongs to THIS
   * flow (`state` matches — RFC 7636 §4.6 correlation, PR #180 review M6);
   * the stash is consumed either way so an abandoned login cannot qualify a
   * later landing.
   */
  async exchangeCode(code: string, state?: string): Promise<void> {
    const stash = readPkceStash()
    clearPkceStash()
    if (!stash || !state || stash.state !== state) {
      throw new Error('No PKCE verifier matching this authorization flow')
    }

    const params = new URLSearchParams({
      grant_type: 'authorization_code',
      code,
      redirect_uri: REDIRECT_URI,
      client_id: CLIENT_ID,
      code_verifier: stash.verifier,
    })

    const resp = await http.post<TokenResponse>('/oauth2/token', params)
    setTokens(resp.data.access_token, resp.data.refresh_token)
  },

  async register(username: string, password: string, email: string): Promise<void> {
    await http.post('/api/register', new URLSearchParams({ username, password, email }))
  },

  async requestPasswordReset(email: string): Promise<void> {
    await http.post('/api/password-reset/request', JSON.stringify({ email }), {
      headers: { 'Content-Type': 'application/json' },
    })
  },

  async confirmPasswordReset(token: string, newPassword: string): Promise<void> {
    await http.post('/api/password-reset/confirm', JSON.stringify({ token, new_password: newPassword }), {
      headers: { 'Content-Type': 'application/json' },
    })
  },

  async logout(): Promise<void> {
    // Revoke the refresh token (RFC 7009) and end the server-side session,
    // both via fetch({keepalive:true}) so they survive page teardown
    // (navigation/tab-close), which axios would cancel mid-flight.
    try {
      const access = getAccessToken()
      const refresh = localStorage.getItem('refresh_token')
      const revokes: Promise<Response>[] = []
      // PR-review fix: the access token goes ONLY to /oauth2/logout. It used
      // to be sent concurrently to /oauth2/revoke and /oauth2/logout — but
      // logout authenticates the Bearer token (OAuth2AuthFilter validates
      // against the store), so whichever request landed first revoked the
      // other's credentials: logout would 401 and the server-side session
      // clear + backchannel RP fanout would never happen. /oauth2/logout
      // itself revokes the access token (SessionController::logout ->
      // plugin->revokeAccessToken), so a separate revoke call is redundant.
      // The refresh token is a different credential with no such conflict.
      if (access) {
        revokes.push(fetch('/oauth2/logout', {
          method: 'POST',
          headers: { Authorization: `Bearer ${access}` },
          keepalive: true,
        }))
      }
      if (refresh) {
        revokes.push(fetch('/oauth2/revoke', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({ token: refresh, client_id: CLIENT_ID }),
          keepalive: true,
        }))
      }
      await Promise.allSettled(revokes)
    } catch {} finally {
      clearTokens()
    }
  },
}
