// Unit: pure-JS SHA-256 fallback pinned by the
// FIPS 180-4 example vectors. The fallback must produce byte-identical
// digests to the Web Crypto path it substitutes in non-secure contexts.
import { describe, expect, it } from 'vitest'
import { sha256 } from './sha256'

function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('')
}

describe('sha256 (pure-JS fallback)', () => {
  // FIPS 180-4 example results (single-block and multi-block messages).
  it('matches the FIPS 180-4 vectors', () => {
    expect(toHex(sha256(new TextEncoder().encode(''))))
      .toBe('e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855')
    expect(toHex(sha256(new TextEncoder().encode('abc'))))
      .toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
    // 416-bit message — spans the one-block padding boundary exercises.
    expect(toHex(sha256(new TextEncoder().encode(
      'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
    ))))
      .toBe('248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1')
    // Million 'a' — exercises the 64-bit length high word (bitLen ≥ 2^32).
    expect(toHex(sha256(new Uint8Array(1_000_000).fill(97))))
      .toBe('cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0')
  })
})
