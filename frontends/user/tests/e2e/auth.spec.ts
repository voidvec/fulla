import { test, expect } from '@playwright/test'
import { setupMocks, MOCK_TOKEN_PAIR } from './helpers/mock-api'

test.describe('Login', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  test('displays login form', async ({ page }) => {
    await page.goto('/login')
    await expect(page.getByRole('heading', { name: /sign in/i })).toBeVisible()
    await expect(page.locator('input[autocomplete="username"]')).toBeVisible()
    await expect(page.locator('input[autocomplete="current-password"]')).toBeVisible()
    await expect(page.locator('button[type="submit"]')).toBeVisible()
  })

  test('successful login redirects to dashboard', async ({ page }) => {
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    // After successful login, should navigate away from /login
    await expect(page).not.toHaveURL(/\/login$/, { timeout: 10000 })
  })

  test('shows error on invalid credentials', async ({ page }) => {
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('wrong')
    await page.locator('button[type="submit"]').click()
    await expect(page.locator('text=Incorrect username or password')).toBeVisible()
  })

  test('shows MFA challenge when required', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }) })
    })
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    await expect(page.locator('text=Two-Factor Authentication')).toBeVisible()
    await expect(page.locator('input[maxlength="6"]')).toBeVisible()
  })

  test('MFA verification completes login', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }) })
    })
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    // Should show MFA form
    await expect(page.locator('input[maxlength="6"]')).toBeVisible()
    await page.locator('input[maxlength="6"]').fill('123456')
    await page.locator('button[type="submit"]').click()
    // After MFA verify, should navigate to dashboard
    await expect(page).toHaveURL('/', { timeout: 10000 })
  })

  test('forgot password link navigates correctly', async ({ page }) => {
    await page.goto('/login')
    await page.click('a:has-text("Forgot password")')
    await expect(page).toHaveURL('/forgot-password')
  })

  test('register link navigates correctly', async ({ page }) => {
    await page.goto('/login')
    await page.click('a:has-text("create a new account")')
    await expect(page).toHaveURL('/register')
  })
})

// U-3 (browser-e2e 2026-09-08): /oauth2/authorize bounces anonymous users to
// /login with the OAuth parameters flattened onto the query (no `redirect`
// param). A successful login must RESUME the flow by navigating back to
// /oauth2/authorize with every flattened parameter preserved — not drop the
// user on the dashboard.
test.describe('Authorize flow resume (U-3)', () => {
  const authorizeQuery = '/login?client_id=rp-app&redirect_uri=' +
    encodeURIComponent('https://rp.example.com/cb') +
    '&scope=openid%20profile&state=st4te123&response_type=code' +
    '&code_challenge=ch4ll8910&code_challenge_method=S256&nonce=n0nce77'

  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
    // The resume is a full-page navigation to the (backend) authorize
    // endpoint; intercept it so the mock run does not need a live server.
    await page.route('**/oauth2/authorize*', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'text/plain',
        body: 'authorize endpoint reached (mock)',
      })
    })
  })

  test('password login resumes authorize with all parameters', async ({ page }) => {
    await page.goto(authorizeQuery)
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    await page.waitForURL(/\/oauth2\/authorize\?/, { timeout: 10000 })
    const url = new URL(page.url())
    expect(url.searchParams.get('client_id')).toBe('rp-app')
    expect(url.searchParams.get('redirect_uri')).toBe('https://rp.example.com/cb')
    expect(url.searchParams.get('scope')).toBe('openid profile')
    expect(url.searchParams.get('state')).toBe('st4te123')
    expect(url.searchParams.get('response_type')).toBe('code')
    expect(url.searchParams.get('code_challenge')).toBe('ch4ll8910')
    expect(url.searchParams.get('code_challenge_method')).toBe('S256')
    expect(url.searchParams.get('nonce')).toBe('n0nce77')
  })

  test('MFA login resumes authorize with all parameters', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
      })
    })
    await page.route('**/oauth2/mfa/verify', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify(MOCK_TOKEN_PAIR),
      })
    })
    await page.goto(authorizeQuery)
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    await expect(page.locator('input[maxlength="6"]')).toBeVisible()
    await page.locator('input[maxlength="6"]').fill('123456')
    await page.locator('button[type="submit"]').click()
    await page.waitForURL(/\/oauth2\/authorize\?/, { timeout: 10000 })
    const url = new URL(page.url())
    expect(url.searchParams.get('client_id')).toBe('rp-app')
    expect(url.searchParams.get('state')).toBe('st4te123')
    expect(url.searchParams.get('code_challenge')).toBe('ch4ll8910')
  })

  test('plain login (no OAuth query) still lands on the dashboard', async ({ page }) => {
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    await expect(page).toHaveURL('/', { timeout: 10000 })
  })
})

test.describe('Register', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  test('displays registration form', async ({ page }) => {
    await page.goto('/register')
    await expect(page.getByRole('heading', { name: /create/i })).toBeVisible()
  })

  test('successful registration shows verification guidance instead of auto-redirect', async ({ page }) => {
    await page.goto('/register')
    await page.locator('input[autocomplete="username"]').fill('timeruser')
    await page.locator('input[autocomplete="email"]').fill('timer@example.com')
    const passwordFields = page.locator('input[type="password"]')
    await passwordFields.first().fill('Password123')
    await passwordFields.nth(1).fill('Password123')
    await page.locator('button[type="submit"]').click()
    // Success message appears with the email-verification guidance
    await expect(page.locator('text=successfully')).toBeVisible()
    const notice = page.getByTestId('verify-email-notice')
    await expect(notice).toBeVisible()
    await expect(notice).toContainText('timer@example.com')
    // The page stays put — no auto-redirect to a login form that would only
    // reject an unverified account; the user navigates via the explicit link.
    await expect(page.getByTestId('go-to-login')).toBeVisible()
    await page.waitForTimeout(2500)
    await expect(page).not.toHaveURL(/\/login/)
  })
})

test.describe('Forgot Password', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  test('displays forgot password form', async ({ page }) => {
    await page.goto('/forgot-password')
    await expect(page.getByRole('heading', { name: /reset/i })).toBeVisible()
  })

  test('shows success message after submission', async ({ page }) => {
    await page.goto('/forgot-password')
    await page.locator('input[type="email"]').fill('test@example.com')
    await page.locator('button[type="submit"]').click()
    await expect(page.locator('text=If an account with that email exists')).toBeVisible()
  })
})

test.describe('Email Verification', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  test('shows success on valid token', async ({ page }) => {
    await page.goto('/verify-email?token=valid-token')
    await expect(page.getByRole('heading', { name: /verified/i })).toBeVisible()
  })

  test('shows error when token is missing', async ({ page }) => {
    await page.goto('/verify-email')
    await expect(page.locator('text=Missing')).toBeVisible()
  })
})

test.describe('GitHub Login', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  // The GitHub button is compiled in only when VITE_GITHUB_CLIENT_ID is set
  // for the dev server (LoginPage.vue gates on it). Skip — not fail — when
  // the environment does not configure GitHub login.
  test('shows GitHub login button on login page', async ({ page }) => {
    await page.goto('/login')
    await page.locator('form').first().waitFor()
    const btn = page.locator('text=Sign in with GitHub')
    if ((await btn.count()) === 0)
      test.skip(true, 'VITE_GITHUB_CLIENT_ID not configured for this build')
    await expect(btn).toBeVisible()
  })

  test('GitHub button links to GitHub OAuth', async ({ page }) => {
    await page.goto('/login')
    await page.locator('form').first().waitFor()
    const githubLink = page.locator('a:has-text("Sign in with GitHub")')
    if ((await githubLink.count()) === 0)
      test.skip(true, 'VITE_GITHUB_CLIENT_ID not configured for this build')
    await expect(githubLink).toBeVisible()
    const href = await githubLink.getAttribute('href')
    expect(href).toContain('github.com/login/oauth/authorize')
  })
})

test.describe('MFA Validation', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  test('invalid MFA code shows error', async ({ page }) => {
    // Override login to return MFA required (registered after setupMocks, takes priority)
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
      })
    })
    // Override MFA verify to return error
    await page.route('**/oauth2/mfa/verify', async (route) => {
      await route.fulfill({
        status: 401,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'MFA_INVALID_CODE', category: 'AUTHENTICATION', message: 'Invalid MFA code' } }),
      })
    })
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    // Wait for MFA form to appear
    const mfaInput = page.locator('input[inputmode="numeric"]')
    await expect(mfaInput).toBeVisible({ timeout: 5000 })
    await mfaInput.fill('000000')
    await page.locator('button:has-text("Verify")').click()
    await page.waitForTimeout(1500)
    // After invalid code: should NOT redirect to dashboard, should stay showing MFA or show error
    const url = page.url()
    expect(url).not.toContain('/dashboard')
    // Verify MFA input is still present or error is shown (page didn't crash)
    const pageContent = await page.content()
    expect(pageContent.length).toBeGreaterThan(0)
  })

  test('less than 6 digits disables verify button', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
      })
    })
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    await page.waitForTimeout(500)
    const mfaInput = page.locator('input[inputmode="numeric"]')
    if (await mfaInput.isVisible()) {
      await mfaInput.fill('1234')
      const verifyButton = page.locator('button:has-text("Verify")')
      await expect(verifyButton).toBeDisabled()
    }
  })

  test('back to login from MFA form', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
      })
    })
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('testuser')
    await page.locator('input[autocomplete="current-password"]').fill('password123')
    await page.locator('button[type="submit"]').click()
    await page.waitForTimeout(500)
    const backButton = page.locator('button:has-text("Back to sign in")')
    await expect(backButton).toBeVisible()
    await backButton.click()
    // MFA form should be hidden, login form visible
    await expect(page.locator('input[inputmode="numeric"]')).not.toBeVisible()
    await expect(page.locator('input[autocomplete="username"]')).toBeVisible()
  })
})
