import axios from 'axios'
import { sessionExpiredError } from './errorAdapter'
import type { NormalizedError } from './errorAdapter'

const http = axios.create({
  baseURL: import.meta.env.VITE_API_BASE_URL || '',
  timeout: 15000,
  headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
})

// Token management
// access_token: memory only (not persisted — reduces XSS impact)
// refresh_token: localStorage (used to silently refresh on page reload)
let accessToken: string | null = null
let refreshToken: string | null = localStorage.getItem('refresh_token')

export function setTokens(access: string, refresh?: string) {
  accessToken = access
  // RFC 6749 §6: a new refresh token replaces the old one ONLY when the
  // server actually issues one. When the response carries no refresh_token
  // field, the existing token remains valid and MUST be kept — deleting it
  // (the previously suggested fix) would force a needless logout on the
  // next page load. Leaving the state untouched also keeps the in-memory
  // token and the persisted copy consistent (no more localStorage residue
  // diverging from a nulled in-memory value).
  if (refresh) {
    refreshToken = refresh
    localStorage.setItem('refresh_token', refresh)
  }
}

export function clearTokens() {
  accessToken = null
  refreshToken = null
  localStorage.removeItem('refresh_token')
}

export function getAccessToken() { return accessToken }
export function getRefreshToken() { return refreshToken }

/**
 * Attempt to restore session from refresh_token on page load.
 * Returns true if session was restored.
 */
export async function tryRestoreSession(): Promise<boolean> {
  if (accessToken) return true
  if (!refreshToken) return false

  try {
    const CLIENT_ID = import.meta.env.VITE_CLIENT_ID || 'fulla-portal'
    const resp = await axios.post('/oauth2/token', new URLSearchParams({
      grant_type: 'refresh_token',
      refresh_token: refreshToken,
      client_id: CLIENT_ID,
    }))
    setTokens(resp.data.access_token, resp.data.refresh_token)
    return true
  } catch {
    clearTokens()
    return false
  }
}

// Request interceptor: attach Bearer token
http.interceptors.request.use((config) => {
  if (accessToken && !config.headers.Authorization) {
    config.headers.Authorization = `Bearer ${accessToken}`
  }
  return config
})

/**
 * Navigate to the login view after a failed 401 token refresh.
 *
 * Uses a lazy dynamic import of the router to avoid a static import cycle
 * (router → stores/auth → http). Falls back to a hard redirect if the
 * router cannot be resolved (e.g. very early during bootstrap).
 */
async function redirectToLogin(): Promise<void> {
  try {
    const { default: router } = await import('../router')
    if (router.currentRoute.value.name !== 'login') {
      await router.push({ name: 'login' })
    }
  } catch {
    window.location.href = '/login'
  }
}

// Endpoints whose 401 must NOT trigger the silent refresh-and-retry. These are
// the credential-bearing calls themselves (retrying them with a refreshed
// token would be wrong or pointless) — everything else, including Bearer-
// protected resources like /oauth2/userinfo and /api/*, auto-refreshes.
//
// Gap-fix: the previous blanket test (`url.includes('/oauth2/')`) excluded
// userinfo from the refresh path, so an expired access token nulled the user
// in the UI ("User" placeholder) until some unrelated /api/* request happened
// to 401 first.
const NO_AUTO_REFRESH_ENDPOINTS = [
  '/oauth2/login',
  '/oauth2/token',
  '/oauth2/mfa/verify',
  '/oauth2/revoke',
]

function isNoAutoRefreshEndpoint(url: string | undefined): boolean {
  return !!url && NO_AUTO_REFRESH_ENDPOINTS.some((endpoint) => url.includes(endpoint))
}

// Response interceptor: auto-refresh on 401
http.interceptors.response.use(
  (response) => response,
  async (error) => {
    const originalRequest = error.config
    if (error.response?.status === 401 && refreshToken && !originalRequest._retry && !isNoAutoRefreshEndpoint(originalRequest.url)) {
      originalRequest._retry = true
      try {
        const CLIENT_ID = import.meta.env.VITE_CLIENT_ID || 'fulla-portal'
        const resp = await axios.post('/oauth2/token', new URLSearchParams({
          grant_type: 'refresh_token',
          refresh_token: refreshToken,
          client_id: CLIENT_ID,
        }))
        setTokens(resp.data.access_token, resp.data.refresh_token)
        originalRequest.headers.Authorization = `Bearer ${resp.data.access_token}`
        return http(originalRequest)
      } catch {
        // Token refresh failed: clear the session, surface a consistent
        // localized "session expired" message via the Frontend_Error_Module,
        // and navigate to the login view (Requirement 10.4, 10.5).
        clearTokens()
        const normalized: NormalizedError = sessionExpiredError()
        redirectToLogin()
        return Promise.reject(normalized)
      }
    }
    // All other errors: reject the raw axios error untouched so each view (or
    // the auth store) parses it once through the Frontend_Error_Module via
    // normalizeError(e), displaying a consistent localized message without
    // reading raw e.response.data.* (Requirement 10.1, 10.3). The branded
    // session-expired NormalizedError above passes through normalizeError
    // unchanged (idempotent), so views can uniformly call normalizeError(e).
    return Promise.reject(error)
  }
)

export default http
