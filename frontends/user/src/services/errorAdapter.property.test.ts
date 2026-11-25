// Feature: error-code-message-standardization, Property 11: 前端规范化全域性与字段不变量
//
// Property 11 — 前端规范化全域性与字段不变量
// Validates: Requirements 8.1, 8.5, 9.5
//
// 对任意输入值（包括任意 axios 错误对象、畸形响应体，乃至无 HTTP 响应的
// 网络/超时错误），`normalizeError` 永不抛出异常，且返回的规范化结构满足：
//   - `code` 为字符串
//   - `message` 为非空字符串
//   - `request_id` 为字符串（无该值时为空字符串）
//   - `httpStatus` 为数字
// 当输入为无 HTTP 响应的 axios 错误（网络/超时）时 `code` 为网络类回退码且
// `httpStatus` 为 0；当输入为无响应的非 axios 值（前端自身抛出的异常）
// 时 `code` 为客户端回退码 `__client__`；
// 当响应体含 Request_ID 时规范化结构的 `request_id` 等于该取值。
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import fc from 'fast-check'
import { normalizeError } from './errorAdapter'
import { getErrorMessage, CLIENT_CODE, NETWORK_CODE, UNKNOWN_CODE } from './messages'
import { zhCN } from './messages/zh-CN'
import { en } from './messages/en'

const RUNS = { numRuns: 100 } as const

describe('Property 11: 前端规范化全域性与字段不变量', () => {
  // The catalog logs `console.warn` for unmapped codes and normalizeError
  // logs `console.error` for frontend-side exceptions (both expected for
  // random inputs). Silence them so the property runs stay quiet and
  // deterministic.
  beforeEach(() => {
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    vi.spyOn(console, 'error').mockImplementation(() => {})
  })
  afterEach(() => {
    vi.restoreAllMocks()
  })

  // 8.1 — totality + field invariants over arbitrary inputs (incl. malformed
  // bodies and non-error values). normalizeError must never throw.
  it('never throws and returns well-formed fields for any input', () => {
    fc.assert(
      fc.property(
        fc.anything(),
        fc.option(fc.string(), { nil: undefined }),
        (input, locale) => {
          const r = normalizeError(input, locale)
          expect(typeof r.code).toBe('string')
          expect(typeof r.message).toBe('string')
          expect(r.message.length).toBeGreaterThan(0)
          expect(typeof r.request_id).toBe('string')
          expect(typeof r.httpStatus).toBe('number')
        },
      ),
      RUNS,
    )
  })

  // 8.5 — no HTTP response (network failure / timeout): axios leaves
  // `error.response` absent or undefined AND marks the error with
  // `isAxiosError: true`. code === NETWORK_CODE, httpStatus 0.
  // (A response-less NON-axios value is a frontend
  // exception → CLIENT_CODE, see the next case.)
  it('maps responseless axios (network/timeout) errors to the network fallback with httpStatus 0', () => {
    // axios network errors: `isAxiosError: true` with NO `response`
    // property, or with `response` explicitly undefined / non-object.
    const networkErrorArb = fc.oneof(
      // typical axios network error shape (no `response` key at all)
      fc.record(
        {
          isAxiosError: fc.constant(true),
          message: fc.string(),
          code: fc.constantFrom('ECONNABORTED', 'ERR_NETWORK', 'ETIMEDOUT'),
          name: fc.constant('AxiosError'),
        },
        { requiredKeys: ['isAxiosError'] },
      ),
      // explicit `response: undefined`
      fc.record({
        isAxiosError: fc.constant(true),
        response: fc.constant(undefined),
      }),
      // `response` present but not an object (treated as no response)
      fc.record({
        isAxiosError: fc.constant(true),
        response: fc.oneof(fc.constant(null), fc.string(), fc.integer()),
      })
    )

    fc.assert(
      fc.property(networkErrorArb, (err) => {
        const r = normalizeError(err)
        expect(r.code).toBe(NETWORK_CODE)
        expect(r.httpStatus).toBe(0)
        expect(r.request_id).toBe('')
        expect(r.message.length).toBeGreaterThan(0)
      }),
      RUNS,
    )
  })

  // 8.5 — a response-less value that is NOT an axios error
  // (a plain Error / arbitrary object thrown by frontend code BEFORE any
  // request was sent) must NOT be reported as a network failure; it maps to
  // CLIENT_CODE with httpStatus 0 and a non-empty message.
  it('maps responseless non-axios values (frontend exceptions) to the client fallback', () => {
    const frontendErrorArb = fc.oneof(
      fc.constant(new Error('PKCE setup failed')),
      fc.string(),
      fc.record({
        message: fc.string(),
        response: fc.constant(undefined),
        isAxiosError: fc.constant(false),
      }),
      // arbitrary object without isAxiosError
      fc.dictionary(
        fc.string().filter((k) => k !== 'response' && k !== 'isAxiosError'),
        fc.anything(),
      ),
    )

    fc.assert(
      fc.property(frontendErrorArb, (err) => {
        const r = normalizeError(err)
        expect(r.code).toBe(CLIENT_CODE)
        expect(r.httpStatus).toBe(0)
        expect(r.request_id).toBe('')
        expect(r.message.length).toBeGreaterThan(0)
      }),
      RUNS,
    )
  })

  // 9.5 — Error Envelope shape carrying a request_id in `error.request_id`.
  it('passes through request_id from an Error Envelope body', () => {
    fc.assert(
      fc.property(
        fc.string({ minLength: 1 }), // Error_Code (non-empty → envelope branch)
        fc.string({ minLength: 1 }), // request_id (non-empty)
        fc.integer({ min: 100, max: 599 }),
        (code, requestId, status) => {
          const err = {
            response: {
              status,
              data: { error: { code, request_id: requestId } },
            },
          }
          const r = normalizeError(err)
          expect(r.code).toBe(code)
          expect(r.request_id).toBe(requestId)
          expect(r.httpStatus).toBe(status)
        },
      ),
      RUNS,
    )
  })

  // 9.5 — top-level `data.request_id` is used when the envelope `error` object
  // omits its own request_id (and also for RFC 6749 string-error bodies).
  it('passes through top-level data.request_id when error.request_id is absent', () => {
    const codeArb = fc.string({ minLength: 1 })
    const requestIdArb = fc.string({ minLength: 1 })
    const statusArb = fc.integer({ min: 100, max: 599 })

    const bodyArb = fc.oneof(
      // Error Envelope whose `error` object has NO request_id → fall back to
      // top-level data.request_id.
      requestIdArb.chain((rid) =>
        codeArb.map((code) => ({
          data: { error: { code }, request_id: rid },
          expected: rid,
        })),
      ),
      // RFC 6749 protocol body: top-level `error` is a string, request_id at
      // the data top level.
      requestIdArb.chain((rid) =>
        codeArb.map((code) => ({
          data: { error: code, request_id: rid },
          expected: rid,
        })),
      ),
    )

    fc.assert(
      fc.property(bodyArb, statusArb, ({ data, expected }, status) => {
        const r = normalizeError({ response: { status, data } })
        expect(r.request_id).toBe(expected)
        expect(typeof r.code).toBe('string')
        expect(r.message.length).toBeGreaterThan(0)
      }),
      RUNS,
    )
  })
})

// Feature: error-code-message-standardization, Property 12: 前端格式解析与本地化映射
//
// Property 12 — 前端格式解析与本地化映射
// Validates: Requirements 8.2, 8.3, 8.4, 9.2, 9.6, 12.5
//
// 对任意错误响应体：
//   - 若符合 Error Envelope（顶层对象且 `error.code` 为字符串），则规范化结构
//     的 `code` 取自 `error.code`；
//   - 若符合 RFC 6749（顶层 `error` 为字符串），则 `code` 取自顶层 `error`；
//   - 若两者都不匹配（data 非对象，或对象但 `error` 不可用），则 `code` 为通用
//     未知错误码 UNKNOWN_CODE；
//   - 在简体中文界面下，对任一 code（含未知码、以及当前语言缺失条目时回退默认
//     语言 zh-CN）均返回该 code 对应的非空本地化用户信息。
describe('Property 12: 前端格式解析与本地化映射', () => {
  // Random/unknown codes trigger `console.warn` in the catalog; silence it.
  beforeEach(() => {
    vi.spyOn(console, 'warn').mockImplementation(() => {})
  })
  afterEach(() => {
    vi.restoreAllMocks()
  })

  const statusArb = fc.integer({ min: 100, max: 599 })

  // 8.2 — Error Envelope: top-level object whose `error` is an object with a
  // non-empty string `code`. The normalized `code` MUST be taken verbatim from
  // `error.code`.
  it('takes code from error.code for Error Envelope bodies', () => {
    fc.assert(
      fc.property(
        fc.string({ minLength: 1 }), // error.code (non-empty)
        // Arbitrary extra fields alongside `code` must not change the source.
        fc.dictionary(
          fc.string().filter((k) => k !== 'code'),
          fc.anything(),
        ),
        statusArb,
        (code, extra, status) => {
          const err = {
            response: {
              status,
              data: { error: { ...extra, code } },
            },
          }
          const r = normalizeError(err)
          expect(r.code).toBe(code)
          expect(r.httpStatus).toBe(status)
          expect(r.message.length).toBeGreaterThan(0)
        },
      ),
      RUNS,
    )
  })

  // 8.3 — RFC 6749 protocol error: top-level `error` is a (non-empty) string.
  // The normalized `code` MUST be taken from that top-level `error` string,
  // independent of any optional `error_description`.
  it('takes code from the top-level error string for RFC 6749 bodies', () => {
    fc.assert(
      fc.property(
        fc.string({ minLength: 1 }), // top-level error string
        fc.option(fc.string(), { nil: undefined }), // optional error_description
        statusArb,
        (error, errorDescription, status) => {
          const data: Record<string, unknown> = { error }
          if (errorDescription !== undefined) {
            data['error_description'] = errorDescription
          }
          const r = normalizeError({ response: { status, data } })
          expect(r.code).toBe(error)
          expect(r.httpStatus).toBe(status)
          expect(r.message.length).toBeGreaterThan(0)
        },
      ),
      RUNS,
    )
  })

  // 8.4 — Malformed bodies: a body present that matches NEITHER the Error
  // Envelope NOR the RFC 6749 shape MUST yield the generic unknown code.
  it('maps malformed bodies to UNKNOWN_CODE', () => {
    // `error` values that are neither a non-empty string (RFC) nor an object
    // carrying a non-empty string `code` (Envelope).
    const unusableErrorArb = fc.oneof(
      fc.constant(null),
      fc.integer(),
      fc.boolean(),
      fc.constant(''), // empty string is NOT a non-empty string
      fc.array(fc.anything()),
      // object whose `code` is absent or not a non-empty string
      fc.record(
        {
          code: fc.oneof(
            fc.constant(undefined),
            fc.integer(),
            fc.constant(''),
            fc.boolean(),
          ),
        },
        { requiredKeys: [] },
      ),
    )

    const malformedDataArb = fc.oneof(
      // data is not an object at all (string / number / null / array)
      fc.string(),
      fc.integer(),
      fc.constant(null),
      fc.array(fc.anything()),
      // data is an object but its `error` is unusable
      unusableErrorArb.map((errVal) => ({ error: errVal })),
      // data is an object without any `error` key
      fc.dictionary(
        fc.string().filter((k) => k !== 'error'),
        fc.anything(),
      ),
    )

    fc.assert(
      fc.property(malformedDataArb, statusArb, (data, status) => {
        const r = normalizeError({ response: { status, data } })
        expect(r.code).toBe(UNKNOWN_CODE)
        expect(r.httpStatus).toBe(status)
        expect(r.message.length).toBeGreaterThan(0)
      }),
      RUNS,
    )
  })

  // 9.2 / 9.6 / 12.5 — Localization mapping: under the default active locale
  // (en, per services/locale.ts initial state) EVERY resolved code maps to a
  // non-empty localized message. A code present in the catalog maps to its
  // entry; any other code (including the reserved unknown code) falls back to
  // the unknown-code message. Passing a locale absent from the catalog falls
  // back to the en fallback table and still returns a non-empty message
  // (Requirement 9.6 language fallback). The registered zh-CN locale resolves
  // its own table (ADR-0013).
  it('returns a non-empty localized message for any code in the default and registered locales', () => {
    const knownCodes = Object.keys(zhCN)
    const codeArb = fc.oneof(
      fc.constantFrom(...knownCodes), // catalog-mapped codes
      fc.constant(UNKNOWN_CODE), // reserved unknown fallback key
      fc.constant(NETWORK_CODE), // reserved network fallback key
      fc.string({ minLength: 1 }), // arbitrary (most likely unmapped) codes
    )

    fc.assert(
      fc.property(
        codeArb,
        // A locale that is NOT registered in the catalog → must fall back to
        // the default language (zh-CN) rather than yield an empty string.
        fc.constantFrom('en-US', 'fr-FR', 'ja-JP', 'xx-ZZ'),
        (code, missingLocale) => {
          // Build an Error Envelope so `normalizeError` resolves exactly `code`.
          const err = { response: { status: 400, data: { error: { code } } } }

          // Default active locale (en): message is non-empty and matches the
          // catalog mapping (entry when present, else the unknown fallback).
          const def = normalizeError(err)
          expect(def.code).toBe(code)
          expect(def.message.length).toBeGreaterThan(0)
          const expected =
            typeof en[code] === 'string' && en[code].length > 0
              ? en[code]
              : en[UNKNOWN_CODE]
          expect(def.message).toBe(expected)

          // Registered zh-CN: resolves its own table.
          const zh = normalizeError(err, 'zh-CN')
          expect(zh.code).toBe(code)
          expect(zh.message.length).toBeGreaterThan(0)
          const expectedZh =
            typeof zhCN[code] === 'string' && zhCN[code].length > 0
              ? zhCN[code]
              : zhCN[UNKNOWN_CODE]
          expect(zh.message).toBe(expectedZh)

          // Missing locale: falls back to the en table and remains non-empty,
          // and equals the value the catalog resolves for that locale.
          const fallback = normalizeError(err, missingLocale)
          expect(fallback.code).toBe(code)
          expect(fallback.message.length).toBeGreaterThan(0)
          expect(fallback.message).toBe(getErrorMessage(code, missingLocale))
        },
      ),
      RUNS,
    )
  })
})
