import { beforeEach, describe, expect, it, vi } from 'vitest'
// vitest 5 isolation: the vi.hoisted() values referenced inside the vi.mock
// factory are structurally cloned into the mock registration graph, so the
// `create` spy the factory calls is NOT the instance this file asserts on
// (call counts never propagate back). The axios.create ARGUMENTS are recorded
// on globalThis instead — one object identity shared across both graphs.
const { post, create, remember } = vi.hoisted(() => {
  const post = vi.fn()
  const createCalls: unknown[] = ((globalThis as Record<string, unknown>).__socialCreateCalls ??= []) as unknown[]
  return {
    post,
    create: vi.fn((cfg: unknown) => { createCalls.push(cfg); return { post } }),
    remember: vi.fn(),
  }
})
vi.mock('axios', () => ({ default: { create } }))
vi.mock('../../src/services/socialFlowContext', () => ({ rememberSocialFlow: remember }))
import { beginSocialLogin, exchangeSocialLogin } from '../../src/services/socialLogin'
const response = { status: 200, data: { state: 'a'.repeat(64), authorize_url: 'https://github.com/login/oauth/authorize', redirect_uri: 'https://portal.test/callback/github' } }
beforeEach(() => { post.mockReset(); remember.mockReset() })
describe('credentialed social client', () => {
  it('uses a dedicated credentialed client without interceptors', () => {
    // Assert on the globalThis record (see the hoisted comment) — the local
    // `create` reference loses call counts under vitest 5's graph isolation.
    const calls = (globalThis as Record<string, unknown[]>).__socialCreateCalls as unknown[]
    expect(calls.length).toBeGreaterThan(0)
    expect(calls[0]).toEqual(expect.objectContaining({ withCredentials: true, timeout: 15000 }))
  })
  it('retries only the initialization 204, then saves context', async () => {
    post.mockResolvedValueOnce({ status: 204 }).mockResolvedValueOnce(response)
    await expect(beginSocialLogin('github')).resolves.toContain('https://github.com/')
    expect(post).toHaveBeenCalledTimes(2)
    expect(remember).toHaveBeenCalledWith(response.data.state, { purpose: 'login', provider: 'github', redirectUri: response.data.redirect_uri })
  })
  it('stops after a second 204 when cookies are disabled', async () => {
    post.mockResolvedValue({ status: 204 })
    await expect(beginSocialLogin('github')).rejects.toMatchObject({ reason: 'cookies' })
    expect(post).toHaveBeenCalledTimes(2)
    expect(remember).not.toHaveBeenCalled()
  })
  it('does not retry a failed exchange', async () => {
    post.mockRejectedValue(new Error('timeout'))
    await expect(exchangeSocialLogin('github', 'code', response.data.state)).rejects.toThrow('timeout')
    expect(post).toHaveBeenCalledExactlyOnceWith('/api/github/login', { code: 'code', state: response.data.state })
  })
  it('does not navigate when saving context fails', async () => {
    post.mockResolvedValue(response)
    remember.mockImplementation(() => { throw new Error('storage disabled') })
    await expect(beginSocialLogin('github')).rejects.toThrow('storage disabled')
  })
})
