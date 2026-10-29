// Demo wiring for the fulla-oauth2 SDK (P1-5 #282).
//
// Everything secret (tokens, transaction state) lives in httpOnly cookies;
// the browser never sees a token string. The SDK's handwritten layer does
// the protocol work — this file only configures it and (de)serializes the
// session.

import { AuthorizationCodeFlow } from 'fulla-oauth2';

export const BASE_URL = process.env.FULLA_BASE_URL ?? 'http://localhost:5555';
export const CLIENT_ID = process.env.FULLA_CLIENT_ID ?? 'fulla-portal';
export const REDIRECT_URI = process.env.FULLA_REDIRECT_URI ?? 'http://localhost:8080/callback';
// fulla validates post_logout_redirect_uri against the client's registered
// redirect family — the seeded fulla-portal client has no standalone
// post-logout URI, so reuse the callback (the openid-client interop driver
// does the same). /callback without a code just bounces home.
export const POST_LOGOUT_URI = process.env.FULLA_POST_LOGOUT_URI ?? REDIRECT_URI;
export const SCOPES = ['openid', 'profile', 'email'];

export function flow(): AuthorizationCodeFlow {
  // PUBLIC client: no secret — PKCE S256 is the client authentication.
  return new AuthorizationCodeFlow(BASE_URL, CLIENT_ID, {
    redirectUri: REDIRECT_URI,
    scopes: SCOPES,
  });
}

/** What we keep in the httpOnly session cookie. */
export interface DemoSession {
  access_token: string;
  refresh_token?: string;
  id_token?: string;
  /** epoch seconds */
  expires_at: number;
}

export const SESSION_COOKIE = 'fulla_session';
/** Short-lived authorize transaction (state + PKCE verifier), cleared at callback. */
export const TX_COOKIE = 'fulla_tx';

export interface AuthTransaction {
  state: string;
  codeVerifier: string;
}

export function encodeCookie(value: unknown): string {
  return Buffer.from(JSON.stringify(value), 'utf8').toString('base64url');
}

export function decodeCookie<T>(raw: string | undefined): T | undefined {
  if (!raw) return undefined;
  try {
    return JSON.parse(Buffer.from(raw, 'base64url').toString('utf8')) as T;
  } catch {
    return undefined;
  }
}

/** userinfo via the SDK's generated typed endpoint (server-side Bearer call). */
export async function fetchUserInfo(accessToken: string): Promise<Record<string, unknown> | undefined> {
  const response = await fetch(`${BASE_URL}/oauth2/userinfo`, {
    headers: { authorization: `Bearer ${accessToken}` },
  });
  if (!response.ok) return undefined;
  return (await response.json()) as Record<string, unknown>;
}
