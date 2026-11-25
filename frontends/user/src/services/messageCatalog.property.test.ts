// Feature: error-code-message-standardization, Property 13: 前端信息目录覆盖与清洁性
//
// Property 13 — 前端信息目录覆盖与清洁性
// Validates: Requirements 9.1, 9.4
//
// 对任意后端 Error_Catalog 中登记的 Error_Code 以及任一 OAuth2 协议错误码，
// Error_Message_Catalog_FE 都提供一条非空、不含未替换占位符标记的本地化条目；
// 且 Error_Message_Catalog_FE 中所有展示给用户的信息（含通用回退信息）均不包含
// Request_ID 之外的后端 Internal_Detail（如 SQL、文件路径、堆栈片段）。
import { describe, expect, it } from 'vitest'
import fc from 'fast-check'
import {
  CLIENT_CODE,
  DEFAULT_LOCALE,
  getErrorMessage,
  messages,
  NETWORK_CODE,
  UNKNOWN_CODE,
} from './messages'

const RUNS = { numRuns: 100 } as const

// --- 期望覆盖的码集合（与后端 ErrorCatalog 单一权威来源镜像，design.md AD-5） ---

// 后端 Error_Catalog 全部 Error_Code（初始 14 条 + 方案 A 新增的
// VALIDATION_RESOURCE_NOT_FOUND/CONFLICT 共 16 条，整数取值保留不变）。
const BACKEND_ERROR_CODES = [
  'NET_CONNECTION_FAILED',
  'NET_TIMEOUT',
  'DB_CONNECTION_ERROR',
  'DB_QUERY_ERROR',
  'DB_CONSTRAINT_VIOLATION',
  'VALIDATION_INVALID_INPUT',
  'VALIDATION_MISSING_REQUIRED_FIELD',
  'VALIDATION_FORMAT_ERROR',
  'VALIDATION_RESOURCE_NOT_FOUND',
  'VALIDATION_RESOURCE_CONFLICT',
  'AUTH_INVALID_CREDENTIALS',
  'AUTH_TOKEN_EXPIRED',
  'AUTH_TOKEN_INVALID',
  'AUTHZ_ACCESS_DENIED',
  'AUTHZ_INSUFFICIENT_PERMISSIONS',
  'INTERNAL_ERROR',
] as const

// 后端 ErrorCatalog 登记的全部 OAuth2 协议字符串错误码：
//   - RFC 6749 §5.2 基础集合（8 个）
//   - RFC 6749 §4.1.2.1 授权端点: access_denied
//   - RFC 7009（令牌撤销）: unsupported_token_type
//   - RFC 8628（设备授权）: authorization_pending / slow_down / expired_token
// 这些与后端 OAuth2Plugin/src/error/ErrorCatalog.cc 的 rawOAuthEntries() 一一对应。
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
] as const

// Property 13 要求覆盖的码集合：后端 Error_Code ∪ OAuth2 协议码。
const REQUIRED_CODES = [...BACKEND_ERROR_CODES, ...FULLA_PROTOCOL_CODES] as const

// 保留回退键：其值同样需为非空且清洁（占位符/Internal_Detail 检查），但它们作为
// KEY 命名（__unknown__/__network__/__client__）本身不算占位符标记。
const RESERVED_CODES = [UNKNOWN_CODE, NETWORK_CODE, CLIENT_CODE] as const

// --- 占位符标记（未替换的模板片段视为目录缺陷） ---
const PLACEHOLDER_PATTERNS: readonly RegExp[] = [
  /\{\{/, // mustache 开标记
  /\}\}/, // mustache 闭标记
  /\$\{/, // 模板字符串插值
  /%[sd]/, // printf 风格占位符
  /__MISSING__/i,
]

// --- Internal_Detail 模式（SQL / 文件路径 / 堆栈跟踪）。 ---
// 仅匹配带上下文的英文技术片段，避免在合法中文文案上误报。
const INTERNAL_DETAIL_PATTERNS: readonly RegExp[] = [
  // SQL 片段（需具备 SQL 结构上下文）
  /\bSELECT\b[\s\S]*\bFROM\b/i,
  /\bINSERT\s+INTO\b/i,
  /\bUPDATE\b[\s\S]*\bSET\b/i,
  /\bDELETE\s+FROM\b/i,
  /\bFROM\s+[A-Za-z_]\w*/i,
  /\bWHERE\s+[A-Za-z_]\w*/i,
  // 文件系统路径
  /[A-Za-z]:\\/, // Windows 绝对路径 C:\
  /\/(usr|etc|var|home|opt|root|tmp)\//, // Unix 绝对路径
  /\.(cc|cpp|cxx|hpp|h|ts|tsx|js|jsx|py):\d+/i, // 带行号的源文件栈帧
  // 堆栈跟踪标记
  /\bat\s+[\w$.]+\s*\(/, // 栈帧: at func (
  /\bException\b/,
  /\bTraceback\b/i,
  /\bstack\s*trace\b/i,
]

function findPlaceholder(value: string): RegExp | undefined {
  return PLACEHOLDER_PATTERNS.find((re) => re.test(value))
}

function findInternalDetail(value: string): RegExp | undefined {
  return INTERNAL_DETAIL_PATTERNS.find((re) => re.test(value))
}

describe('Property 13: 前端信息目录覆盖与清洁性', () => {
  // 9.1 — 覆盖性：后端每个 Error_Code 与每个 OAuth2 协议码在目录中都各有一条
  // 非空、无占位符、无 Internal_Detail 的本地化条目。
  //
  // 注意：getErrorMessage() 对缺失键会回退到 __unknown__，从而掩盖「缺键」缺陷；
  // 因此覆盖性必须直接检查目录表是否拥有该键，而非仅看返回值是否非空。
  it('为后端每个 Error_Code 与每个协议码提供非空、清洁的本地化条目（穷举，全部已登记语言）', () => {
    // Property 13 覆盖范围随登记语言扩展（ADR-0013）：en 与 zh-CN 都必须
    // 完整覆盖 REQUIRED_CODES，缺一即违反 Requirement 9.1。
    const registered = Object.keys(messages)
    expect(registered).toContain(DEFAULT_LOCALE)

    for (const locale of registered) {
      const table = messages[locale]
      expect(table, `缺失语言目录: ${locale}`).toBeTruthy()

      const missing: string[] = []
      for (const code of REQUIRED_CODES) {
        const hasOwn = Object.prototype.hasOwnProperty.call(table, code)
        const value = table[code]
        if (!hasOwn || typeof value !== 'string' || value.length === 0) {
          missing.push(code)
          continue
        }

        // 非空、无未替换占位符标记。
        const placeholder = findPlaceholder(value)
        expect(
          placeholder,
          `[${locale}] 码 ${code} 的条目含未替换占位符标记 (${placeholder}): "${value}"`,
        ).toBeUndefined()

        // 无 SQL/路径/堆栈等 Internal_Detail。
        const internal = findInternalDetail(value)
        expect(
          internal,
          `[${locale}] 码 ${code} 的条目含 Internal_Detail (${internal}): "${value}"`,
        ).toBeUndefined()

        // getErrorMessage 的功能路径返回的就是该条目本身（而非回退到 __unknown__）。
        expect(getErrorMessage(code, locale)).toBe(value)
      }

      expect(
        missing,
        `[${locale}] Error_Message_Catalog_FE 缺失以下码的条目（违反 Requirement 9.1）: ${missing.join(', ')}`,
      ).toEqual([])
    }
  })

  // 9.4 — 清洁性：目录中所有展示给用户的信息（含保留回退键 __unknown__/__network__
  // 的值）均不含 Request_ID 之外的 Internal_Detail，也不含未替换占位符。
  it('目录内全部条目（含保留回退键的值）均不含占位符与 Internal_Detail', () => {
    for (const [locale, table] of Object.entries(messages)) {
      for (const [code, value] of Object.entries(table)) {
        expect(
          typeof value === 'string' && value.length > 0,
          `[${locale}] 码 ${code} 的条目应为非空字符串`,
        ).toBe(true)

        const placeholder = findPlaceholder(value)
        expect(
          placeholder,
          `[${locale}] 码 ${code} 含未替换占位符 (${placeholder}): "${value}"`,
        ).toBeUndefined()

        const internal = findInternalDetail(value)
        expect(
          internal,
          `[${locale}] 码 ${code} 含 Internal_Detail (${internal}): "${value}"`,
        ).toBeUndefined()
      }
    }

    // 保留回退键的值显式校验为非空且清洁。
    for (const reserved of RESERVED_CODES) {
      const value = messages[DEFAULT_LOCALE][reserved]
      expect(typeof value, `保留键 ${reserved} 缺失`).toBe('string')
      expect(value.length).toBeGreaterThan(0)
      expect(findPlaceholder(value)).toBeUndefined()
      expect(findInternalDetail(value)).toBeUndefined()
    }
  })

  // 9.1 / 9.4 — 属性化采样：对任一后端码/协议码，在任一已登记语言下都映射到
  // 非空、清洁、无占位符的本地化信息（numRuns: 100）。直接检查目录键存在以
  // 避免 __unknown__ 回退掩盖缺键。
  it('对任一码与任一已登记语言均返回非空、无占位符、无 Internal_Detail 的本地化信息', () => {
    const registered = Object.keys(messages)
    fc.assert(
      fc.property(
        fc.constantFrom(...REQUIRED_CODES),
        fc.constantFrom(...registered),
        (code, locale) => {
          const localeTable = messages[locale]
          // 覆盖性：目录拥有该键。
          expect(Object.prototype.hasOwnProperty.call(localeTable, code)).toBe(true)

          const message = getErrorMessage(code, locale)
          // 非空本地化信息。
          expect(typeof message).toBe('string')
          expect(message.length).toBeGreaterThan(0)
          // 返回的是该码自身的条目，而非通用未知回退。
          expect(message).toBe(localeTable[code])
          // 清洁性。
          expect(findPlaceholder(message)).toBeUndefined()
          expect(findInternalDetail(message)).toBeUndefined()
        },
      ),
      RUNS,
    )
  })
})
