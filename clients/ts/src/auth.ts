/**
 * Handwritten auth layer for the Fulla TypeScript SDK (P1-5 #282).
 *
 * Mirrors the semantics of clients/python `oauth.py` + `m2m.py` (client-sdk
 * design D5: the browser round-trip cannot be automated by an SDK, but
 * building it safely can). Zero runtime dependencies — Node >= 18.17 and
 * every evergreen browser provide `fetch` and Web Crypto natively.
 *
 * Security red lines encoded here (locked by test/auth.test.ts):
 *
 * - PKCE S256 only (RFC 7636): verifier is CSPRNG-generated, 43-128 chars
 *   of the unreserved set; `exchangeCode` rejects out-of-spec verifiers
 *   before any network call.
 * - `completeAuthorizationFlow` validates `state` (anti-CSRF, RFC 6749
 *   §10.12) and refuses error callbacks BEFORE exchanging anything.
 * - Confidential clients authenticate with HTTP Basic on every token
 *   request (F-017: the server rejects the body form); the secret never
 *   enters the form body.
 * - Refresh tokens rotate on every use (migration V008): a replayed
 *   (already-used) refresh token surfaces as `FullaAuthError` from the
 *   server's `invalid_grant` — never retried silently.
 * - `M2MClient.authedFetch` retries a 401 exactly once after a forced
 *   token refresh; a second 401 is returned to the caller (no loop).
 *
 * What this layer deliberately does NOT do: id_token signature
 * verification (JWKS). See `decodeJwtPayload` — payload decoding for
 * nonce/logout hints only. For strict JOSE validation use openid-client.
 */

/** Fetch shape; injectable for tests and custom transports. */
export type FetchLike = (input: string | URL | Request, init?: RequestInit) => Promise<Response>;

/** Error raised when a token endpoint or callback is rejected. */
export class FullaAuthError extends Error {
  /** RFC 6749 §5.2 error code (`invalid_grant`, `invalid_client`, ...) or `invalid_response` / `state_mismatch`. */
  readonly error: string;
  readonly errorDescription?: string;
  /** HTTP status when the rejection came from the network leg. */
  readonly status?: number;

  constructor(error: string, errorDescription?: string, status?: number) {
    const description = errorDescription ? ` (${errorDescription})` : '';
    const statusText = status !== undefined ? ` [HTTP ${status}]` : '';
    super(`token endpoint rejected client: ${error}${description}${statusText}`);
    this.name = 'FullaAuthError';
    this.error = error;
    this.errorDescription = errorDescription;
    this.status = status;
  }
}

// ---------------------------------------------------------------------------
// Crypto primitives (Web Crypto; CSPRNG)
// ---------------------------------------------------------------------------

function assertWebCrypto(cryptoObj: Crypto | undefined): asserts cryptoObj is Crypto {
  if (!cryptoObj?.getRandomValues || !cryptoObj.subtle) {
    throw new Error(
      'fulla-oauth2 requires Web Crypto (globalThis.crypto). ' +
        'Node >= 18.17 and all evergreen browsers provide it; older runtimes do not.',
    );
  }
}

/** base64url (RFC 4648 §5) without padding — JWT/PKCE alphabet. */
function toBase64Url(bytes: Uint8Array): string {
  let binary = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function fromBase64Url(value: string): Uint8Array {
  const padded = value.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - (value.length % 4)) % 4);
  const binary = atob(padded);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

const UNRESERVED = /^[A-Za-z0-9\-._~]+$/;

/** RFC 7636 §4.1 verifier shape: 43-128 chars from the unreserved set. */
export function isPkceVerifier(value: string): boolean {
  return value.length >= 43 && value.length <= 128 && UNRESERVED.test(value);
}

/** S256 code challenge (RFC 7636 §4.2): BASE64URL(SHA256(ASCII(verifier))). */
export async function createPkceChallenge(verifier: string): Promise<string> {
  assertWebCrypto(globalThis.crypto);
  const digest = await globalThis.crypto.subtle.digest('SHA-256', new TextEncoder().encode(verifier));
  return toBase64Url(new Uint8Array(digest));
}

/** A generated verifier plus its S256 challenge. */
export class PkcePair {
  private constructor(
    readonly verifier: string,
    readonly challenge: string,
  ) {}

  /** 48 CSPRNG bytes -> 64-char verifier (384 bits of entropy; RFC 7636 §4.1 allows 43-128). */
  static async generate(): Promise<PkcePair> {
    assertWebCrypto(globalThis.crypto);
    const bytes = globalThis.crypto.getRandomValues(new Uint8Array(48));
    const verifier = toBase64Url(bytes);
    return new PkcePair(verifier, await createPkceChallenge(verifier));
  }
}

/** CSPRNG state/nonce (256 bits, base64url). */
export function randomToken(bytes = 32): string {
  assertWebCrypto(globalThis.crypto);
  return toBase64Url(globalThis.crypto.getRandomValues(new Uint8Array(bytes)));
}

// ---------------------------------------------------------------------------
// Token plumbing
// ---------------------------------------------------------------------------

/** Successful /oauth2/token response (RFC 6749 §5.1); extra members pass through. */
export interface TokenSet {
  access_token: string;
  token_type: string;
  expires_in: number;
  refresh_token?: string;
  id_token?: string;
  scope?: string;
  [key: string]: unknown;
}

export function basicAuthHeader(clientId: string, clientSecret: string): string {
  return `Basic ${btoa(`${clientId}:${clientSecret}`)}`;
}

function normalizeBaseUrl(baseUrl: string): string {
  return baseUrl.replace(/\/+$/, '');
}

async function postForm(
  fetchImpl: FetchLike,
  url: string,
  form: Record<string, string>,
  headers: Record<string, string>,
  timeoutMs: number,
): Promise<TokenSet> {
  let response: Response;
  try {
    response = await fetchImpl(url, {
      method: 'POST',
      headers: { 'content-type': 'application/x-www-form-urlencoded', ...headers },
      body: new URLSearchParams(form).toString(),
      signal: AbortSignal.timeout(timeoutMs),
    });
  } catch (cause) {
    if (cause instanceof FullaAuthError) throw cause;
    throw new FullaAuthError('network_error', cause instanceof Error ? cause.message : String(cause));
  }
  let body: Record<string, unknown> = {};
  try {
    body = (await response.json()) as Record<string, unknown>;
  } catch {
    // fall through: non-JSON error bodies are reported as invalid_response
  }
  if (response.status === 200) {
    const accessToken = body['access_token'];
    if (typeof accessToken !== 'string' || !accessToken) {
      throw new FullaAuthError('invalid_response', '200 without access_token', response.status);
    }
    return {
      ...body,
      access_token: accessToken,
      token_type: typeof body['token_type'] === 'string' ? body['token_type'] : 'Bearer',
      expires_in: typeof body['expires_in'] === 'number' ? body['expires_in'] : 3600,
    } as TokenSet;
  }
  throw new FullaAuthError(
    typeof body['error'] === 'string' ? body['error'] : 'invalid_response',
    typeof body['error_description'] === 'string' ? body['error_description'] : undefined,
    response.status,
  );
}

/** Decode a JWT payload WITHOUT verifying the signature (nonce/logout hints only). */
export function decodeJwtPayload(jwt: string): Record<string, unknown> {
  const parts = jwt.split('.');
  if (parts.length !== 3) throw new FullaAuthError('invalid_response', 'not a compact JWT');
  try {
    return JSON.parse(new TextDecoder().decode(fromBase64Url(parts[1] as string))) as Record<string, unknown>;
  } catch {
    throw new FullaAuthError('invalid_response', 'id_token payload is not JSON');
  }
}

// ---------------------------------------------------------------------------
// Authorization-code flow (RFC 6749 §4.1 + PKCE + OIDC nonce)
// ---------------------------------------------------------------------------

export interface AuthorizationCodeFlowOptions {
  /** Present only for CONFIDENTIAL clients; sent as HTTP Basic (F-017), never in the body. */
  clientSecret?: string;
  redirectUri?: string;
  scopes?: string[];
  timeoutMs?: number;
  fetchImpl?: FetchLike;
}

/** What came back on the redirect_uri (RFC 6749 §4.1.2 / §4.1.2.1). */
export interface AuthorizationResponse {
  code?: string;
  state?: string;
  error?: string;
  errorDescription?: string;
}

/** Extract (code, state, error) from the redirected URL. */
export function parseAuthorizationResponse(callbackUrl: string): AuthorizationResponse {
  const query = callbackUrl.includes('?') ? (callbackUrl.split('?')[1] as string) : callbackUrl;
  const params = new URLSearchParams(query.split('#')[0] as string);
  const pick = (key: string): string | undefined => {
    const value = params.get(key);
    return value !== null && value !== '' ? value : undefined;
  };
  return {
    code: pick('code'),
    state: pick('state'),
    error: pick('error'),
    errorDescription: pick('error_description'),
  };
}

export interface AuthorizeUrlOptions {
  /** Anti-CSRF token (RFC 6749 §10.12). REQUIRED in practice; `completeAuthorizationFlow` enforces it. */
  state?: string;
  /** OIDC Core §3.1.2.1 nonce; compare against the id_token payload (`decodeJwtPayload`). */
  nonce?: string;
  pkce?: PkcePair;
  scopes?: string[];
  redirectUri?: string;
}

export class AuthorizationCodeFlow {
  private readonly baseUrl: string;
  private readonly clientId: string;
  private readonly clientSecret?: string;
  private readonly redirectUri?: string;
  private readonly scopes: string[];
  private readonly timeoutMs: number;
  private readonly fetchImpl: FetchLike;

  constructor(baseUrl: string, clientId: string, options: AuthorizationCodeFlowOptions = {}) {
    this.baseUrl = normalizeBaseUrl(baseUrl);
    this.clientId = clientId;
    this.clientSecret = options.clientSecret;
    this.redirectUri = options.redirectUri;
    this.scopes = options.scopes ? [...options.scopes] : [];
    this.timeoutMs = options.timeoutMs ?? 10_000;
    this.fetchImpl = options.fetchImpl ?? ((input, init) => globalThis.fetch(input as RequestInfo, init));
  }

  get authorizeUrl(): string {
    return `${this.baseUrl}/oauth2/authorize`;
  }

  get tokenUrl(): string {
    return `${this.baseUrl}/oauth2/token`;
  }

  /** Build the GET /oauth2/authorize URL to send the user's browser to. */
  buildAuthorizeUrl(options: AuthorizeUrlOptions = {}): string {
    const redirectUri = options.redirectUri ?? this.redirectUri;
    if (!redirectUri) {
      throw new FullaAuthError('invalid_request', 'redirectUri is required (server strict-matches it)');
    }
    const params = new URLSearchParams({ response_type: 'code', client_id: this.clientId, redirect_uri: redirectUri });
    const scopes = options.scopes ?? this.scopes;
    if (scopes.length > 0) params.set('scope', scopes.join(' '));
    if (options.state) params.set('state', options.state);
    if (options.nonce) params.set('nonce', options.nonce);
    if (options.pkce) {
      params.set('code_challenge', options.pkce.challenge);
      params.set('code_challenge_method', 'S256');
    }
    return `${this.authorizeUrl}?${params.toString()}`;
  }

  /** POST /oauth2/token with grant_type=authorization_code. */
  async exchangeCode(
    code: string,
    codeVerifier?: string,
    options: { redirectUri?: string } = {},
  ): Promise<TokenSet> {
    if (codeVerifier !== undefined && !isPkceVerifier(codeVerifier)) {
      throw new FullaAuthError('invalid_request', 'code_verifier must be 43-128 unreserved chars (RFC 7636 §4.1)');
    }
    const redirectUri = options.redirectUri ?? this.redirectUri;
    if (!redirectUri) {
      throw new FullaAuthError('invalid_request', 'redirectUri is required (must equal the authorize request)');
    }
    const form: Record<string, string> = {
      grant_type: 'authorization_code',
      code,
      redirect_uri: redirectUri,
    };
    if (codeVerifier !== undefined) form['code_verifier'] = codeVerifier;
    const headers: Record<string, string> = {};
    if (this.clientSecret === undefined) {
      // PUBLIC clients identify themselves in the form body (no secret exists).
      form['client_id'] = this.clientId;
    } else {
      headers['authorization'] = basicAuthHeader(this.clientId, this.clientSecret);
    }
    return postForm(this.fetchImpl, this.tokenUrl, form, headers, this.timeoutMs);
  }

  /**
   * POST /oauth2/token with grant_type=refresh_token.
   *
   * Fulla rotates refresh tokens on every use (migration V008): ALWAYS
   * persist the new `refresh_token` from the returned TokenSet. Replaying
   * an already-used token returns `invalid_grant` (surfaced as
   * FullaAuthError — never silently retried).
   */
  async refresh(refreshToken: string, options: { scopes?: string[] } = {}): Promise<TokenSet> {
    const form: Record<string, string> = { grant_type: 'refresh_token', refresh_token: refreshToken };
    const scopes = options.scopes ?? this.scopes;
    if (scopes.length > 0) form['scope'] = scopes.join(' ');
    const headers: Record<string, string> = {};
    if (this.clientSecret === undefined) {
      form['client_id'] = this.clientId;
    } else {
      headers['authorization'] = basicAuthHeader(this.clientId, this.clientSecret);
    }
    return postForm(this.fetchImpl, this.tokenUrl, form, headers, this.timeoutMs);
  }

  /**
   * Safe callback completion: state check + error check + code exchange.
   *
   * Red lines (unit-locked): a `state` mismatch or an error callback throws
   * BEFORE any network call; the code is exchanged with the SAME redirect_uri
   * and the flow's PKCE verifier. Takes the raw verifier (not the PkcePair)
   * so browser/server apps can round-trip it through a short-lived cookie.
   */
  async completeAuthorizationFlow(
    callbackUrl: string,
    expected: { state: string; codeVerifier: string; redirectUri?: string },
  ): Promise<TokenSet> {
    const parsed = parseAuthorizationResponse(callbackUrl);
    if (parsed.error !== undefined) {
      throw new FullaAuthError(parsed.error, parsed.errorDescription);
    }
    if (parsed.state !== expected.state) {
      throw new FullaAuthError('state_mismatch', 'callback state does not match the authorize request (possible CSRF)');
    }
    if (parsed.code === undefined) {
      throw new FullaAuthError('invalid_response', 'callback carries neither code nor error');
    }
    return this.exchangeCode(parsed.code, expected.codeVerifier, { redirectUri: expected.redirectUri });
  }

  /** GET /oauth2/end_session URL (OIDC RP-Initiated Logout; note the
   *  underscore — matches the discovery `end_session_endpoint`). */
  buildEndSessionUrl(options: { idTokenHint: string; postLogoutRedirectUri?: string; state?: string }): string {
    const params = new URLSearchParams({ id_token_hint: options.idTokenHint });
    if (options.postLogoutRedirectUri) params.set('post_logout_redirect_uri', options.postLogoutRedirectUri);
    if (options.state) params.set('state', options.state);
    return `${this.baseUrl}/oauth2/end_session?${params.toString()}`;
  }
}

// ---------------------------------------------------------------------------
// Machine-to-machine (client_credentials, RFC 6749 §4.4)
// ---------------------------------------------------------------------------

/** Refresh this many seconds before the advertised expiry (clock skew margin). */
export const DEFAULT_CLOCK_SKEW_SEC = 30;

export interface M2MOptions {
  scopes?: string[];
  clockSkewSec?: number;
  timeoutMs?: number;
  fetchImpl?: FetchLike;
}

/** One-shot client_credentials token fetch (no caching). */
export async function fetchClientCredentialsToken(
  baseUrl: string,
  clientId: string,
  clientSecret: string,
  options: M2MOptions = {},
): Promise<TokenSet> {
  const fetchImpl = options.fetchImpl ?? ((input, init) => globalThis.fetch(input as RequestInfo, init));
  const form: Record<string, string> = { grant_type: 'client_credentials' };
  if (options.scopes && options.scopes.length > 0) form['scope'] = options.scopes.join(' ');
  return postForm(
    fetchImpl,
    `${normalizeBaseUrl(baseUrl)}/oauth2/token`,
    form,
    { authorization: basicAuthHeader(clientId, clientSecret) },
    options.timeoutMs ?? 10_000,
  );
}

/**
 * Client-credentials client: lazy acquisition, proactive refresh under the
 * clock-skew margin, serialized refreshes, and Bearer-injecting `authedFetch`
 * with exactly one 401-triggered retry.
 */
export class M2MClient {
  private readonly tokenUrl: string;
  private readonly clientId: string;
  private readonly clientSecret: string;
  private readonly scopes: string[];
  private readonly clockSkewSec: number;
  private readonly timeoutMs: number;
  private readonly fetchImpl: FetchLike;
  private accessToken: string | undefined;
  private expiresAtSec = 0;
  private refreshChain: Promise<string> = Promise.resolve('');

  constructor(baseUrl: string, clientId: string, clientSecret: string, options: M2MOptions = {}) {
    this.tokenUrl = `${normalizeBaseUrl(baseUrl)}/oauth2/token`;
    this.clientId = clientId;
    this.clientSecret = clientSecret;
    this.scopes = options.scopes ? [...options.scopes] : [];
    this.clockSkewSec = options.clockSkewSec ?? DEFAULT_CLOCK_SKEW_SEC;
    this.timeoutMs = options.timeoutMs ?? 10_000;
    this.fetchImpl = options.fetchImpl ?? ((input, init) => globalThis.fetch(input as RequestInfo, init));
  }

  /** Valid Bearer token: cached, refreshed `clockSkewSec` before expiry. `force` bypasses the cache (post-401). */
  async getAccessToken(force = false): Promise<string> {
    const now = Date.now() / 1000;
    if (!force && this.accessToken !== undefined && this.expiresAtSec - this.clockSkewSec > now) {
      return this.accessToken;
    }
    // Serialize concurrent misses onto one token request (python m2m lock
    // semantics): later callers chain onto the in-flight promise and then
    // re-check the cache in case a forced refresh raced them.
    const run = this.refreshChain.then(async () => {
      const again = Date.now() / 1000;
      if (!force && this.accessToken !== undefined && this.expiresAtSec - this.clockSkewSec > again) {
        return this.accessToken;
      }
      const form: Record<string, string> = { grant_type: 'client_credentials' };
      if (this.scopes.length > 0) form['scope'] = this.scopes.join(' ');
      const tokenSet = await postForm(
        this.fetchImpl,
        this.tokenUrl,
        form,
        { authorization: basicAuthHeader(this.clientId, this.clientSecret) },
        this.timeoutMs,
      );
      this.accessToken = tokenSet.access_token;
      this.expiresAtSec = again + tokenSet.expires_in;
      return this.accessToken;
    });
    this.refreshChain = run.catch(() => '');
    return run;
  }

  /**
   * fetch with Bearer injection. On 401: force ONE refresh and retry once;
   * a second 401 is returned as-is (no loop — server-side revocation that
   * outlives a fresh token is the caller's problem to surface).
   */
  async authedFetch(input: string | URL | Request, init: RequestInit = {}): Promise<Response> {
    const token = await this.getAccessToken(false);
    const first = await this.fetchImpl(input, { ...init, headers: { ...initHeaders(init), authorization: `Bearer ${token}` } });
    if (first.status !== 401) return first;
    const forced = await this.getAccessToken(true);
    return this.fetchImpl(input, { ...init, headers: { ...initHeaders(init), authorization: `Bearer ${forced}` } });
  }
}

function initHeaders(init: RequestInit): Record<string, string> {
  const headers: Record<string, string> = {};
  if (init.headers) {
    const iterable =
      init.headers instanceof Headers ? init.headers.entries() : Object.entries(init.headers as Record<string, string>);
    for (const [key, value] of iterable) headers[key.toLowerCase()] = value;
  }
  return headers;
}
