/**
 * Unit tests for the WebAuthn authentication conversions (UF-01).
 *
 * The encode/decode round-trips are exactly where a passkey login can break
 * silently (the base64url `-_` alphabet vs. atob, optional userHandle,
 * allowCredentials id decoding) — R2 in the remediation plan: keep the
 * conversions pure and pin them here, so e2e only has to exercise the flow.
 */
import { describe, it, expect } from 'vitest'
import {
  toPublicKeyRequestOptions,
  assertionToFinishPayload,
} from '../../src/services/webauthnService'
import { base64UrlEncode } from '../../src/utils/pkce'

// A canonical 32-byte challenge, encoded by the same encoder under test —
// built from bytes (not a hand-written literal) so the string is valid,
// canonical base64url by construction. A hand-written constant can carry
// non-zero trailing bits in its final quantum, which decode/re-encode
// legitimately re-canonicalizes (observed with the e2e fixture string).
const CHALLENGE = base64UrlEncode(
  Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + 11) & 0xff),
)

describe('toPublicKeyRequestOptions', () => {
  it('decodes the base64url challenge to bytes', () => {
    const options = toPublicKeyRequestOptions({ options: { challenge: CHALLENGE } })
    // Round-trip: decode(CHALLENGE) must re-encode to the identical string.
    expect(base64UrlEncode(new Uint8Array(options.challenge as ArrayBuffer))).toBe(CHALLENGE)
  })

  it('applies the documented defaults (UV=required, 60s timeout)', () => {
    const options = toPublicKeyRequestOptions({ options: { challenge: CHALLENGE } })
    expect(options.userVerification).toBe('required')
    expect(options.timeout).toBe(60000)
    expect(options.rpId).toBeUndefined()
    expect(options.allowCredentials).toEqual([])
  })

  it('passes server-advertised fields through verbatim', () => {
    const options = toPublicKeyRequestOptions({
      options: {
        challenge: CHALLENGE,
        rpId: 'example.com',
        timeout: 30000,
        userVerification: 'required',
        allowCredentials: [{ id: 'AAECAwQ', type: 'public-key' }],
      },
    })
    expect(options.rpId).toBe('example.com')
    expect(options.timeout).toBe(30000)
    expect(options.allowCredentials).toHaveLength(1)
    expect(base64UrlEncode(new Uint8Array(options.allowCredentials![0].id as ArrayBuffer))).toBe('AAECAwQ')
  })

  it('throws on a missing challenge (caller renders a local failure)', () => {
    expect(() => toPublicKeyRequestOptions(undefined)).toThrow()
    expect(() => toPublicKeyRequestOptions({ options: {} as never })).toThrow()
  })
})

describe('assertionToFinishPayload', () => {
  // Reusable minimal assertion mimic matching the AuthenticatorAssertionResponse shape.
  function fakeAssertion(withUserHandle: boolean): PublicKeyCredential {
    const bytes = (n: number) => new Uint8Array(n).buffer
    const response: Record<string, ArrayBuffer | undefined> = {
      authenticatorData: bytes(37),
      clientDataJSON: bytes(16),
      signature: bytes(70),
      userHandle: withUserHandle ? bytes(4) : undefined,
    }
    return {
      id: 'stub-id',
      rawId: bytes(32),
      type: 'public-key',
      response,
    } as unknown as PublicKeyCredential
  }

  it('emits the WebAuthnAssertionCredential contract fields (base64url)', () => {
    const payload = assertionToFinishPayload(fakeAssertion(true))
    expect(payload.id).toBeTruthy()
    expect(payload.rawId).toBeTruthy()
    expect(payload.id).toBe(payload.rawId)
    expect(payload.response.authenticatorData).toBeTruthy()
    expect(payload.response.clientDataJSON).toBeTruthy()
    expect(payload.response.signature).toBeTruthy()
    // base64url alphabet only (no +, /, = padding).
    for (const value of [payload.id, payload.response.authenticatorData, payload.response.signature]) {
      expect(value).toMatch(/^[A-Za-z0-9_-]+$/)
    }
  })

  it('includes userHandle only when the authenticator supplied it', () => {
    expect(assertionToFinishPayload(fakeAssertion(true)).userHandle).toBeTruthy()
    expect(assertionToFinishPayload(fakeAssertion(false)).userHandle).toBeUndefined()
  })
})
