import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import axios from 'axios'
import { normalizeError, sessionExpiredError } from '../services/errorAdapter'
import type { NormalizedError } from '../services/errorAdapter'
import { getErrorMessage } from '../services/messages'
import { generatePkcePair } from '../utils/pkce'

// Refresh token is persisted in sessionStorage so a page refresh can restore the
// session without re-prompting credentials. The access token stays in memory only
// (never persisted), limiting exposure to XSS. sessionStorage is scoped to the
// tab and cleared when the tab closes. See A-LOGIN-014 / A-SEC-002.
const REFRESH_TOKEN_STORAGE_KEY = 'admin_refresh_token'

function persistRefreshToken(token: string | null) {
  try {
    if (token) {
      sessionStorage.setItem(REFRESH_TOKEN_STORAGE_KEY, token)
    } else {
      sessionStorage.removeItem(REFRESH_TOKEN_STORAGE_KEY)
    }
  } catch {
    // sessionStorage may be unavailable (private mode, disabled); degrade to
    // in-memory only — login still works for the lifetime of the tab.
  }
}

function loadPersistedRefreshToken(): string | null {
  try {
    return sessionStorage.getItem(REFRESH_TOKEN_STORAGE_KEY)
  } catch {
    return null
  }
}

// Shared promise so the router guard, the app bootstrap, and any component
// mounting during the first tick all wait for the same one-shot session
// restoration. Without this, the initial navigation can run before
// restoreSession() finishes, see isAuthenticated === false, and bounce to
// /login even though the refresh token is valid. See A-LOGIN-014.
let sessionRestorePromise: Promise<boolean> | null = null

/**
 * Navigate to the login view after a failed 401 token refresh
 * (Requirement 10.5).
 *
 * Uses a lazy dynamic import of the router to avoid a static import cycle
 * (router → stores/auth → router). Falls back to a hard redirect if the
 * router cannot be resolved (e.g. very early during bootstrap).
 */
async function redirectToLogin(): Promise<void> {
  try {
    const { default: router } = await import('../router')
    if (router.currentRoute.value.name !== 'login') {
      await router.push({ name: 'login' })
    }
  } catch {
    window.location.href = '/admin/login'
  }
}

// Result shape shared by login()/verifyMfa() — the LoginPage branches on it.
// #158: `error` carries the NormalizedError (not the resolved string) so the
// banner re-translates on locale switch; plain strings keep snapshot semantics.
export interface AdminLoginResult {
  success?: boolean
  mfaRequired?: boolean
  mfaToken?: string
  passwordChangeRequired?: boolean
  error?: NormalizedError | string
}

export const useAuthStore = defineStore('auth', () => {
  // Registered redirect_uri for the fulla-admin-console client (seeded at startup); passed as the
  // login/token parameter — the json login flow never actually redirects here.
  const REDIRECT_URI = window.location.origin + '/admin/callback'

  const accessToken = ref<string | null>(null)
  const refreshToken = ref<string | null>(null)
  const user = ref<any>(null)
  // #158: catalog-backed errors are stored as NormalizedError and resolved at
  // render time (loginErrorText) so switching locale re-translates the banner;
  // plain strings (chrome copy via t()) keep snapshot semantics.
  const loginError = ref<NormalizedError | string | null>(null)
  const loginErrorText = computed(() => {
    const e = loginError.value
    if (!e) return ''
    return typeof e === 'string' ? e : getErrorMessage(e.code)
  })

  const isAuthenticated = computed(() => !!accessToken.value)

  /**
   * Shared post-token-exchange path: store tokens, load the profile, and gate
   * on the admin role. Used by both the direct login() and the MFA completion
   * verifyMfa() so the two entry points cannot drift.
   */
  async function applySession(tokenResp: { data: { access_token: string; refresh_token?: string } }): Promise<AdminLoginResult> {
    accessToken.value = tokenResp.data.access_token
    refreshToken.value = tokenResp.data.refresh_token ?? null
    persistRefreshToken(refreshToken.value)

    await fetchUserInfo()

    if (!user.value?.roles?.includes('admin')) {
      logout()
      loginError.value = 'Admin role required to access this console'
      return { error: 'forbidden' }
    }
    return { success: true }
  }

  /**
   * Direct login: POST credentials to /oauth2/login (json mode)
   * Then exchange the code for tokens.
   * This avoids the redirect-based flow for the admin SPA.
   */
  async function login(username: string, password: string): Promise<AdminLoginResult> {
    loginError.value = null
    try {
      // PKCE (RFC 7636): generate a verifier/challenge pair so the backend's
      // `require_pkce_for_public` enforcement (F-011) does not reject the
      // login. The verifier stays in this closure — `json:'true'` makes
      // /oauth2/login return JSON (not a browser redirect), so the page does
      // not reload and the closure survives to the token-exchange step below.
      const pkce = await generatePkcePair()

      // Step 1: Login and get auth code
      const loginResp = await axios.post('/oauth2/login', new URLSearchParams({
        username,
        password,
        client_id: 'fulla-admin-console',
        redirect_uri: REDIRECT_URI,
        scope: 'openid profile admin',
        state: crypto.randomUUID(),
        code_challenge: pkce.challenge,
        code_challenge_method: pkce.method,
        json: 'true',
      }), {
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      })

      if (loginResp.data.mfa_required) {
        // MFA required: the backend persists the code_challenge on the
        // session (C4 fix, SessionController) so verifyLogin can thread it
        // onto the code it generates at MFA completion. Stash our verifier
        // too — verifyMfa() must send code_verifier so the exchange passes
        // PKCE verification (RFC 7636). One-shot: read+cleared there.
        sessionStorage.setItem('pkce_code_verifier', pkce.verifier)
        return { mfaRequired: true, mfaToken: loginResp.data.mfa_token }
      }

      // #145: forced first-login password change (e.g. the bootstrap admin).
      // The backend refuses to issue an authorization code while the account
      // is flagged; changePasswordForced() replaces the password on the login
      // session and the administrator signs in again.
      if (loginResp.data.password_change_required) {
        return { passwordChangeRequired: true }
      }

      const code = loginResp.data.code
      if (!code) {
        throw new Error('No authorization code received')
      }

      // Step 2: Exchange code for tokens
      const tokenResp = await axios.post('/oauth2/token', new URLSearchParams({
        grant_type: 'authorization_code',
        code,
        redirect_uri: REDIRECT_URI,
        client_id: 'fulla-admin-console',
        code_verifier: pkce.verifier,
      }), {
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      })

      return await applySession(tokenResp)
    } catch (e: unknown) {
      // Surface a consistent localized message via the Frontend_Error_Module
      // instead of reading raw e.response.data.* (Requirements 10.2, 10.3).
      loginError.value = normalizeError(e)
      return { error: loginError.value }
    }
  }

  /**
   * Complete an MFA-challenged login (gap-fix E2): exchange the mfa_token
   * issued by /oauth2/login plus the user's TOTP code for tokens via
   * /oauth2/mfa/verify. The PKCE verifier stashed by login() is consumed here
   * one-shot — the backend threads the challenge through the code it generates
   * at MFA completion (C4), so the internal token exchange needs the matching
   * verifier (RFC 7636). Errors land in loginError so the LoginPage banner
   * shows them and the user can retry without losing the challenge state.
   */
  async function verifyMfa(mfaToken: string, code: string): Promise<AdminLoginResult> {
    // The verifier is consumed only on success: a wrong TOTP code must not
    // burn it, or the retry would hit the token exchange with an empty
    // code_verifier and fail PKCE (TokenService rejects empty verifier when a
    // challenge is bound to the code) — trading the old login loop for a
    // retry loop.
    const codeVerifier = sessionStorage.getItem('pkce_code_verifier') || ''

    const params = new URLSearchParams({
      mfa_token: mfaToken,
      code,
      client_id: 'fulla-admin-console',
      redirect_uri: REDIRECT_URI,
    })
    if (codeVerifier) params.set('code_verifier', codeVerifier)

    try {
      const resp = await axios.post('/oauth2/mfa/verify', params, {
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      })
      if (!resp.data.access_token) {
        throw new Error('MFA verification failed')
      }
      sessionStorage.removeItem('pkce_code_verifier')
      return await applySession(resp)
    } catch (e: unknown) {
      loginError.value = normalizeError(e)
      return { error: loginError.value }
    }
  }

  /**
   * #145: forced first-login password change. Session-authenticated (the
   * login that returned password_change_required set the browser session);
   * no Bearer token exists at this point by design.
   */
  async function changePasswordForced(oldPassword: string, newPassword: string): Promise<void> {
    loginError.value = null
    try {
      await axios.post('/oauth2/password/change', JSON.stringify({
        old_password: oldPassword,
        new_password: newPassword,
      }), { headers: { 'Content-Type': 'application/json' } })
    } catch (e: unknown) {
      loginError.value = normalizeError(e)
      throw e
    }
  }

  async function fetchUserInfo() {
    if (!accessToken.value) return
    try {
      const response = await axios.get('/oauth2/userinfo', {
        headers: { Authorization: `Bearer ${accessToken.value}` },
      })
      user.value = response.data
    } catch {
      logout()
    }
  }

  // In-flight refresh promise. When multiple requests 401 concurrently (common
  // on page load: userinfo + dashboard + list endpoints), all of them must share
  // a single refresh_token exchange — the backend rotates the refresh token on
  // each use, so a second concurrent exchange would hit the now-invalidated
  // token and log the user out. See A-LOGIN-014.
  let refreshInFlight: Promise<boolean> | null = null

  async function refreshAccessToken() {
    if (!refreshToken.value) return false
    if (refreshInFlight) return refreshInFlight
    refreshInFlight = (async () => {
      try {
        const resp = await axios.post('/oauth2/token', new URLSearchParams({
          grant_type: 'refresh_token',
          // Narrowed by the refreshToken.value guard above, but the closure
          // re-reads the ref (nullable) -- pin the value for the body.
          refresh_token: refreshToken.value ?? '',
          client_id: 'fulla-admin-console',
        }), {
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        })
        accessToken.value = resp.data.access_token
        refreshToken.value = resp.data.refresh_token
        persistRefreshToken(refreshToken.value)
        return true
      } catch {
        logout()
        return false
      } finally {
        refreshInFlight = null
      }
    })()
    return refreshInFlight
  }

  /**
   * Logout: revoke BOTH the access + refresh tokens server-side (RFC 7009),
   * then clear local state. The server's revoke handler (C3 fix) now actually
   * revokes a refresh token presented here (previously a silent no-op).
   *
   * C5: revokes use fetch({keepalive:true}) rather than axios so they survive
   * page teardown — browsers cancel in-flight XHR on navigation/tab-close, but
   * keepalive fetches are flushed. Errors are swallowed because logout must
   * succeed client-side regardless of backend availability.
   */
  async function logout() {
    try {
      const access = accessToken.value
      const refresh = refreshToken.value
      const requests: Promise<Response>[] = []
      if (access) {
        // #55: /oauth2/logout revokes the access token AND fans a signed
        // logout_token out to RPs with a backchannel_logout_uri (plain
        // /oauth2/revoke does not trigger backchannel notification).
        requests.push(fetch('/oauth2/logout', {
          method: 'POST',
          headers: { Authorization: `Bearer ${access}` },
          keepalive: true,
        }))
      }
      if (refresh) {
        requests.push(fetch('/oauth2/revoke', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({ token: refresh, client_id: 'fulla-admin-console' }),
          keepalive: true,
        }))
      }
      await Promise.all(requests)
    } catch {
      // Best-effort: proceed to clear local state even if revoke fails.
    }
    accessToken.value = null
    refreshToken.value = null
    user.value = null
    persistRefreshToken(null)
  }

  /**
   * Restore a previously established session after a page refresh.
   * Uses the persisted refresh token to obtain a fresh access token. If the
   * refresh fails (expired/revoked), the session is cleared silently and the
   * user is treated as unauthenticated. Returns true if the session was
   * restored. See A-LOGIN-014.
   */
  async function restoreSession(): Promise<boolean> {
    const persisted = loadPersistedRefreshToken()
    if (!persisted) return false
    refreshToken.value = persisted
    const ok = await refreshAccessToken()
    if (!ok) return false
    await fetchUserInfo()
    // Verify the restored session still has the admin role.
    return !!user.value?.roles?.includes('admin')
  }

  // Deduplicate session restoration across concurrent callers (router guard +
  // app bootstrap). Returns a shared promise; the underlying restoreSession
  // runs at most once per page load. See A-LOGIN-014.
  function ensureSessionRestored(): Promise<boolean> {
    if (!sessionRestorePromise) {
      sessionRestorePromise = restoreSession()
    }
    return sessionRestorePromise
  }

  // Axios interceptor for auto-attaching token
  axios.interceptors.request.use((config) => {
    if (accessToken.value && !config.headers.Authorization) {
      config.headers.Authorization = `Bearer ${accessToken.value}`
    }
    return config
  })

  // Axios interceptor for 401 → auto refresh, else normalize the error so
  // views always receive a NormalizedError (Requirements 10.2, 10.3, 10.4, 10.5).
  axios.interceptors.response.use(
    (response) => response,
    async (error) => {
      const originalRequest = error.config
      if (
        error.response?.status === 401 &&
        refreshToken.value &&
        originalRequest &&
        !originalRequest._retry
      ) {
        originalRequest._retry = true
        const refreshed = await refreshAccessToken()
        if (refreshed) {
          originalRequest.headers.Authorization = `Bearer ${accessToken.value}`
          return axios(originalRequest)
        }
        // Token refresh failed: clear the session, surface a consistent
        // localized "session expired" message via the Frontend_Error_Module,
        // and navigate to the login view (Requirements 10.4, 10.5). The
        // rejected value is a BRANDED NormalizedError so a view calling
        // normalizeError(e) receives it unchanged (idempotent passthrough).
        logout()
        const expired: NormalizedError = sessionExpiredError()
        loginError.value = expired.message
        redirectToLogin()
        return Promise.reject(expired)
      }
      // All other errors: reject the raw axios error untouched so each view
      // parses it through the Frontend_Error_Module via normalizeError(e),
      // displaying a consistent localized message without reading raw
      // e.response.data.* (Requirements 10.2, 10.3).
      return Promise.reject(error)
    }
  )

  return {
    accessToken,
    user,
    loginError,
    loginErrorText,
    isAuthenticated,
    login,
    verifyMfa,
    changePasswordForced,
    fetchUserInfo,
    refreshAccessToken,
    restoreSession,
    ensureSessionRestored,
    logout,
  }
})
