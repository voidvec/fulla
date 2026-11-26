// Feature: error-code-message-standardization, Property 14: 跨应用映射确定性一致
//
// Property 14 — 跨应用映射确定性一致
// Validates: Requirements 10.7
//
// 对任意 Error_Code，在界面语言相同的条件下，frontends/user 与 frontends/admin
// 经共享 `errorAdapter` 与共享目录得到的本地化用户信息相同。
//
// 设计依据（design.md §AD-5 / §AD-6 / Requirement 8.6）：`errorAdapter` 与
// `Error_Message_Catalog_FE` 是「单一权威来源（single source of truth）」。
// frontends/admin 复用同一实现来源（在任务 10.2 中接线为 re-export/shim）。因此
// 「跨应用一致」在本质上等价于「该共享来源对同一 (code, locale) 的映射是
// 确定性的」：两应用各自的入口最终都调用同一个 `getErrorMessage`。
//
// 本测试通过模拟两个应用入口来验证 Property 14：
//   - frontendPath：frontends/user 直接使用的共享 `getErrorMessage`。
//   - adminPath：若 frontends/admin 已存在同源 re-export/shim（任务 10.2 产物），
//     则动态导入该 shim 并断言其导出与共享来源指向同一实现；否则回退为对共享
//     来源的第二次独立求值（确定性），并据“Admin 导入同一模块”这一不变量
//     得出两应用一致的结论。
//
// 现状说明（2026-11 复核 UF-08 时做了变异验证）：adminPath 的候选路径在 vitest 下
// 都加载不到——测试文件不得跨 app import（.claude/rules/frontend-contract.md），
// user 的模块上下文只有 frontends/user/。把 admin 目录里的文案改坏，本文件照样全绿。
// 所以本文件实际验证的只是共享来源自身的确定性；两应用目录逐字一致由
// scripts/check-ui-sync.mjs 的“错误目录镜像”一节守护，那一步同样的变异会失败。
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

describe('Property 14: 跨应用映射确定性一致', () => {
  // 对未登记 code/缺失键，目录会发出 console.warn —— 静默以保持输出干净。
  beforeEach(() => {
    vi.spyOn(console, 'warn').mockImplementation(() => {})
  })
  afterEach(() => {
    vi.restoreAllMocks()
  })

  // 核心属性：两应用入口（共享来源 + Admin 路径）对同一 (code, locale)
  // 返回相同 message，且该映射是确定性的（多次调用一致）。
  it('两应用经同源 errorAdapter/目录对同一 code+locale 得到相同 message', async () => {
    // 尝试加载 frontends/admin 的同源 re-export/shim（任务 10.2 产物）。若尚未
    // 创建则为 undefined，回退到“共享来源确定性”路径。
    const adminGetErrorMessage = await loadAdminGetErrorMessage()

    fc.assert(
      fc.property(
        fc.constantFrom(...ALL_CODES),
        fc.constantFrom(...LOCALES),
        (code, locale) => {
          // frontends/user 入口：直接使用共享 getErrorMessage。
          const frontendMessage = getErrorMessage(code, locale)

          // frontends/admin 入口：使用 Admin 同源 shim（若存在），否则对共享来源
          // 再次独立求值（同源 ⇒ 同实现 ⇒ 同结果）。
          const adminMessage = (adminGetErrorMessage ?? getErrorMessage)(
            code,
            locale,
          )

          expect(adminMessage).toBe(frontendMessage)
          // 同时断言信息为非空字符串（Requirement 9：永不返回空信息）。
          expect(typeof frontendMessage).toBe('string')
          expect(frontendMessage.length).toBeGreaterThan(0)
        },
      ),
      RUNS,
    )
  })

  // 确定性：同一 (code, locale) 连续多次调用共享来源结果恒等 —— 这是“跨应用
  // 一致”的充要前提（两应用都调用同一纯函数）。
  it('共享来源对同一 code+locale 的映射是确定性的（幂等）', () => {
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

/**
 * 动态加载管理后台（frontends/admin）的同源 `getErrorMessage`（任务 10.2
 * 接线后的 shim）。
 *
 * 约定的候选位置（任意一处存在即视为 Admin 复用了同一来源）：
 *   - frontends/admin/src/services/messages
 *   - frontends/admin/src/services/errorAdapter（若 re-export getErrorMessage）
 *
 * 若均不存在（任务 10.2 尚未完成），返回 undefined，由调用方回退到共享来源的
 * 确定性求值，并据“Admin 导入同一模块”的设计不变量得出一致性结论。
 */
async function loadAdminGetErrorMessage(): Promise<
  ((code: string, locale?: string) => string) | undefined
> {
  const candidates = [
    '../../../admin/src/services/messages',
    '../../../admin/src/services/messages/index',
    '../../../admin/src/services/errorAdapter',
  ]
  for (const path of candidates) {
    try {
      // @vite-ignore — 路径在 shim 创建前不存在属预期情况。
      const mod: Record<string, unknown> = await import(/* @vite-ignore */ path)
      const fn = mod['getErrorMessage']
      if (typeof fn === 'function') {
        return fn as (code: string, locale?: string) => string
      }
    } catch {
      // 模块尚不存在：继续尝试下一个候选。
    }
  }
  return undefined
}
