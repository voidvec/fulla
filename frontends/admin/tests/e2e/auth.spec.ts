import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin } from './helpers/mock-api'

test.describe('Authentication', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
  })

  test('redirects to login when not authenticated', async ({ page }) => {
    await page.goto('/admin/')
    await expect(page).toHaveURL(/\/admin\/login/)
  })

  test('shows login form with correct elements', async ({ page }) => {
    await page.goto('/admin/login')
    await expect(page.locator('h1')).toContainText('Fulla Admin')
    await expect(page.locator('input[type="text"]')).toBeVisible()
    await expect(page.locator('input[type="password"]')).toBeVisible()
    await expect(page.locator('button[type="submit"]')).toContainText('Sign in')
  })

  test('successful login navigates to dashboard', async ({ page }) => {
    await loginAsAdmin(page)
    await expect(page).toHaveURL(/\/admin\//)
    await expect(page.locator('h2:has-text("Dashboard")')).toBeVisible()
  })

  test('shows error on login failure', async ({ page }) => {
    // Override login mock to return error. Post-standardization the backend
    // returns the unified Error Envelope; the admin app maps error.code ->
    // localized message via the shared catalog (AUTH_INVALID_CREDENTIALS).
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 401,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: '用户名或密码错误', numeric_code: 4001, request_id: 'req-e2e-invalid-credentials' } }),
      })
    })

    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'wrong')
    await page.fill('input[type="password"]', 'wrong')
    await page.click('button[type="submit"]')

    await expect(page.locator('[role="alert"]')).toBeVisible()
    await expect(page.locator('[role="alert"]')).toContainText('Incorrect username or password')
  })

  test('denies access for non-admin users', async ({ page }) => {
    // Override userinfo to return non-admin user
    await page.route('**/oauth2/userinfo', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({
          sub: '123',
          username: 'regularuser',
          email: 'user@example.com',
          roles: ['user'],
        }),
      })
    })

    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'regularuser')
    await page.fill('input[type="password"]', 'password')
    await page.click('button[type="submit"]')

    // Should show error about admin role
    await expect(page.locator('[role="alert"]')).toContainText('Admin role required')
  })

  // Gap-fix E2: an mfa_required response now switches the form to the
  // 6-digit code view and completes the login via POST /oauth2/mfa/verify —
  // previously the flow dead-ended in a redirect loop back to /login.
  test('completes the MFA login flow end to end', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
      })
    })
    const mfaVerify = page.waitForRequest('**/oauth2/mfa/verify')
    await page.route('**/oauth2/mfa/verify', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        // Generated placeholder values (mock-api.ts rationale) — no token
        // literals in source.
        body: JSON.stringify({
          access_token: process.env.E2E_MOCK_AT ?? 'fixture',
          refresh_token: process.env.E2E_MOCK_RT ?? 'fixture',
          token_type: 'Bearer',
          expires_in: 3600,
        }),
      })
    })
    await page.route('**/oauth2/userinfo', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ sub: 'admin-1', name: 'admin', roles: ['admin'] }) })
    })

    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'admin')
    await page.fill('input[type="password"]', 'admin')
    await page.click('button[type="submit"]')

    // The MFA code view replaces the credential form.
    await expect(page.getByText('Two-factor authentication')).toBeVisible()
    await page.fill('#mfa-code-field', '123456')
    await page.click('button:has-text("Verify code")')

    // The verify call must carry the challenge state (mfa_token + verifier).
    const request = await mfaVerify
    const body = request.postData() || ''
    expect(body).toContain('mfa_token=mfa-token-123')
    expect(body).toContain('code=123456')
    expect(body).toContain('client_id=fulla-admin-console')
    expect(body).toContain('code_verifier=')

    await expect(page).toHaveURL(/\/admin\/?$|\/admin\/dashboard/)
  })

  test('stays on the MFA view with an error banner when the code is wrong', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
      })
    })
    await page.route('**/oauth2/mfa/verify', async (route) => {
      await route.fulfill({
        status: 401,
        contentType: 'application/json',
        // The backend rejects a wrong TOTP code with AUTH_INVALID_CREDENTIALS
        // (MfaController verifyLogin); the zh-CN catalog maps it to the
        // localized banner message.
        body: JSON.stringify({ error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: 'Invalid MFA code', numeric_code: 4001, request_id: 'req-e2e-mfa-bad' } }),
      })
    })

    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'admin')
    await page.fill('input[type="password"]', 'admin')
    await page.click('button[type="submit"]')
    await expect(page.getByText('Two-factor authentication')).toBeVisible()

    await page.fill('#mfa-code-field', '000000')
    await page.click('button:has-text("Verify code")')

    // Error banner shows and the user can retry on the same view.
    await expect(page.locator('[role="alert"]')).toContainText('Incorrect username or password')
    await expect(page.getByText('Two-factor authentication')).toBeVisible()
    await expect(page).toHaveURL(/\/admin\/login/)
  })

  test('a wrong code does not burn the PKCE verifier — retry succeeds', async ({ page }) => {
    // Regression: the verifier was consumed on entry, so a failed attempt
    // made every retry fail server-side PKCE (empty code_verifier against
    // the challenge bound at login) — the login loop returned via retry.
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
      })
    })
    const verifyRequests: string[] = []
    await page.route('**/oauth2/mfa/verify', async (route) => {
      verifyRequests.push(route.request().postData() || '')
      if (verifyRequests.length === 1) {
        await route.fulfill({
          status: 401,
          contentType: 'application/json',
          body: JSON.stringify({ error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: 'wrong code', numeric_code: 4001, request_id: 'req-e2e-mfa-try1' } }),
        })
      } else {
        // Generated placeholder values (mock-api.ts rationale): the security
        // scanner treats token literals as leaks; e2e needs obviously-fake
        // stable strings only.
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({
            access_token: process.env.E2E_MOCK_AT ?? 'fixture',
            refresh_token: process.env.E2E_MOCK_RT ?? 'fixture',
            token_type: 'Bearer',
            expires_in: 3600,
          }),
        })
      }
    })
    await page.route('**/oauth2/userinfo', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ sub: 'admin-1', name: 'admin', roles: ['admin'] }) })
    })

    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'admin')
    await page.fill('input[type="password"]', 'admin')
    await page.click('button[type="submit"]')
    await page.fill('#mfa-code-field', '000000')
    await page.click('button:has-text("Verify code")')
    await expect(page.locator('[role="alert"]')).toBeVisible()

    await page.fill('#mfa-code-field', '123456')
    await page.click('button:has-text("Verify code")')
    await expect(page).toHaveURL(/\/admin\/?$|\/admin\/dashboard/)

    // Both attempts must carry the SAME verifier — the first failure must
    // not have consumed it.
    expect(verifyRequests).toHaveLength(2)
    const verifierOf = (body: string) => new URLSearchParams(body).get('code_verifier')
    expect(verifierOf(verifyRequests[0])).toBeTruthy()
    expect(verifierOf(verifyRequests[1])).toBe(verifierOf(verifyRequests[0]))
  })

  test('empty username prevents form submission', async ({ page }) => {
    await page.goto('/admin/login')
    // Clear any default value and try to submit
    const usernameInput = page.locator('input[type="text"]')
    await expect(usernameInput).toHaveAttribute('required', '')
    // Browser HTML5 validation will prevent submission
    const isValid = await usernameInput.evaluate((el: HTMLInputElement) => el.validity.valueMissing)
    expect(isValid).toBe(true)
  })

  test('empty password prevents form submission', async ({ page }) => {
    await page.goto('/admin/login')
    const passwordInput = page.locator('input[type="password"]')
    await expect(passwordInput).toHaveAttribute('required', '')
    const isValid = await passwordInput.evaluate((el: HTMLInputElement) => el.validity.valueMissing)
    expect(isValid).toBe(true)
  })

  test('loading state during login', async ({ page }) => {
    // Slow the login response
    await page.route('**/oauth2/login', async (route) => {
      await new Promise(resolve => setTimeout(resolve, 500))
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ code: 'mock-auth-code' }),
      })
    })
    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'admin')
    await page.fill('input[type="password"]', 'admin')
    await page.click('button[type="submit"]')
    // Button should show loading text and be disabled
    const button = page.locator('button[type="submit"]')
    await expect(button).toContainText('Signing in')
    await expect(button).toBeDisabled()
    // Wait for completion
    await page.waitForURL('**/admin/')
  })

  test('non-existent user shows error', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 401,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: 'Invalid credentials' } }),
      })
    })
    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'nonexistent')
    await page.fill('input[type="password"]', 'password')
    await page.click('button[type="submit"]')
    await expect(page.locator('[role="alert"]')).toBeVisible()
    await expect(page).toHaveURL(/\/admin\/login/)
  })

  test('browser back after login stays on dashboard', async ({ page }) => {
    await loginAsAdmin(page)
    await expect(page).toHaveURL(/\/admin\//)
    await page.goBack()
    // Should still be on dashboard (auth guard redirects)
    await page.waitForTimeout(500)
    const url = page.url()
    expect(url).toMatch(/\/admin/)
  })
})
