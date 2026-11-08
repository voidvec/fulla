import { beforeEach, describe, expect, it, vi } from 'vitest'
import { rememberSocialFlow, takeSocialFlow, captureSocialCallback, takeSocialCallback } from '../../src/services/socialFlowContext'

class StorageFake {
  entries = new Map<string, string>()
  get length() { return this.entries.size }
  key(index: number) { return [...this.entries.keys()][index] ?? null }
  getItem(key: string) { return this.entries.get(key) ?? null }
  setItem(key: string, value: string) { this.entries.set(key, value) }
  removeItem(key: string) { this.entries.delete(key) }
}
const state = 'a'.repeat(64)
const redirectUri = 'https://portal.test/callback/github'
beforeEach(() => {
  vi.stubGlobal('sessionStorage', new StorageFake())
  vi.useFakeTimers()
  vi.setSystemTime(new Date('2026-01-01T00:00:00Z'))
})

describe('social transaction context', () => {
  it('accepts the matching flow exactly once', () => {
    rememberSocialFlow(state, { purpose: 'login', provider: 'github', redirectUri })
    expect(takeSocialFlow(state, 'github', redirectUri)?.purpose).toBe('login')
    expect(takeSocialFlow(state, 'github', redirectUri)).toBeNull()
  })
  it('does not consume another provider, origin, path or state', () => {
    rememberSocialFlow(state, { purpose: 'login', provider: 'github', redirectUri })
    expect(takeSocialFlow(state, 'google', redirectUri)).toBeNull()
    expect(takeSocialFlow(state, 'github', 'https://attacker.test/callback/github')).toBeNull()
    expect(takeSocialFlow(state, 'github', 'https://portal.test/callback/google')).toBeNull()
    expect(takeSocialFlow('b'.repeat(64), 'github', redirectUri)).toBeNull()
    expect(takeSocialFlow(state, 'github', redirectUri)?.purpose).toBe('login')
  })
  it('expires at exactly 600 seconds and rejects future timestamps', () => {
    rememberSocialFlow(state, { purpose: 'login', provider: 'github', redirectUri })
    vi.advanceTimersByTime(600_000)
    expect(takeSocialFlow(state, 'github', redirectUri)).toBeNull()
    rememberSocialFlow(state, { purpose: 'login', provider: 'github', redirectUri })
    vi.setSystemTime(new Date('2025-01-01T00:00:00Z'))
    expect(takeSocialFlow(state, 'github', redirectUri)).toBeNull()
  })
  it('preserves independent transactions and explicit link purpose', () => {
    rememberSocialFlow(state, { purpose: 'login', provider: 'github', redirectUri })
    rememberSocialFlow('link-state', { purpose: 'link', provider: 'google', redirectUri: 'https://portal.test/callback/google' })
    expect(takeSocialFlow('link-state', 'google', 'https://portal.test/callback/google')?.purpose).toBe('link')
    expect(takeSocialFlow(state, 'github', redirectUri)?.purpose).toBe('login')
  })
  it('fails before navigation if storage is unavailable', () => {
    vi.stubGlobal('sessionStorage', { get length() { throw new Error('disabled') } })
    expect(() => rememberSocialFlow(state, { purpose: 'login', provider: 'github', redirectUri })).toThrow()
    expect(takeSocialFlow(state, 'github', redirectUri)).toBeNull()
  })
  it('clears callback secrets synchronously and retains one in-memory snapshot', () => {
    const replaceState = vi.fn()
    vi.stubGlobal('window', { location: { href: redirectUri + '?code=secret&state=' + state, pathname: '/callback/github' }, history: { state: {}, replaceState } })
    captureSocialCallback()
    expect(replaceState).toHaveBeenCalledWith({}, '', '/callback/github')
    expect(takeSocialCallback().searchParams.get('code')).toBe('secret')
  })
})
