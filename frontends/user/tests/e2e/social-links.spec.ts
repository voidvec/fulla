// B2 social account link/unlink: SecurityPage's Connected Accounts card.
// Covers the list rendering, the unlink confirm+refresh interaction, and the
// last-credential guard error surfacing. The link button's outbound redirect
// to github.com is out of e2e scope (same gap as the GitHub login e2e); the
// callback page's link branch is exercised via a direct visit with state=link.
import { test, expect } from '@playwright/test'
import { setupMocks, loginUser, mockApiError } from './helpers/mock-api'

test.describe('Connected Accounts (social links)', () => {
  test('card renders linked provider with Unlink action', async ({ page }) => {
    await setupMocks(page)
    await loginUser(page)
    await page.goto('/security')

    await expect(page.getByRole('heading', { name: 'Connected Accounts' })).toBeVisible()
    await expect(page.getByText('GitHub', { exact: true })).toBeVisible()
    await expect(page.getByRole('button', { name: 'Unlink' })).toBeVisible()
  })

  test('unlink confirms then refreshes the list', async ({ page }) => {
    await setupMocks(page)
    await loginUser(page)
    await page.goto('/security')

    await page.getByRole('button', { name: 'Unlink' }).click()
    // Confirm the shared AppConfirmDialog (#181).
    await page.getByTestId('confirm-dialog-confirm').click()

    // The mock DELETE always succeeds; after refresh the mock list still
    // returns the entry, so assert the request happened and the success
    // toast appeared instead of an error banner.
    await expect(page.getByText('GitHub account unlinked')).toBeVisible({ timeout: 5000 })
    await expect(page.locator('.bg-error-50')).toHaveCount(0)
  })

  test('unlink cancelled at the confirm dialog sends no request', async ({ page }) => {
    await setupMocks(page)
    await loginUser(page)
    let deleteSeen = false
    await page.route('**/api/me/social/links/*', async (route) => {
      if (route.request().method() === 'DELETE') deleteSeen = true
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{}' })
    })
    await page.goto('/security')

    await page.getByRole('button', { name: 'Unlink' }).click()
    await page.getByTestId('confirm-dialog-cancel').click()
    await page.waitForTimeout(500)

    expect(deleteSeen).toBe(false)
  })

  test('last-credential guard 409 surfaces the backend message', async ({ page }) => {
    await setupMocks(page)
    await loginUser(page)
    await mockApiError(page, '**/api/me/social/links/*', 409, {
      error: {
        code: 'VALIDATION_RESOURCE_CONFLICT',
        category: 'VALIDATION',
        message: 'cannot remove your last sign-in method',
        numeric_code: 3005,
        request_id: 'req-e2e-guard',
      },
    })
    await page.goto('/security')

    await page.getByRole('button', { name: 'Unlink' }).click()
    await page.getByTestId('confirm-dialog-confirm').click()

    await expect(page.locator('.bg-error-50')).toBeVisible({ timeout: 5000 })
  })

  test('callback page with state=link posts the link endpoint, not login', async ({ page }) => {
    await setupMocks(page)
    await loginUser(page)
    let linkSeen = false
    let loginSeen = false
    await page.route('**/api/me/social/links/github', async (route) => {
      if (route.request().method() === 'POST') {
        linkSeen = true
        await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ provider: 'github', subject: '4242', message: 'linked' }) })
      } else {
        await route.continue()
      }
    })
    await page.route('**/api/github/login', async (route) => {
      loginSeen = true
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{}' })
    })

    await page.goto('/callback/github?code=e2e-code&state=link')
    await page.waitForTimeout(1000)

    expect(linkSeen).toBe(true)
    expect(loginSeen).toBe(false)
    await expect(page).toHaveURL(/\/security/)
  })

  // W1 (PR review): a link-flow visit with NO session (logged out in another
  // tab / session expired) must show an error -- it must NEVER fall through
  // to the login POST, which would mint a login session and auto-create an
  // account for an unmapped identity.
  test('callback page with state=link and no session errors out without calling any API', async ({ page }) => {
    await setupMocks(page)
    // No loginUser() -- no tokens in storage.
    let loginSeen = false
    let linkSeen = false
    await page.route('**/api/github/login', async (route) => {
      loginSeen = true
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{}' })
    })
    await page.route('**/api/me/social/links/github', async (route) => {
      linkSeen = true
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{}' })
    })
    // tryRestoreSession finds no refresh_token -> resolves false without
    // network, but stub the token endpoint defensively anyway.
    await page.route('**/oauth2/token', async (route) => {
      await route.fulfill({ status: 400, contentType: 'application/json', body: JSON.stringify({ error: { code: 'AUTH_INVALID_GRANT', category: 'AUTHENTICATION', message: 'invalid' } }) })
    })

    await page.goto('/callback/github?code=e2e-code&state=link')
    await page.waitForTimeout(1000)

    expect(loginSeen).toBe(false)
    expect(linkSeen).toBe(false)
    await expect(page.getByText('Please sign in first', { exact: false })).toBeVisible()
    await expect(page).toHaveURL(/\/callback\/github/)
  })

  // #70: the generalized google callback completes a LOGIN flow — the token
  // response is stored and the user lands on the home page.
  test('google callback page completes login with the issued token pair', async ({ page }) => {
    await setupMocks(page)
    await page.route('**/api/google/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({
          access_token: 'e2e-google-access',
          refresh_token: 'e2e-google-refresh',
          token_type: 'Bearer',
          expires_in: 3600,
        }),
      })
    })
    // auth.fetchUser() after setTokens; the token refresh endpoint is
    // stubbed defensively (the CI property-test leg runs without a live
    // backend — any un-mocked pass through the dev-server proxy dies with
    // ECONNREFUSED and breaks the redirect assertion).
    await page.route('**/oauth2/token', async (route) => {
      await route.fulfill({ status: 400, contentType: 'application/json', body: JSON.stringify({ error: { code: 'AUTH_INVALID_GRANT', category: 'AUTHENTICATION', message: 'stub' } }) })
    })
    await page.route('**/api/me', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ id: 1, username: 'google_e2e', email: 'g@example.test' }),
      })
    })

    await page.goto('/callback/google?code=e2e-google-code')

    // setTokens keeps the access token IN MEMORY (http.ts) and persists
    // only the refresh_token — assert the persisted half, the redirect,
    // and the authenticated shell instead of a localStorage key that is
    // deliberately never written.
    await expect
      .poll(async () => page.evaluate(() => localStorage.getItem('refresh_token')), { timeout: 10_000 })
      .toBe('e2e-google-refresh')
    await expect(page).toHaveURL(/\/$/, { timeout: 10_000 })
    await expect(page.getByRole('heading', { name: 'Dashboard' })).toBeVisible()
  })
})
