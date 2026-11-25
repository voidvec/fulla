/**
 * Frontend_Error_Module — shared, single source of truth for both
 * frontends/user and frontends/admin (Requirement 8.6).
 *
 * `normalizeError` is a pure function that accepts an axios error (or any
 * value) and ALWAYS returns a normalized structure — it NEVER throws
 * (Requirement 8.1). All field access uses optional chaining and type
 * guards so any malformed input safely falls into the generic-unknown or
 * network-fallback branch.
 *
 * Parsing priority (design §9 / Requirements 8.2–8.5):
 *   1. Error Envelope — top-level object whose `error` is an object with a
 *      string `code` → take `error.code` and `error.request_id`.
 *   2. RFC 6749 — top-level `error` is a string → take the top-level `error`.
 *   3. Has a body but matches neither → generic unknown code.
 *   4. No HTTP response → `httpStatus = 0`. An axios error (request was
 *      dispatched; network failure / timeout) maps to the network fallback
 *      code; anything else is a frontend-side exception thrown before any
 *      request was sent and maps to the client fallback `__client__`
 *      (never tell the user to "check the network" for a
 *      client bug).
 *
 * The resolved code is mapped to a localized, user-readable message via the
 * Error_Message_Catalog_FE (`getErrorMessage`). When no explicit locale is
 * passed, the active UI locale (`services/locale.ts`) is used; the fallback
 * table is English (ADR-0013).
 */
import {
  getErrorMessage,
  CLIENT_CODE,
  NETWORK_CODE,
  UNKNOWN_CODE,
} from './messages'
import { getCurrentLocale } from './locale'

/** Backend Error_Code for rate limiting (maps to the localized throttling message). */
const RATE_LIMITED_CODE = 'VALIDATION_RATE_LIMITED'

/**
 * Detect the token-endpoint rate-limit response shape: a body whose top-level
 * `error` is the string `"invalid_request"` (the RFC 6749 code the rate limiter
 * emits per F-018). Only this specific body on a 429 triggers the throttling
 * message remap; a 429 with any other error code keeps its own code/message.
 */
function isRateLimitBody(data: unknown): boolean {
  if (!isObject(data)) return false
  const err = data['error']
  return typeof err === 'string' && err === 'invalid_request'
}

export interface NormalizedError {
  /** Error_Code, OAuth2 protocol code, or a reserved fallback code. */
  code: string
  /** Non-empty localized, user-readable message. */
  message: string
  /** Request_ID when present in the response body, otherwise empty string. */
  request_id: string
  /** HTTP status code, or 0 when there is no HTTP response. */
  httpStatus: number
  /**
   * Validation `details` line from the Error Envelope (`error.details`):
   * backend-assembled field-level failure reasons. Present
   * only when the server sent it — Production_Mode deployments omit the key
   * entirely, so it is optional and views must treat it as a hint, never a
   * guarantee.
   */
  details?: string
}

/** Type guard: a non-null plain object (records, arrays excluded conceptually). */
function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null
}

/** Type guard: a non-empty string. */
function isNonEmptyString(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0
}

/** Brand marker for objects that are already NormalizedError instances. */
const NORMALIZED_BRAND = '__isNormalizedError__'

/** Type guard: an object previously produced by this module (branded). */
function isNormalizedError(value: unknown): value is NormalizedError {
  return isObject(value) && (value as Record<string, unknown>)[NORMALIZED_BRAND] === true
}

/** Attach the non-enumerable brand so `normalizeError` can pass it through. */
function brand(n: NormalizedError): NormalizedError {
  Object.defineProperty(n, NORMALIZED_BRAND, {
    value: true,
    enumerable: false,
    writable: false,
    configurable: false,
  })
  return n
}

/**
 * Normalize any (axios) error into a stable, user-facing structure.
 * Guaranteed to never throw and to always return a non-empty `message`.
 */
export function normalizeError(err: unknown, locale?: string): NormalizedError {
  // Idempotency: a value already normalized by this module (e.g. the
  // session-expired error rejected by the axios interceptor) is returned
  // as-is so views can call `normalizeError(e)` uniformly.
  if (isNormalizedError(err)) {
    return err
  }

  // Resolve locale defensively; an invalid/missing locale follows the active
  // UI locale (services/locale.ts), never a hardcoded one.
  const loc = isNonEmptyString(locale) ? locale : getCurrentLocale()

  const errObj = isObject(err) ? err : undefined
  const response = errObj?.['response']
  const responseObj = isObject(response) ? response : undefined

  // Case 4: no HTTP response at all. This
  // branch used to blanket-map EVERY response-less error onto the network
  // fallback — including plain exceptions the frontend threw BEFORE any
  // request was sent (e.g. PKCE setup failing in a non-secure context),
  // which sent integrators chasing backend/network ghosts. Only genuine
  // axios errors (request dispatched, no response: network failure /
  // timeout) keep the network code; anything else is a frontend exception →
  // CLIENT_CODE. (Requirement 8.5, split.)
  if (responseObj === undefined) {
    if (errObj?.['isAxiosError'] !== true) {
      // Frontend-side exception: keep the diagnostic channel informative
      // (console) but the user-visible message generic — no internals leak.
      console.error('[normalizeError] frontend-side exception:', err)
      return {
        code: CLIENT_CODE,
        message: getErrorMessage(CLIENT_CODE, loc),
        request_id: '',
        httpStatus: 0,
      }
    }
    return {
      code: NETWORK_CODE,
      message: getErrorMessage(NETWORK_CODE, loc),
      request_id: '',
      httpStatus: 0,
    }
  }

  const status = responseObj['status']
  const httpStatus = typeof status === 'number' ? status : 0
  const data = responseObj['data']

  // F-018: the token endpoint emits RFC 6749 `{"error":"invalid_request"}`
  // with HTTP 429 when the (ip, client_id) failure bucket overflows. The
  // generic "Missing or invalid request parameters" message for `invalid_request` is misleading
  // for a rate-limit response, so remap to the dedicated throttling message.
  // Only applies when the body's error code IS `invalid_request` — a 429
  // carrying a different code (e.g. `access_denied`) keeps that code.
  if (httpStatus === 429 && isRateLimitBody(data)) {
    const requestId = isObject(data) && isNonEmptyString(data['request_id'])
      ? data['request_id']
      : ''
    return {
      code: RATE_LIMITED_CODE,
      message: getErrorMessage(RATE_LIMITED_CODE, loc),
      request_id: requestId,
      httpStatus,
    }
  }

  if (isObject(data)) {
    const envelopeError = data['error']

    // Case 1: Error Envelope — `error` is an object with a string `code`.
    // (Requirement 8.2)
    if (isObject(envelopeError) && isNonEmptyString(envelopeError['code'])) {
      const code = envelopeError['code']
      const requestId = isNonEmptyString(envelopeError['request_id'])
        ? envelopeError['request_id']
        : isNonEmptyString(data['request_id'])
          ? data['request_id']
          : ''
      // Pass the validation details through (when the
      // server emitted them) so views can show WHICH field failed instead of
      // only the generic catalog message.
      const details = isNonEmptyString(envelopeError['details'])
        ? envelopeError['details']
        : undefined
      return {
        code,
        message: getErrorMessage(code, loc),
        request_id: requestId,
        httpStatus,
        details,
      }
    }

    // Case 2: RFC 6749 protocol error — top-level `error` is a string.
    // (Requirement 8.3)
    if (isNonEmptyString(envelopeError)) {
      const requestId = isNonEmptyString(data['request_id'])
        ? data['request_id']
        : ''
      return {
        code: envelopeError,
        message: getErrorMessage(envelopeError, loc),
        request_id: requestId,
        httpStatus,
      }
    }

    // Case 3: has a body object but matches neither shape → generic unknown.
    // (Requirement 8.4)
    const requestId = isNonEmptyString(data['request_id'])
      ? data['request_id']
      : ''
    return {
      code: UNKNOWN_CODE,
      message: getErrorMessage(UNKNOWN_CODE, loc),
      request_id: requestId,
      httpStatus,
    }
  }

  // There is an HTTP response, but the body is not a parseable object
  // (string, number, null, empty, ...) → generic unknown. (Requirement 8.4)
  return {
    code: UNKNOWN_CODE,
    message: getErrorMessage(UNKNOWN_CODE, loc),
    request_id: '',
    httpStatus,
  }
}

/**
 * Reserved Error_Code used to convey an expired/invalid session
 * (401 with a failed token refresh). Maps to the localized "Your session has expired"
 * message in the Error_Message_Catalog_FE.
 */
export const SESSION_EXPIRED_CODE = 'AUTH_TOKEN_EXPIRED'

/**
 * Build a NormalizedError describing an expired session. Shared by both
 * frontends/user and frontends/admin so the 401-refresh-failure path surfaces a
 * single, consistent localized message via the Frontend_Error_Module
 * (Requirement 10.4). Never throws.
 */
export function sessionExpiredError(locale?: string): NormalizedError {
  const loc = isNonEmptyString(locale) ? locale : getCurrentLocale()
  return brand({
    code: SESSION_EXPIRED_CODE,
    message: getErrorMessage(SESSION_EXPIRED_CODE, loc),
    request_id: '',
    httpStatus: 401,
  })
}

export default normalizeError
