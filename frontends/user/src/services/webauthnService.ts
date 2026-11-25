import axios from 'axios'
import { base64UrlEncode, base64UrlDecode } from '../utils/pkce'

/**
 * Passkey (WebAuthn) authentication service — UF-01.
 *
 * The authenticate/begin + finish pair is ANONYMOUS and session-carried: the
 * challenge lives in the browser session cookie, no Bearer token exists (or
 * is wanted) during login. A dedicated axios instance keeps the flow out of
 * the shared http.ts interceptors — a 401 here must surface as
 * AUTH_INVALID_CREDENTIALS, not trigger the silent token-refresh dance
 * (same discipline as socialLogin.ts).
 */
const webauthnHttp = axios.create({
  baseURL: import.meta.env.VITE_API_BASE_URL || '',
  timeout: 15000,
  withCredentials: true,
  headers: { 'Content-Type': 'application/json' },
})

/** GET/POST /oauth2/webauthn/authenticate/begin response (openapi.yaml:6958). */
export interface AuthenticationOptionsPayload {
  options: {
    /** base64url challenge (must be decoded before the browser API). */
    challenge: string
    rpId?: string
    timeout?: number
    userVerification?: UserVerificationRequirement
    allowCredentials?: { id: string; type?: string }[]
  }
}

/** Wire shape for /oauth2/webauthn/authenticate/finish (openapi.yaml:6970) —
 * WebAuthnAssertionCredential: all binary fields base64url, no padding. */
export interface AssertionFinishPayload {
  id: string
  rawId: string
  response: {
    authenticatorData: string
    clientDataJSON: string
    signature: string
  }
  userHandle?: string
}

/**
 * Pure conversion: server options → PublicKeyCredentialRequestOptions.
 * The challenge and every allowCredentials id arrive base64url and must be
 * decoded to bytes; everything else passes through verbatim (the server
 * verifies against exactly what it advertised — #142 policy alignment).
 * Throws on a missing challenge so the caller can render a local failure
 * instead of feeding the browser API garbage.
 */
export function toPublicKeyRequestOptions(
  payload: AuthenticationOptionsPayload | null | undefined,
): PublicKeyCredentialRequestOptions {
  const options = payload?.options
  if (!options?.challenge) throw new Error('Passkey authentication: invalid server options')
  return {
    challenge: base64UrlDecode(options.challenge),
    rpId: options.rpId,
    timeout: options.timeout ?? 60000,
    userVerification: options.userVerification ?? 'required',
    allowCredentials: (options.allowCredentials || []).map((e) => ({
      id: base64UrlDecode(e.id),
      type: (e.type || 'public-key') as PublicKeyCredentialType,
    })),
  }
}

/**
 * Pure conversion: browser assertion → the finish wire payload. userHandle
 * is optional per contract (present only when the authenticator supplied
 * it); omitting the key entirely (vs. sending null) matches the schema.
 */
export function assertionToFinishPayload(credential: PublicKeyCredential): AssertionFinishPayload {
  const response = credential.response as AuthenticatorAssertionResponse
  const payload: AssertionFinishPayload = {
    id: base64UrlEncode(new Uint8Array(credential.rawId)),
    rawId: base64UrlEncode(new Uint8Array(credential.rawId)),
    response: {
      authenticatorData: base64UrlEncode(new Uint8Array(response.authenticatorData)),
      clientDataJSON: base64UrlEncode(new Uint8Array(response.clientDataJSON)),
      signature: base64UrlEncode(new Uint8Array(response.signature)),
    },
  }
  if (response.userHandle) {
    payload.userHandle = base64UrlEncode(new Uint8Array(response.userHandle))
  }
  return payload
}

/** Begin: mint the session-bound challenge (send cookies). */
export async function beginPasskeyAuthentication(): Promise<AuthenticationOptionsPayload> {
  const resp = await webauthnHttp.post<AuthenticationOptionsPayload>(
    '/oauth2/webauthn/authenticate/begin',
  )
  return resp.data
}

/** Finish: verify the assertion and establish the browser session. All
 * failures answer the generic 401 AUTH_INVALID_CREDENTIALS (anti-enumeration
 * — the UI must not distinguish cause). */
export async function finishPasskeyAuthentication(payload: AssertionFinishPayload): Promise<void> {
  await webauthnHttp.post('/oauth2/webauthn/authenticate/finish', payload)
}
