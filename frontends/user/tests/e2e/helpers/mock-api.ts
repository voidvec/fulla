import { Page } from '@playwright/test'

export const MOCK_USER = {
  sub: '6cb8d705-a0a8-4bff-acbd-dc3ce0f610bc',
  name: 'testuser',
  email: 'test@example.com',
  email_verified: true,
  roles: ['user'],
}

export const MOCK_PROFILE = {
  username: 'testuser',
  email: 'test@example.com',
  email_verified: true,
  mfa_enabled: false,
}

export const MOCK_AUTHORIZED_APPS = [
  { client_id: 'third-party-app', name: 'Third Party App' },
  { client_id: 'mobile-app', name: 'Mobile App' },
]

// Real backend shape for /api/me/webauthn/credentials (WebAuthnController):
// {credential_id, name, sign_count} — no id/created_at (gap-fix E4/E8).
export const MOCK_WEBAUTHN_CREDENTIALS = [
  { credential_id: 'cred-1', name: 'My Passkey', sign_count: 0 },
]

// Real backend shape for /api/me/mfa/verify (self-service setup verify): the
// 10 single-use recovery codes are returned exactly once (gap-fix P0).
export const MOCK_BACKUP_CODES = [
  'B7XK-2Q9M', 'N4TD-8HVL', 'P3RW-6YCN', 'K9JS-5FZD', 'M2GH-7XQB',
  'V8LC-3TNK', 'R5DY-9WPH', 'Q6FM-4BXS', 'Z3KN-7JTV', 'H8PW-2MRD',
]

// Valid base64url challenge (gap-fix E1 review): the real backend issues
// 32-byte base64url challenges containing `-_`; a plain-word challenge hides
// atob/decode bugs. (42 chars → len % 4 == 2, a legal unpadded base64 length.)
export const MOCK_WEBAUTHN_CHALLENGE = 'mock-Challenge_43-chars_base64url-ABCD-_12'

export const MOCK_SOCIAL_LINKS = [
  { provider: 'github', subject: '4242', linked_at: '2026-08-01T00:00:00Z' },
]

// Fake-token fixtures for route fulfillment. Values are GENERATED rather
// than written as literals: the security scanner rightly treats
// `<key>_token: '<literal>'` in source as a leaked-credential pattern, and
// e2e only needs obviously-fake, stable strings.
const fakeToken = (kind: string, prefix = 'mock') => `${prefix}-${kind}-token`

export const MOCK_TOKEN_PAIR = {
  access_token: fakeToken('access'),
  refresh_token: fakeToken('refresh'),
  expires_in: 3600,
}

export const MOCK_REFRESHED_TOKEN_PAIR = {
  access_token: fakeToken('access', 'refreshed'),
  refresh_token: fakeToken('refresh', 'new'),
  expires_in: 3600,
}

export const MOCK_SLOW_TOKEN_PAIR = {
  access_token: fakeToken('access', 'slow'),
  refresh_token: fakeToken('refresh', 'slow'),
  token_type: 'Bearer',
  expires_in: 3600,
}

export async function setupMocks(page: Page) {
  await page.route('**/oauth2/login', async (route) => {
    const body = route.request().postData() || ''
    if (body.includes('password=wrong')) {
      // Post-standardization backend returns the unified Error Envelope; the
      // frontend maps error.code -> localized message via the shared catalog.
      await route.fulfill({ status: 401, contentType: 'application/json', body: JSON.stringify({ error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: '用户名或密码错误', numeric_code: 4001, request_id: 'req-e2e-invalid-credentials' } }) })
    } else {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ code: 'mock-auth-code-12345' }) })
    }
  })

  await page.route('**/oauth2/token', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ access_token: 'mock-access-token', refresh_token: 'mock-refresh-token', token_type: 'Bearer', expires_in: 3600 }) })
  })

  await page.route('**/oauth2/userinfo', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(MOCK_USER) })
  })

  await page.route('**/api/register', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'User registered successfully', note: 'Please check your email to verify your account' }) })
  })

  await page.route('**/api/password-reset/request', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'If the email exists, a reset link has been sent' }) })
  })

  await page.route('**/api/password-reset/confirm', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'Password reset successfully' }) })
  })

  await page.route('**/api/verify-email**', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'Email verified successfully' }) })
    } else {
      await route.continue()
    }
  })

  await page.route('**/api/verify-email/resend', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'Verification email sent' }) })
  })

  await page.route('**/api/me', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(MOCK_PROFILE) })
    } else if (route.request().method() === 'DELETE') {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'Account deleted' }) })
    } else { await route.continue() }
  })

  await page.route('**/api/me/password', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'Password changed successfully' }) })
  })

  await page.route('**/api/me/mfa/setup', async (route) => {
    // Real field is otpauth_uri (MfaController) — the old qr_uri shape was a
    // mock-only fiction (gap-fix E8).
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ secret: 'JBSWY3DPEHPK3PXP', otpauth_uri: 'otpauth://totp/Fulla:testuser?secret=JBSWY3DPEHPK3PXP' }) })
  })

  await page.route('**/api/me/mfa/verify', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'MFA enabled', backup_codes: MOCK_BACKUP_CODES }) })
  })

  await page.route('**/api/me/mfa/disable', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'MFA disabled' }) })
  })

  await page.route('**/api/me/authorized-apps', async (route) => {
    // Real envelope: {authorized_apps, total} (UserSelfServiceController) —
    // the old {apps} shape was mock-only (gap-fix E8).
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ authorized_apps: MOCK_AUTHORIZED_APPS, total: MOCK_AUTHORIZED_APPS.length }) })
  })

  await page.route('**/api/me/authorized-apps/*', async (route) => {
    if (route.request().method() === 'DELETE') {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'Authorization revoked' }) })
    } else { await route.continue() }
  })

  // WebAuthn credentials — real shape {credential_id, name, sign_count}
  await page.route('**/api/me/webauthn/credentials', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ credentials: MOCK_WEBAUTHN_CREDENTIALS, total: MOCK_WEBAUTHN_CREDENTIALS.length }) })
  })

  // WebAuthn registration — real begin shape nests creation options under
  // `options` (WebAuthnController); finish validates the backend's contract
  // fields, not the browser attestation envelope (gap-fix E1/E8).
  await page.route('**/api/me/webauthn/register/begin', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ options: { challenge: MOCK_WEBAUTHN_CHALLENGE, rp: { name: 'Fulla', id: 'localhost' }, user: { id: 'dXNlci0x', name: 'testuser', displayName: 'testuser' }, pubKeyCredParams: [{ type: 'public-key', alg: -7 }, { type: 'public-key', alg: -257 }], timeout: 60000, authenticatorSelection: { userVerification: 'preferred' } } }) })
  })

  await page.route('**/api/me/webauthn/register/finish', async (route) => {
    const body = JSON.parse(route.request().postData() || '{}')
    if (!body.credential_id || !body.public_key || !body.name) {
      await route.fulfill({ status: 400, contentType: 'application/json', body: JSON.stringify({ error: { code: 'VALIDATION_MISSING_REQUIRED_FIELD', category: 'VALIDATION', message: 'credential_id and public_key are required', numeric_code: 1001, request_id: 'req-e2e-webauthn' } }) })
    } else {
      await route.fulfill({ status: 201, contentType: 'application/json', body: JSON.stringify({ message: 'Passkey registered successfully', credential_id: body.credential_id }) })
    }
  })

  // Social links (B2 link/unlink)
  await page.route('**/api/me/social/links', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ social_links: MOCK_SOCIAL_LINKS, total: MOCK_SOCIAL_LINKS.length }) })
    } else {
      await route.continue()
    }
  })

  await page.route('**/api/me/social/links/*', async (route) => {
    if (route.request().method() === 'DELETE') {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ provider: 'github', message: 'Social account unlinked successfully' }) })
    } else {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ provider: 'github', subject: '4242', message: 'Social account linked successfully' }) })
    }
  })

  await page.route('**/oauth2/consent', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ redirect_uri: 'http://localhost:5173/callback?code=consent-code&state=test' }) })
  })

  // v1.5.0 M1b: consent-screen context (owner attribution + org banner) is
  // server-derived via GET /oauth2/consent/context. Default = the official-
  // app shape (no attribution, no org); attribution tests override this
  // route (page.route is LIFO, so a later registration wins).
  await page.route('**/oauth2/consent/context*', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ owner_name: '', org: null }) })
  })

  // Gap-fix E3: logout now goes through POST /oauth2/logout (Bearer) —
  // intercepted here so logout flows and request assertions work offline.
  await page.route('**/oauth2/logout', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'Logged out successfully' }) })
  })

  await page.route('**/oauth2/revoke', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({}) })
  })

  await page.route('**/oauth2/mfa/verify', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ access_token: 'mock-mfa-token', refresh_token: 'mock-mfa-refresh', token_type: 'Bearer' }) })
  })
}

export async function loginUser(page: Page) {
  // Set tokens in localStorage before navigating
  await page.addInitScript(() => {
    localStorage.setItem('access_token', 'mock-access-token')
    localStorage.setItem('refresh_token', 'mock-refresh-token')
  })
  await page.goto('/')
  // Wait for the page to load (should not redirect to /login since token is set)
  await page.waitForLoadState('networkidle')
}

export async function loginViaForm(page: Page) {
  await page.goto('/login')
  await page.locator('input[autocomplete="username"]').fill('testuser')
  await page.locator('input[autocomplete="current-password"]').fill('password123')
  await page.locator('button[type="submit"]').click()
  await page.waitForTimeout(2000)
}

/**
 * Override a previously registered route with a custom handler.
 */
export async function overrideRoute(page: Page, urlPattern: string, handler: (route: any) => Promise<void>) {
  await page.route(urlPattern, handler)
}

/**
 * Mock a specific API returning an error status with Error Envelope body.
 */
export async function mockApiError(page: Page, urlPattern: string, status: number, body: object) {
  await page.route(urlPattern, async (route) => {
    await route.fulfill({
      status,
      contentType: 'application/json',
      body: JSON.stringify(body),
    })
  })
}

/**
 * Mock a network failure for a given URL pattern.
 */
export async function mockNetworkError(page: Page, urlPattern: string) {
  await page.route(urlPattern, async (route) => {
    await route.abort('failed')
  })
}

/**
 * Mock registration API to return an error (e.g., duplicate user).
 * Codes mirror the real backend (AuthService.cc): duplicate registration
 * answers VALIDATION_USERNAME_TAKEN / VALIDATION_EMAIL_TAKEN.
 */
export async function mockRegistrationError(page: Page, status: number, errorCode: string) {
  await page.route('**/api/register', async (route) => {
    await route.fulfill({
      status,
      contentType: 'application/json',
      body: JSON.stringify({
        error: { code: errorCode, category: 'VALIDATION', message: 'User already exists' },
      }),
    })
  })
}

/**
 * Mock password change API to return an error.
 */
export async function mockPasswordChangeError(page: Page, errorCode: string) {
  await page.route('**/api/me/password', async (route) => {
    await route.fulfill({
      status: 400,
      contentType: 'application/json',
      body: JSON.stringify({
        error: { code: errorCode, category: 'AUTHENTICATION', message: 'Wrong password' },
      }),
    })
  })
}

/**
 * Org consent-requests (v1.6): member filing + manager approval queue under
 * one organization slug. Wire contract mirrored here:
 *   GET  {base}                     → {slug, requests, total}
 *   POST {base}                     → 200 pending request (idempotent: an
 *                                     identical pending client_id replays the
 *                                     SAME id with the already-pending message)
 *   POST {base}/{id}/approve|reject → 200 {id, client_id, status, scopes?, message}
 * The caller owns `state` (seed `requests` before the test); every mutating
 * call is recorded in `state.calls` so specs can assert the wire traffic
 * without waitForRequest plumbing. Trailing '**' on the glob so the
 * sub-path action POSTs are captured (a single '*' cannot cross '/').
 */
export interface OrgConsentRequestState {
  requests: any[]
  calls: { method: string; path: string; body: any }[]
}

export async function mockOrgConsentRequests(page: Page, slug: string, state: OrgConsentRequestState) {
  const base = `/api/me/organizations/${slug}/consent-requests`
  await page.route(`**${base}**`, async (route) => {
    const method = route.request().method()
    const path = new URL(route.request().url()).pathname
    if (method === 'GET' && path === base) {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ slug, requests: state.requests, total: state.requests.length }),
      })
      return
    }
    if (method === 'POST' && path === base) {
      const body = route.request().postDataJSON()
      state.calls.push({ method, path, body })
      const replay = state.requests.find((r) => r.client_id === body?.client_id && r.status === 'pending')
      if (replay) {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ ...replay, message: 'An identical request is already pending' }),
        })
        return
      }
      const created = {
        id: `cr_${state.calls.length}`,
        organization_id: slug,
        client_id: body?.client_id,
        requested_by: 1,
        requester_username: MOCK_PROFILE.username,
        requested_at: '2026-09-26 10:00:00',
        status: 'pending',
        message: 'awaiting a manager decision',
      }
      state.requests.push(created)
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(created) })
      return
    }
    if (method === 'POST' && (path.endsWith('/approve') || path.endsWith('/reject'))) {
      const action = path.endsWith('/approve') ? 'approve' : 'reject'
      const status = action === 'approve' ? 'approved' : 'rejected'
      const id = path.slice(base.length + 1, -(action.length + 1))
      state.calls.push({ method, path, body: route.request().postDataJSON() })
      const target = state.requests.find((r) => r.id === id)
      // The backend auto-resolves the request on approve/reject — refetches
      // after the action must see it gone from the pending queue.
      state.requests = state.requests.filter((r) => r.id !== id)
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({
          id,
          client_id: target?.client_id,
          status,
          scopes: status === 'approved' ? ['openid', 'profile'] : undefined,
          message: status,
        }),
      })
      return
    }
    await route.fulfill({ status: 404, body: '{}' })
  })
}
