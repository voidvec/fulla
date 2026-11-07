export type SocialProvider = 'github' | 'google' | 'wechat'
export interface SocialFlow {
  purpose: 'login' | 'link'
  provider: SocialProvider
  redirectUri: string
  createdAt: number
}

const PREFIX = 'fulla_social_flow:'
const TTL = 600_000
let callbackUrl: string | null = null

export function isSocialProvider(value: string): value is SocialProvider {
  return value === 'github' || value === 'google' || value === 'wechat'
}

export function rememberSocialFlow(state: string, flow: Omit<SocialFlow, 'createdAt'>): void {
  if (!state || state.length > 512 ||
      (flow.purpose === 'login' && !/^[0-9a-f]{64}$/.test(state))) {
    throw new Error('Invalid social transaction')
  }
  const redirect = new URL(flow.redirectUri)
  if (!['http:', 'https:'].includes(redirect.protocol) || redirect.hash || redirect.username) {
    throw new Error('Invalid social callback')
  }
  // Storage failure prevents navigation: a callback without local context must
  // never be treated as a different kind of flow.
  for (let index = sessionStorage.length - 1; index >= 0; index--) {
    const key = sessionStorage.key(index)
    if (!key?.startsWith(PREFIX)) continue
    try {
      const old: SocialFlow = JSON.parse(sessionStorage.getItem(key) || 'null')
      if (!old || Date.now() - old.createdAt >= TTL) sessionStorage.removeItem(key)
    } catch { sessionStorage.removeItem(key) }
  }
  sessionStorage.setItem(PREFIX + state, JSON.stringify({ ...flow, createdAt: Date.now() }))
}

export function takeSocialFlow(state: string, provider: SocialProvider, actualUri: string): SocialFlow | null {
  if (!state || state.length > 512) return null
  try {
    const raw = sessionStorage.getItem(PREFIX + state)
    if (!raw) return null
    const flow: SocialFlow = JSON.parse(raw)
    if (!flow || !Number.isFinite(flow.createdAt) || Date.now() - flow.createdAt >= TTL ||
        flow.createdAt > Date.now()) {
      sessionStorage.removeItem(PREFIX + state)
      return null
    }
    if (flow.provider !== provider || (flow.purpose !== 'login' && flow.purpose !== 'link')) return null
    if (flow.purpose === 'login' && !/^[0-9a-f]{64}$/.test(state)) return null
    const expected = new URL(flow.redirectUri)
    const actual = new URL(actualUri)
    if (expected.origin !== actual.origin || expected.pathname !== actual.pathname) return null
    sessionStorage.removeItem(PREFIX + state)
    return flow
  } catch {
    return null
  }
}

// Called before router startup, so guards/telemetry cannot retain response
// parameters in the address bar. The one-use snapshot stays in memory only.
export function captureSocialCallback(): void {
  if (callbackUrl !== null || !/^\/callback\/(github|google|wechat)\/?$/.test(window.location.pathname)) return
  callbackUrl = window.location.href
  window.history.replaceState(window.history.state, '', window.location.pathname)
}

export function takeSocialCallback(): URL {
  captureSocialCallback()
  const result = new URL(callbackUrl || window.location.href)
  callbackUrl = null
  return result
}
