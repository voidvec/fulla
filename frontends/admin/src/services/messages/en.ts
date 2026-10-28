/**
 * English (en) error message resources.
 *
 * Mirror of the canonical source
 *   frontends/user/src/services/messages/en.ts
 * kept in lockstep to honor the single-logical-source contract (Requirement
 * 8.6 / 10.7). The cross-app determinism property test (task 9.7) asserts
 * both catalogs return identical messages for every (code, locale).
 */
export const en: Record<string, string> = {
  // --- Reserved fallback keys (required by errorAdapter) ---
  __unknown__: 'An unexpected error occurred. Please try again later.',
  __network__: 'Network connection failed. Please check your network and try again.',

  // --- Backend Error_Code catalog ---
  NET_CONNECTION_FAILED: 'Upstream connection failed',
  NET_TIMEOUT: 'The request timed out',
  DB_CONNECTION_ERROR: 'The service is temporarily unavailable',
  DB_QUERY_ERROR: 'The service is temporarily unavailable',
  DB_CONSTRAINT_VIOLATION: 'Data conflict',
  VALIDATION_INVALID_INPUT: 'Invalid input',
  VALIDATION_MISSING_REQUIRED_FIELD: 'A required field is missing',
  VALIDATION_FORMAT_ERROR: 'Invalid format',
  VALIDATION_PASSWORD_TOO_SHORT: 'The password is too short',

  VALIDATION_RESOURCE_NOT_FOUND: 'Resource not found',
  VALIDATION_RESOURCE_CONFLICT: 'The resource already exists or conflicts',
  AUTH_INVALID_CREDENTIALS: 'Incorrect username or password',
  AUTH_TOKEN_EXPIRED: 'Your session has expired',
  AUTH_TOKEN_INVALID: 'Invalid credentials',
  AUTH_SESSION_REQUIRED: 'Please sign in first',
  AUTHZ_ACCESS_DENIED: 'Access denied',
  AUTHZ_INSUFFICIENT_PERMISSIONS: 'Insufficient permissions',
  INTERNAL_ERROR: 'Internal server error',

  // --- auth-flow-error-code-gaps codes ---
  VALIDATION_USERNAME_TAKEN: 'This username is already taken',
  VALIDATION_EMAIL_TAKEN: 'This email address is already registered',
  VALIDATION_CREDENTIAL_ALREADY_REGISTERED: 'This security key is already registered',
  VALIDATION_RESET_TOKEN_INVALID: 'This reset link has expired. Please request a new one',
  VALIDATION_VERIFICATION_TOKEN_INVALID: 'This verification link has expired. Please send a new email',
  VALIDATION_DEVICE_CODE_INVALID: 'The device code is invalid, expired, or already used',
  VALIDATION_RATE_LIMITED: 'Too many requests. Please try again later',
  VALIDATION_REDIRECT_URI_NOT_REGISTERED: 'The logout redirect URI is not registered',
  // --- P1-2 (#278): email-template admin codes (admin console only) ---
  EMAIL_TEMPLATE_NOT_FOUND: 'Email template not found',
  EMAIL_TEMPLATE_CONTENT_REJECTED: 'Template rejected: forbidden tag or outbound link outside email.allowed_link_domains',
  EMAIL_TEST_RECIPIENT_DENIED: 'Test recipient not whitelisted (your own address or email.test_recipients)',
  EMAIL_TEST_QUOTA_EXCEEDED: 'Test-send quota reached. Please try again later',
  AUTH_MFA_CODE_INVALID: 'Incorrect verification code',
  AUTH_MFA_NOT_CONFIGURED: 'Two-factor authentication is not configured yet. Set it up first',

  // --- OAuth2 / RFC 6749 protocol error codes ---
  invalid_request: 'Missing or invalid request parameters',
  invalid_client: 'Client authentication failed',
  invalid_grant: 'The authorization grant is invalid or expired',
  unauthorized_client: 'The client is not allowed to use this grant type',
  unsupported_grant_type: 'Unsupported grant type',
  invalid_scope: 'The requested scope is invalid',
  server_error: 'Internal server error',
  temporarily_unavailable: 'The service is temporarily unavailable',
  access_denied: 'The authorization request was denied',

  // --- RFC 7009 (token revocation) §2.2.1 ---
  unsupported_token_type: 'Unsupported token type',

  // --- RFC 8628 (device authorization grant) §3.5 polling error codes ---
  authorization_pending: 'Authorization is not complete yet. Please try again later',
  slow_down: 'Polling too frequently. Please slow down',
  expired_token: 'The device code has expired. Please restart the authorization',

  // --- RFC 6750 (Bearer Token Usage) §3.1 WWW-Authenticate error codes ---
  invalid_token: 'The access token is invalid or expired',
  insufficient_scope: 'Insufficient permissions: a required scope is missing',

  // --- OIDC Core 1.0 §3.1.2.6 authorization-error codes ---
  login_required: 'Sign-in is required to complete authorization',
  consent_required: 'Your consent is required to complete authorization',
  interaction_required: 'User interaction is required to complete authorization',
}
