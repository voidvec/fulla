// Unit red lines for the handwritten auth layer (P1-5 #282). All network
// legs go through an injected fake fetch — no server, no mocks library,
// just call recording. The locked behaviors:
//
//   PKCE: RFC 7636 Appendix B test vector; verifier shape validation
//         rejects out-of-spec verifiers BEFORE any HTTP call.
//   redirect_uri: authorize URL and token exchange carry the exact same
//         string, verbatim (the server strict-matches).
//   state: mismatch / error callbacks throw before any HTTP call.
//   refresh: replayed (rotated-away) token surfaces invalid_grant; the
//         SDK never silently retries it.
//   M2M: concurrent misses hit the token endpoint once; 401 triggers
//         exactly one forced retry, a second 401 is returned as-is.

import { describe, expect, it } from 'vitest';
import {
  AuthorizationCodeFlow,
  FullaAuthError,
  M2MClient,
  PkcePair,
  createPkceChallenge,
  decodeJwtPayload,
  isPkceVerifier,
  parseAuthorizationResponse,
  randomToken,
} from '../src/auth.js';
import { buildAuthTestHarness } from './helpers.js';

describe('PKCE (RFC 7636)', () => {
  it('matches the RFC 7636 Appendix B test vector', async () => {
    const verifier = 'dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk';
    await expect(createPkceChallenge(verifier)).resolves.toBe('E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM');
  });

  it('generates 64-char verifiers from the unreserved set, challenge 43 chars', async () => {
    const pkce = await PkcePair.generate();
    expect(pkce.verifier).toMatch(/^[A-Za-z0-9\-._~]{64}$/);
    expect(pkce.challenge).toMatch(/^[A-Za-z0-9\-._~]{43}$/);
    expect(pkce.verifier).not.toBe(pkce.challenge);
    // Two draws must differ (CSPRNG sanity; 2^-384 collision odds).
    const other = await PkcePair.generate();
    expect(other.verifier).not.toBe(pkce.verifier);
  });

  it('isPkceVerifier enforces the 43-128 unreserved shape', () => {
    expect(isPkceVerifier('x'.repeat(42))).toBe(false);
    expect(isPkceVerifier('x'.repeat(43))).toBe(true);
    expect(isPkceVerifier('x'.repeat(128))).toBe(true);
    expect(isPkceVerifier('x'.repeat(129))).toBe(false);
    expect(isPkceVerifier('a'.repeat(42) + '!')).toBe(false); // reserved char
  });

  it('exchangeCode rejects an out-of-spec verifier before any HTTP call', async () => {
    const harness = buildAuthTestHarness();
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', {
      redirectUri: 'http://app.test/callback',
      fetchImpl: harness.fetch,
    });
    await expect(flow.exchangeCode('authcode', 'short')).rejects.toMatchObject({ error: 'invalid_request' });
    expect(harness.calls).toHaveLength(0);
  });
});

describe('authorize URL + redirect_uri exactness', () => {
  const REDIRECT = 'http://localhost:8080/callback?a=1&b=2#frag';

  it('carries state, nonce, S256 challenge and the exact redirect_uri verbatim', async () => {
    const pkce = await PkcePair.generate();
    const flow = new AuthorizationCodeFlow('http://idp.test/', 'web', { redirectUri: REDIRECT });
    const url = new URL(flow.buildAuthorizeUrl({ state: 'st-1', nonce: 'n-1', pkce, scopes: ['openid', 'profile'] }));
    expect(url.pathname).toBe('/oauth2/authorize');
    expect(url.searchParams.get('redirect_uri')).toBe(REDIRECT);
    expect(url.searchParams.get('state')).toBe('st-1');
    expect(url.searchParams.get('nonce')).toBe('n-1');
    expect(url.searchParams.get('code_challenge')).toBe(pkce.challenge);
    expect(url.searchParams.get('code_challenge_method')).toBe('S256');
    expect(url.searchParams.get('scope')).toBe('openid profile');
  });

  it('refuses to build an authorize URL without a redirect_uri', () => {
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web');
    expect(() => flow.buildAuthorizeUrl()).toThrow(FullaAuthError);
  });

  it('exchange sends the byte-identical redirect_uri and client_id (PUBLIC) in the form body', async () => {
    const harness = buildAuthTestHarness();
    harness.respondWithToken({ access_token: 'at', expires_in: 3600, refresh_token: 'rt' });
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', {
      redirectUri: REDIRECT,
      fetchImpl: harness.fetch,
    });
    await flow.exchangeCode('authcode', 'v'.repeat(64));
    const call = harness.calls[0];
    expect(call?.url).toBe('http://idp.test/oauth2/token');
    const body = new URLSearchParams(call?.body ?? '');
    expect(body.get('redirect_uri')).toBe(REDIRECT);
    expect(body.get('client_id')).toBe('web');
    expect(body.get('code_verifier')).toBe('v'.repeat(64));
    expect(call?.headers['authorization']).toBeUndefined();
  });

  it('confidential clients authenticate via HTTP Basic; the secret never enters the body (F-017)', async () => {
    const harness = buildAuthTestHarness();
    harness.respondWithToken({ access_token: 'at', expires_in: 3600 });
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', {
      clientSecret: 's3cret',
      redirectUri: REDIRECT,
      fetchImpl: harness.fetch,
    });
    await flow.exchangeCode('authcode', 'v'.repeat(64));
    const call = harness.calls[0];
    expect(call?.headers['authorization']).toBe(`Basic ${btoa('web:s3cret')}`);
    expect(call?.body).not.toContain('s3cret');
    expect(call?.body).not.toContain('client_id');
  });
});

describe('callback completion red lines', () => {
  it('throws state_mismatch BEFORE any HTTP call when the callback state differs', async () => {
    const harness = buildAuthTestHarness();
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', {
      redirectUri: 'http://app.test/callback',
      fetchImpl: harness.fetch,
    });
    await expect(
      flow.completeAuthorizationFlow('http://app.test/callback?code=stealed&state=evil', {
        state: 'expected',
        codeVerifier: 'v'.repeat(64),
      }),
    ).rejects.toMatchObject({ error: 'state_mismatch' });
    expect(harness.calls).toHaveLength(0);
  });

  it('propagates authorize-time errors (access_denied) without exchanging', async () => {
    const harness = buildAuthTestHarness();
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', {
      redirectUri: 'http://app.test/callback',
      fetchImpl: harness.fetch,
    });
    await expect(
      flow.completeAuthorizationFlow('http://app.test/callback?error=access_denied&state=expected', {
        state: 'expected',
        codeVerifier: 'v'.repeat(64),
      }),
    ).rejects.toMatchObject({ error: 'access_denied' });
    expect(harness.calls).toHaveLength(0);
  });

  it('exchanges the code with the flow PKCE verifier on a good callback', async () => {
    const harness = buildAuthTestHarness();
    harness.respondWithToken({ access_token: 'at', refresh_token: 'rt', id_token: 'it', expires_in: 3600 });
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', {
      redirectUri: 'http://app.test/callback',
      fetchImpl: harness.fetch,
    });
    const pkce = await PkcePair.generate();
    const tokens = await flow.completeAuthorizationFlow(
      `http://app.test/callback?code=good&state=expected`,
      { state: 'expected', codeVerifier: pkce.verifier },
    );
    expect(tokens.access_token).toBe('at');
    expect(new URLSearchParams(harness.calls[0]?.body ?? '').get('code_verifier')).toBe(pkce.verifier);
  });

  it('parseAuthorizationResponse handles query strings, fragments and empty values', () => {
    const parsed = parseAuthorizationResponse('http://a/cb?code=c1&state=s1&error_description=why');
    expect(parsed).toMatchObject({ code: 'c1', state: 's1' });
    expect(parseAuthorizationResponse('code=c&state=')).toEqual({ code: 'c' });
    expect(parseAuthorizationResponse('http://a/cb#code=in-fragment')).toEqual({});
  });
});

describe('refresh rotation (V008)', () => {
  it('a replayed refresh token surfaces invalid_grant — never silently retried', async () => {
    const harness = buildAuthTestHarness();
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', {
      redirectUri: 'http://app.test/callback',
      fetchImpl: harness.fetch,
    });
    harness.respondWithToken({ access_token: 'at-2', refresh_token: 'rt-2', expires_in: 3600 });
    const rotated = await flow.refresh('rt-1');
    expect(rotated.refresh_token).toBe('rt-2');
    // Replaying rt-1 after rotation: server says invalid_grant, exactly one call.
    harness.respondWithError(400, 'invalid_grant', 'refresh token already used');
    await expect(flow.refresh('rt-1')).rejects.toMatchObject({ error: 'invalid_grant', status: 400 });
    expect(harness.calls).toHaveLength(2);
  });

  it('surfaces non-JSON error bodies as invalid_response with the status', async () => {
    const harness = buildAuthTestHarness();
    harness.respondWithRaw(502, 'bad gateway html');
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', {
      redirectUri: 'http://app.test/callback',
      fetchImpl: harness.fetch,
    });
    await expect(flow.exchangeCode('c', 'v'.repeat(64))).rejects.toMatchObject({
      error: 'invalid_response',
      status: 502,
    });
  });
});

describe('M2MClient (client_credentials)', () => {
  it('caches the token, refreshes proactively under the clock-skew margin', async () => {
    const harness = buildAuthTestHarness();
    harness.respondWithToken({ access_token: 'm2m-at', expires_in: 60 });
    const m2m = new M2MClient('http://idp.test', 'svc', 's3cret', {
      scopes: ['tokens:read'],
      clockSkewSec: 30, // effective lifetime window: 60-30 = 30s
      fetchImpl: harness.fetch,
    });
    await expect(m2m.getAccessToken()).resolves.toBe('m2m-at');
    await expect(m2m.getAccessToken()).resolves.toBe('m2m-at'); // cached
    expect(harness.calls).toHaveLength(1);
    const body = new URLSearchParams(harness.calls[0]?.body ?? '');
    expect(body.get('grant_type')).toBe('client_credentials');
    expect(body.get('scope')).toBe('tokens:read');
    expect(harness.calls[0]?.headers['authorization']).toBe(`Basic ${btoa('svc:s3cret')}`);
  });

  it('collapses concurrent first misses into ONE token request', async () => {
    const harness = buildAuthTestHarness();
    harness.respondWithToken({ access_token: 'm2m-at', expires_in: 3600 });
    const m2m = new M2MClient('http://idp.test', 'svc', 's3cret', { fetchImpl: harness.fetch });
    const tokens = await Promise.all([m2m.getAccessToken(), m2m.getAccessToken(), m2m.getAccessToken()]);
    expect(new Set(tokens).size).toBe(1);
    expect(harness.calls).toHaveLength(1);
  });

  it('authedFetch retries a 401 exactly once after a forced refresh; second 401 passes through', async () => {
    const harness = buildAuthTestHarness();
    const api = harness.route('http://api.test/whoami');
    const m2m = new M2MClient('http://idp.test', 'svc', 's3cret', { fetchImpl: harness.fetch });

    harness.respondWithToken({ access_token: 'stale', expires_in: 3600 });
    api.queueResponses(
      { status: 401, body: { error: 'invalid_token' } },
      { status: 200, body: { sub: 'svc' } },
    );
    const ok = await m2m.authedFetch('http://api.test/whoami');
    expect(ok.status).toBe(200);
    // Exactly: 1 token fetch + 1 forced refresh + 2 API calls.
    expect(harness.tokenEndpointCalls()).toBe(2);
    expect(api.calls.map((c) => c.headers['authorization'])).toEqual(['Bearer stale', 'Bearer fresh-2']);

    // Fresh token still 401 (revoked client): returned as-is, no loop.
    api.queueResponses(
      { status: 401, body: { error: 'invalid_token' } },
      { status: 401, body: { error: 'invalid_token' } },
    );
    const denied = await m2m.authedFetch('http://api.test/whoami');
    expect(denied.status).toBe(401);
    expect(api.calls).toHaveLength(4);
    expect(harness.tokenEndpointCalls()).toBe(3);
  });
});

describe('misc helpers', () => {
  it('randomToken yields base64url entropy and differs between draws', () => {
    expect(randomToken()).toMatch(/^[A-Za-z0-9\-._~]{43}$/);
    expect(randomToken()).not.toBe(randomToken(32));
  });

  it('decodeJwtPayload decodes payload segments (no signature verification)', () => {
    const payload = { sub: 'u1', nonce: 'n-1' };
    const b64 = (value: string) => btoa(value).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
    const jwt = `${b64('{"alg":"RS256"}')}.${b64(JSON.stringify(payload))}.${b64('sig')}`;
    expect(decodeJwtPayload(jwt)).toEqual(payload);
    expect(() => decodeJwtPayload('not-a-jwt')).toThrow(FullaAuthError);
  });

  it('buildEndSessionUrl carries id_token_hint, post_logout_redirect_uri and state', () => {
    const flow = new AuthorizationCodeFlow('http://idp.test', 'web', { redirectUri: 'http://app.test/callback' });
    const url = new URL(
      flow.buildEndSessionUrl({ idTokenHint: 'jwt.jwt.jwt', postLogoutRedirectUri: 'http://app.test/', state: 'out' }),
    );
    expect(url.pathname).toBe('/oauth2/end_session');
    expect(url.searchParams.get('id_token_hint')).toBe('jwt.jwt.jwt');
    expect(url.searchParams.get('post_logout_redirect_uri')).toBe('http://app.test/');
    expect(url.searchParams.get('state')).toBe('out');
  });
});
