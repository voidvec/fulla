// Unit: PKCE helpers for non-secure contexts —
//   * `randomId()` must be a valid RFC 4122 v4 UUID built from
//     `crypto.getRandomValues` (usable in non-secure contexts).
//   * The S256 challenge produced via the pure-JS SHA-256 fallback path must
//     equal the Web Crypto digest path for the same verifier, so degraded
//     (plain-HTTP LAN) deployments interoperate with the same codes.
import { describe, expect, it } from 'vitest'
import { base64UrlEncode, generatePkcePair, randomId } from './pkce'
import { sha256 } from './sha256'

const UUID_V4 = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/

describe('randomId', () => {
  it('produces RFC 4122 version-4 UUIDs', () => {
    for (let i = 0; i < 50; i++) {
      expect(randomId()).toMatch(UUID_V4)
    }
  })

  it('produces distinct values across calls', () => {
    const seen = new Set(Array.from({ length: 50 }, () => randomId()))
    expect(seen.size).toBe(50)
  })
})

describe('generatePkcePair S256 fallback parity', () => {
  it('pure-JS digest matches the Web Crypto digest for the same verifier', async () => {
    const pair = await generatePkcePair()
    expect(pair.method).toBe('S256')

    // Reference: the browser path (available in the Node test runner's
    // secure context).
    const digest = await crypto.subtle.digest(
      'SHA-256',
      new TextEncoder().encode(pair.verifier),
    )
    const viaSubtle = base64UrlEncode(new Uint8Array(digest))

    // Fallback path: what pkce.computeS256Challenge uses when crypto.subtle
    // is undefined.
    const viaFallback = base64UrlEncode(
      sha256(new TextEncoder().encode(pair.verifier)),
    )

    expect(viaFallback).toBe(viaSubtle)
    expect(pair.challenge).toBe(viaSubtle)
  })

  it('keeps the verifier in the RFC 7636 range', async () => {
    const pair = await generatePkcePair()
    expect(pair.verifier.length).toBeGreaterThanOrEqual(43)
    expect(pair.verifier.length).toBeLessThanOrEqual(128)
    expect(pair.verifier).toMatch(/^[A-Za-z0-9\-._~]+$/)
  })
})
