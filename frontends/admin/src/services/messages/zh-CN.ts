/**
 * Simplified-Chinese (zh-CN) error message resources.
 *
 * Mirror of the canonical source
 *   frontends/user/src/services/messages/zh-CN.ts
 * kept in lockstep to honor the single-logical-source contract (Requirement
 * 8.6 / 10.7). The cross-app determinism property test (task 9.7) asserts both
 * catalogs return identical messages for every (code, locale).
 *
 * Invariants (Requirements 9.1, 9.4):
 *   - reserved keys `__unknown__` and `__network__` are present and non-empty
 *   - every entry is a non-empty string with no unreplaced placeholders
 *   - no Internal_Detail (SQL/paths/stack traces) beyond Request_ID
 */
export const zhCN: Record<string, string> = {
  // --- Reserved fallback keys (required by errorAdapter) ---
  __unknown__: '发生未知错误，请稍后重试',
  __network__: '网络连接失败，请检查网络后重试',

  // --- Backend Error_Code catalog (design.md initial entries) ---
  NET_CONNECTION_FAILED: '上游连接失败',
  NET_TIMEOUT: '请求超时',
  DB_CONNECTION_ERROR: '服务暂时不可用',
  DB_QUERY_ERROR: '服务暂时不可用',
  DB_CONSTRAINT_VIOLATION: '数据冲突',
  VALIDATION_INVALID_INPUT: '输入参数有误',
  VALIDATION_MISSING_REQUIRED_FIELD: '缺少必填字段',
  VALIDATION_FORMAT_ERROR: '格式不正确',
  VALIDATION_PASSWORD_TOO_SHORT: '密码长度不足',

  VALIDATION_RESOURCE_NOT_FOUND: '资源不存在',
  VALIDATION_RESOURCE_CONFLICT: '资源已存在或冲突',
  AUTH_INVALID_CREDENTIALS: '用户名或密码错误',
  AUTH_TOKEN_EXPIRED: '登录已过期',
  AUTH_TOKEN_INVALID: '登录凭证无效',
  AUTH_SESSION_REQUIRED: '需要先登录',
  AUTHZ_ACCESS_DENIED: '没有访问权限',
  AUTHZ_INSUFFICIENT_PERMISSIONS: '权限不足',
  INTERNAL_ERROR: '服务器内部错误',

  // --- auth-flow-error-code-gaps: 9 new backend Error_Codes ---
  // Mirror of frontends/user/src/services/messages/zh-CN.ts (Requirement 9.3).
  VALIDATION_USERNAME_TAKEN: '该用户名已被注册',
  VALIDATION_EMAIL_TAKEN: '该邮箱已被注册',
  VALIDATION_CREDENTIAL_ALREADY_REGISTERED: '该安全密钥已注册，无需重复添加',
  VALIDATION_RESET_TOKEN_INVALID: '重置链接已失效，请重新申请',
  VALIDATION_VERIFICATION_TOKEN_INVALID: '验证链接已失效，请重新发送邮件',
  VALIDATION_DEVICE_CODE_INVALID: '设备码无效、已过期或已被处理',
  VALIDATION_RATE_LIMITED: '请求过于频繁，请稍后重试',
  VALIDATION_REDIRECT_URI_NOT_REGISTERED: '登出重定向地址未注册',
  // --- P1-2 (#278): email-template admin codes (admin console only) ---
  EMAIL_TEMPLATE_NOT_FOUND: '邮件模板不存在',
  EMAIL_TEMPLATE_CONTENT_REJECTED: '模板含禁用标签或外链域名不在允许列表',
  EMAIL_TEST_RECIPIENT_DENIED: '测试收件人不在白名单（须为本人邮箱或 email.test_recipients 配置项）',
  EMAIL_TEST_QUOTA_EXCEEDED: '测试发送配额已用尽，请稍后重试',
  // --- P1-6 (#283): audit export guard codes ---
  AUDIT_EXPORT_WINDOW_REQUIRED: '导出审计日志必须限定时间范围（from/to）或显式选择全量导出',
  AUDIT_EXPORT_TOO_LARGE: '导出行数超过上限，请收窄时间范围后重试',
  // --- P1-3 (#280): claims mapping + custom attributes ---
  CLAIM_MAPPING_RESERVED_CLAIM: '映射的 claim 名为保留字，不可覆盖标准声明',
  CLAIM_MAPPING_INVALID_SOURCE: '映射源路径不在白名单（仅 custom_attributes.<key> 或 user.display_name）',
  CLAIM_MAPPING_FORBIDDEN_SCOPE: '触发 scope 不允许（openid 为必带 scope，禁止挂映射）',
  CUSTOM_ATTRIBUTES_INVALID: '自定义属性不合法（对象、深度≤2、≤8KB、键名小写字母开头）',
  AUTH_MFA_CODE_INVALID: '验证码不正确',
  AUTH_MFA_NOT_CONFIGURED: '尚未设置双重验证，请先完成设置',

  // --- OAuth2 / RFC 6749 protocol error codes ---
  invalid_request: '请求参数缺失或无效',
  invalid_client: '客户端认证失败',
  invalid_grant: '授权许可无效或已过期',
  unauthorized_client: '客户端无权使用该授权类型',
  unsupported_grant_type: '不支持的授权类型',
  invalid_scope: '请求的 scope 无效',
  server_error: '服务器内部错误',
  temporarily_unavailable: '服务暂时不可用',
  access_denied: '授权请求被拒绝',

  // --- RFC 7009 (token revocation) §2.2.1 ---
  unsupported_token_type: '不支持的令牌类型',

  // --- RFC 8628 (device authorization grant) §3.5 polling error codes ---
  authorization_pending: '授权尚未完成，请稍后重试',
  slow_down: '轮询过于频繁，请降低频率',
  expired_token: '设备码已过期，请重新发起授权',

  // --- RFC 6750 (Bearer Token Usage) §3.1 WWW-Authenticate error codes ---
  invalid_token: '提供的访问令牌无效或已过期',
  insufficient_scope: '权限不足，缺少所需的 scope',

  // --- OIDC Core 1.0 §3.1.2.6 authorization-error codes ---
  login_required: '需要重新登录以完成授权',
  consent_required: '需要用户同意才能完成授权',
  interaction_required: '需要用户交互才能完成授权',
}
