// Feature: error-code-message-standardization, Property 14: 错误码目录确定性与完备性
//
// Validates: Requirements 10.7
//
// 职责分工（2026-11 UF-08 复核后收口）：
//   - 跨应用逐字镜像一致性（frontends/user → frontends/admin）：由仓库级门禁
//     `scripts/check-ui-sync.mjs`（错误目录镜像一节）直接比对两侧文件守护。
//     单测不跨 app import（.claude/rules/frontend-contract.md），原有动态 import
//     回退写法会静默退化为自比，已移除。
//   - 本单测守护权威源（frontends/user）自身的两个不变量：
//       1. 对同一 (code, locale) 映射确定性（幂等）且永不返回空字符串；
//       2. 后端用户可达应用码与 OAuth2 协议码均有真实文案，不会静默回落到 `__unknown__`。
import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest'
import fc from 'fast-check'
import { DEFAULT_LOCALE, UNKNOWN_CODE, getErrorMessage, messages } from './messages'

const RUNS = { numRuns: 100 } as const

// 门户用户可能收到的全部后端应用码——libs/common/src/error/ErrorCatalog.cc 里的
// 条目，去掉下面这些（新增后端码时这里要跟着加；前端测试读不到 C++ 目录）：
//   - 仅管理端会返回：AUDIT_EXPORT_TOO_LARGE / AUDIT_EXPORT_WINDOW_REQUIRED、
//     CLAIM_MAPPING_{RESERVED_CLAIM,INVALID_SOURCE,FORBIDDEN_SCOPE}、
//     EMAIL_{TEMPLATE_NOT_FOUND,TEMPLATE_CONTENT_REJECTED,TEST_RECIPIENT_DENIED,
//     TEST_QUOTA_EXCEEDED}
//   - 以 200 分支结构化处理、不需要文案：AUTH_MFA_REQUIRED、
//     AUTH_PASSWORD_CHANGE_REQUIRED
//   - 面向发起登出的第三方 RP：AUTH_INVALID_ID_TOKEN_HINT
//   - 门户尚无对应界面（UF-06）：CUSTOM_ATTRIBUTES_INVALID
const BACKEND_ERROR_CODES = [
  'NET_CONNECTION_FAILED',
  'NET_TIMEOUT',
  'DB_CONNECTION_ERROR',
  'DB_QUERY_ERROR',
  'DB_CONSTRAINT_VIOLATION',
  'INTERNAL_ERROR',
  'AUTH_INVALID_CREDENTIALS',
  'AUTH_TOKEN_EXPIRED',
  'AUTH_TOKEN_INVALID',
  'AUTH_SESSION_REQUIRED',
  'AUTH_MFA_CODE_INVALID',
  'AUTH_MFA_NOT_CONFIGURED',
  'AUTH_SOCIAL_ACCOUNT_NOT_LINKED',
  'AUTHZ_ACCESS_DENIED',
  'AUTHZ_INSUFFICIENT_PERMISSIONS',
  'VALIDATION_INVALID_INPUT',
  'VALIDATION_MISSING_REQUIRED_FIELD',
  'VALIDATION_FORMAT_ERROR',
  'VALIDATION_PASSWORD_TOO_SHORT',
  'VALIDATION_RESOURCE_NOT_FOUND',
  'VALIDATION_RESOURCE_CONFLICT',
  'VALIDATION_USERNAME_TAKEN',
  'VALIDATION_EMAIL_TAKEN',
  'VALIDATION_CREDENTIAL_ALREADY_REGISTERED',
  'VALIDATION_RESET_TOKEN_INVALID',
  'VALIDATION_VERIFICATION_TOKEN_INVALID',
  'VALIDATION_DEVICE_CODE_INVALID',
  'VALIDATION_RATE_LIMITED',
  'VALIDATION_REDIRECT_URI_NOT_REGISTERED',
  'VALIDATION_CLIENT_REDIRECT_URI_NOT_REGISTERED',
  'WEBAUTHN_INVALID_ATTESTATION',
  'WEBAUTHN_CHALLENGE_MISMATCH',
] as const

// 完整的 OAuth2 / RFC 6749 协议错误码（design.md「OAuth2 协议错误码目录」）。
const FULLA_PROTOCOL_CODES = [
  'invalid_request',
  'invalid_client',
  'invalid_grant',
  'unauthorized_client',
  'unsupported_grant_type',
  'invalid_scope',
  'server_error',
  'temporarily_unavailable',
  'access_denied',
  'unsupported_token_type',
  'authorization_pending',
  'slow_down',
  'expired_token',
  // RFC 6750 §3.1 (WWW-Authenticate Bearer errors) + OIDC Core §3.1.2.6
  // (prompt-related authorization errors) — added during the OAuth/OIDC
  // compliance alignment.
  'invalid_token',
  'insufficient_scope',
  'login_required',
  'consent_required',
  'interaction_required',
] as const

// 保留回退键，亦应在两应用间一致映射。
const RESERVED_CODES = ['__unknown__', '__network__', '__client__'] as const

// 上面两份清单记的是后端已知会返回的码。域的另一半是目录本身：这样确定性属性覆盖
// user 目录实际有的每一个键，而不是一份会随新增码过期的手写清单。
const CATALOG_CODES = Object.keys(messages[DEFAULT_LOCALE])

const ALL_CODES = Array.from(new Set<string>([
  ...BACKEND_ERROR_CODES,
  ...FULLA_PROTOCOL_CODES,
  ...RESERVED_CODES,
  ...CATALOG_CODES,
]))

// Locales both catalogs register.
const REGISTERED_LOCALES = ['en', 'zh-CN'] as const

// 语言域：已登记的 en 与 zh-CN，加上未登记语言（应回退到 en 备选表）。
const LOCALES = ['en', 'zh-CN', 'en-US', 'fr', '__unknown_locale__'] as const

describe('Property 14: 错误码目录确定性与完备性', () => {
  // 对未登记 code/缺失键，目录会发出 console.warn —— 静默以保持输出干净。
  beforeEach(() => {
    vi.spyOn(console, 'warn').mockImplementation(() => {})
  })
  afterEach(() => {
    vi.restoreAllMocks()
  })

  // 确定性与非空：同一 (code, locale) 连续多次调用共享来源结果恒等且非空
  // （Requirement 9：永不返回空信息；跨应用逐字一致由 scripts/check-ui-sync.mjs 守护）。
  it('共享来源对同一 code+locale 的映射是确定性的（幂等）且非空', () => {
    fc.assert(
      fc.property(
        fc.constantFrom(...ALL_CODES),
        fc.constantFrom(...LOCALES),
        (code, locale) => {
          const first = getErrorMessage(code, locale)
          const second = getErrorMessage(code, locale)
          const third = getErrorMessage(code, locale)
          expect(second).toBe(first)
          expect(third).toBe(first)
          expect(typeof first).toBe('string')
          expect(first.length).toBeGreaterThan(0)
        },
      ),
      RUNS,
    )
  })

  // 不许静默兜底：上面的相等性断言分不出“有文案”和“缺码”——缺码时 getErrorMessage
  // 回落到 __unknown__，两次调用照样相等。后端会返回的每个码都必须在 user 目录里有
  // 真实文案（UF-08：三个用户可达的码曾以这种方式漏掉）。admin 侧的镜像不在这里
  // 断言：测试文件不得跨 app import（.claude/rules/frontend-contract.md），那道门由
  // scripts/check-ui-sync.mjs 守。
  it('每个后端码与协议码在 user 目录中都有真实文案，而不是回落到兜底', () => {
    for (const locale of REGISTERED_LOCALES) {
      const fallback = getErrorMessage(UNKNOWN_CODE, locale)
      for (const code of [...BACKEND_ERROR_CODES, ...FULLA_PROTOCOL_CODES]) {
        expect(getErrorMessage(code, locale), `frontends/user ${code} (${locale})`).not.toBe(fallback)
      }
    }
  })
})
