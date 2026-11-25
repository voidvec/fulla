/**
 * PKCE (Proof Key for Code Exchange, RFC 7636) utility.
 *
 * Used by the OAuth2 authorization-code flow to protect against
 * authorization-code interception attacks. The SPA generates a random
 * `code_verifier`, derives the `code_challenge` via S256 (SHA-256), sends
 * the challenge with the authorization request, and later proves possession
 * of the verifier at the token endpoint.
 *
 * The backend enforces PKCE for all authorization_code clients when
 * `auth.require_pkce` is true (the default), so the login flow
 * is rejected at SessionController CHECK 4 (F-011 / RFC 9700 §2.1.1) if
 * no `code_challenge` is provided.
 */
import { sha256 } from './sha256'

/**
 * Generate a high-entropy random `code_verifier` (43–128 chars).
 * Uses Web Crypto `crypto.getRandomValues` for a cryptographically secure
 * random source; 48 bytes → 64 base64url chars, within the RFC range.
 */
function generateCodeVerifier(): string {
  const bytes = new Uint8Array(48)
  crypto.getRandomValues(bytes)
  return base64UrlEncode(bytes)
}

/**
 * Compute the S256 `code_challenge` from a `code_verifier`.
 * S256: BASE64URL-ENCODE(SHA256(ASCII(code_verifier))).
 *
 * In a non-secure context (plain HTTP on a
 * LAN IP, say) Chromium leaves `crypto.subtle` undefined, which used to
 * throw BEFORE the login request was even sent — and the failure surfaced
 * as a misleading "network connection failed" message. Feature-detect and
 * fall back to the pure-JS SHA-256 (`./sha256`); the challenge stays S256,
 * never downgraded to "plain", so the backend's PKCE contract is unchanged.
 */
async function computeS256Challenge(verifier: string): Promise<string> {
  const encoder = new TextEncoder()
  const data = encoder.encode(verifier)
  if (typeof crypto !== 'undefined' && crypto.subtle &&
      typeof crypto.subtle.digest === 'function') {
    const digest = await crypto.subtle.digest('SHA-256', data)
    return base64UrlEncode(new Uint8Array(digest))
  }
  return base64UrlEncode(sha256(data))
}

/**
 * RFC 4122 version-4 UUID from `crypto.getRandomValues`.
 *
 * `crypto.randomUUID` is, like
 * `crypto.subtle`, unavailable in non-secure contexts; `getRandomValues`
 * is not. Use it wherever a random identifier is needed so flows do not
 * depend on a secure context.
 */
export function randomId(): string {
  const bytes = new Uint8Array(16)
  crypto.getRandomValues(bytes)
  bytes[6] = (bytes[6] & 0x0f) | 0x40 // version 4
  bytes[8] = (bytes[8] & 0x3f) | 0x80 // variant 10xx
  const hex = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}

/** RFC 7636 §4.2 PKCE pair: a verifier + its S256 challenge. */
export interface PkcePair {
  /** The secret verifier — kept client-side, sent only to the token endpoint. */
  verifier: string
  /** The S256 challenge — sent with the authorization/login request. */
  challenge: string
  /** Always "S256" (we do not support the weaker "plain" method). */
  method: 'S256'
}

/**
 * Generate a fresh PKCE verifier/challenge pair (method = S256).
 * Call this before initiating the authorization request, then:
 *   1. Send `challenge` + `method` with `/oauth2/login` (or `/oauth2/authorize`).
 *   2. Send `verifier` with `/oauth2/token` (grant_type=authorization_code).
 */
export async function generatePkcePair(): Promise<PkcePair> {
  const verifier = generateCodeVerifier()
  const challenge = await computeS256Challenge(verifier)
  return { verifier, challenge, method: 'S256' }
}

/** Base64url-encode raw bytes without padding (RFC 4648 §5). */
function base64UrlEncode(bytes: Uint8Array): string {
  let str = ''
  for (let i = 0; i < bytes.length; i++) str += String.fromCharCode(bytes[i])
  return btoa(str).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
}
