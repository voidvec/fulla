// Unit: the shared authorize-resume decision (second WeKnora report).
//
// The router's guest guard and the login page both ask this module "does the
// URL carry an authorize request?" — the guard to let /login render, the page
// to rebuild /oauth2/authorize after the login. These cases pin the exact
// behavior the login page had before the logic moved here.
import { describe, expect, it } from 'vitest'
import {
  AUTHORIZE_CARRY_KEYS,
  buildAuthorizeResumeUrl,
  isAuthorizeResumeRequest,
} from './authorizeResume'

const request = {
  client_id: 'rp-app',
  redirect_uri: 'https://rp.example.com/cb',
  scope: 'openid profile',
  state: 'st4te123',
  response_type: 'code',
  code_challenge: 'ch4ll8910',
  code_challenge_method: 'S256',
  nonce: 'n0nce77',
  org_id: 'acme-corp',
}

function paramsOf(url: string): URLSearchParams {
  return new URLSearchParams(url.slice(url.indexOf('?') + 1))
}

describe('isAuthorizeResumeRequest', () => {
  it('is true when both the client and the response type are present', () => {
    expect(isAuthorizeResumeRequest(request)).toBe(true)
    expect(isAuthorizeResumeRequest({ client_id: 'rp-app', response_type: 'code' })).toBe(true)
  })

  it('is false without the client, the response type, or with empty values', () => {
    const { client_id: _client, response_type: _type, ...rest } = request
    expect(isAuthorizeResumeRequest(rest)).toBe(false)
    expect(isAuthorizeResumeRequest({ ...request, client_id: '' })).toBe(false)
    expect(isAuthorizeResumeRequest({ ...request, response_type: '' })).toBe(false)
  })

  it('is false for repeated (array) or null values, and for non-authorize queries', () => {
    expect(isAuthorizeResumeRequest({ ...request, client_id: ['a', 'b'] })).toBe(false)
    expect(isAuthorizeResumeRequest({ ...request, response_type: null })).toBe(false)
    expect(isAuthorizeResumeRequest({ redirect: '/settings' })).toBe(false)
    expect(isAuthorizeResumeRequest({ must_change_password: '1' })).toBe(false)
    expect(isAuthorizeResumeRequest({})).toBe(false)
  })
})

describe('buildAuthorizeResumeUrl', () => {
  it('rebuilds the authorize URL with every carried parameter preserved verbatim', () => {
    const url = buildAuthorizeResumeUrl(request)
    expect(url.startsWith('/oauth2/authorize?')).toBe(true)
    const params = paramsOf(url)
    for (const key of AUTHORIZE_CARRY_KEYS) {
      expect(params.get(key), key).toBe((request as Record<string, string>)[key])
    }
  })

  it('forwards only whitelisted keys', () => {
    const params = paramsOf(buildAuthorizeResumeUrl({
      ...request,
      redirect: 'https://evil.example.com/',
      must_change_password: '1',
      consent_csrf: 'forged',
    }))
    expect(params.has('redirect')).toBe(false)
    expect(params.has('must_change_password')).toBe(false)
    expect(params.has('consent_csrf')).toBe(false)
    expect([...params.keys()].every((key) => (AUTHORIZE_CARRY_KEYS as readonly string[]).includes(key))).toBe(true)
  })

  it('omits optional keys that are absent or empty', () => {
    const params = paramsOf(buildAuthorizeResumeUrl({ client_id: 'rp-app', response_type: 'code', state: '' }))
    expect([...params.keys()]).toEqual(['client_id', 'response_type'])
  })

  it('returns an empty string when the query is not a resumable request', () => {
    expect(buildAuthorizeResumeUrl({ client_id: 'rp-app' })).toBe('')
    expect(buildAuthorizeResumeUrl({})).toBe('')
  })
})
