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
 */
async function computeS256Challenge(verifier: string): Promise<string> {
  const encoder = new TextEncoder()
  const digest = await crypto.subtle.digest('SHA-256', encoder.encode(verifier))
  return base64UrlEncode(new Uint8Array(digest))
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
export function base64UrlEncode(bytes: Uint8Array): string {
  let str = ''
  for (let i = 0; i < bytes.length; i++) str += String.fromCharCode(bytes[i])
  return btoa(str).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
}

/**
 * Base64url string → raw bytes (RFC 4648 §5). Accepts padded or unpadded
 * input and tolerates standard-base64 characters. Used to decode WebAuthn
 * server challenges (`options.challenge` / `options.user.id` are base64url
 * strings) into the ArrayBuffers the browser credential API requires —
 * `atob` alone throws on the `-_` alphabet.
 */
export function base64UrlDecode(value: string): Uint8Array<ArrayBuffer> {
  const base64 = value.replace(/-/g, '+').replace(/_/g, '/')
  const padded = base64 + '='.repeat((4 - (base64.length % 4)) % 4)
  const str = atob(padded)
  const bytes = new Uint8Array(str.length)
  for (let i = 0; i < str.length; i++) bytes[i] = str.charCodeAt(i)
  return bytes
}
