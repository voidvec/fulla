import { test, expect } from '@playwright/test'
import { setupMocks, loginUser, MOCK_SLOW_TOKEN_PAIR } from './helpers/mock-api'

test.describe('OAuth2 Consent Page', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
    await loginUser(page)
  })

  test('displays consent page with client info', async ({ page }) => {
    await page.goto('/consent?client_id=third-party&scope=openid+profile+email&redirect_uri=http://example.com/callback&state=test123')
    await expect(page.getByRole('heading', { name: /authorize/i })).toBeVisible()
    await expect(page.locator('text=third-party')).toBeVisible()
  })

  test('shows requested scopes', async ({ page }) => {
    await page.goto('/consent?client_id=third-party&scope=openid+profile+email&redirect_uri=http://example.com/callback&state=test123')
    await expect(page.locator('text=Verify your identity')).toBeVisible()
    await expect(page.locator('text=Access your basic profile')).toBeVisible()
    await expect(page.locator('text=Access your email')).toBeVisible()
  })

  test('has approve and deny buttons', async ({ page }) => {
    await page.goto('/consent?client_id=third-party&scope=openid&redirect_uri=http://example.com/callback&state=test')
    await expect(page.locator('button:has-text("Authorize")')).toBeVisible()
    await expect(page.locator('button:has-text("Deny")')).toBeVisible()
  })

  // v1.4.0 open platform: consent screens attribute self-registered apps to
  // their owner (owner_name on the authorize -> consent redirect).
  test('shows owner attribution when owner_name is present', async ({ page }) => {
    await page.goto('/consent?client_id=app_community&scope=openid&redirect_uri=http://example.com/callback&state=t&owner_name=Ada%20Lovelace')
    await expect(page.getByTestId('consent-owner-name')).toContainText('Ada Lovelace')
  })

  test('omits owner attribution for official apps', async ({ page }) => {
    await page.goto('/consent?client_id=fulla-portal&scope=openid&redirect_uri=http://example.com/callback&state=t')
    await expect(page.getByTestId('consent-owner-name')).toHaveCount(0)
  })
})

// Gap-fix E7: the consent POST must carry the server-provided user_id (URL
// query first, store sub as fallback). These cases actually click the buttons
// — previously they were never executed (gap-analysis §5.2 zero-coverage).
test.describe('Consent actions', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
    await loginUser(page)
  })

  test('approve submits user_id from the server-provided query param', async ({ page }) => {
    await page.goto('/consent?client_id=third-party&scope=openid+profile&redirect_uri=http://example.com/callback&state=st1&user_id=server-user-42')
    const consentRequest = page.waitForRequest('**/oauth2/consent')
    await page.locator('button:has-text("Authorize")').click()

    const request = await consentRequest
    const body = request.postData() || ''
    expect(body).toContain('action=approve')
    expect(body).toContain('user_id=server-user-42')
  })

  test('deny submits the deny action', async ({ page }) => {
    await page.goto('/consent?client_id=third-party&scope=openid&redirect_uri=http://example.com/callback&state=st2&user_id=server-user-42')
    const consentRequest = page.waitForRequest('**/oauth2/consent')
    await page.locator('button:has-text("Deny")').click()

    const request = await consentRequest
    expect(request.postData()).toContain('action=deny')
  })

  test('without any user id the actions are disabled', async ({ page }) => {
    // No user_id in the query and an empty userinfo sub → submitting would 500
    // server-side; the page must block the action instead (gap-fix E7).
    // (userinfo answers 200 with an empty sub so no 401-refresh cascade fires.)
    await page.route('**/oauth2/userinfo', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ sub: '', name: '' }) })
    })
    await page.goto('/consent?client_id=third-party&scope=openid&redirect_uri=http://example.com/callback&state=st3')
    await expect(page.getByTestId('consent-missing-user')).toBeVisible()
    await expect(page.locator('button:has-text("Authorize")')).toBeDisabled()
    await expect(page.locator('button:has-text("Deny")')).toBeDisabled()
  })
})

test.describe('Callback Page', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  // U-4: the page only redeems a code when THIS SPA holds the PKCE verifier
  // for THE FLOW — the stash is { state, verifier } JSON and its state must
  // match the callback's state (PR #180 review M6). Without a matching stash
  // the code belongs to an external app's flow and redeeming here would only
  // burn it.
  async function seedVerifier(page: import('@playwright/test').Page, state: string) {
    await page.addInitScript((s: string) => {
      sessionStorage.setItem(
        'pkce_code_verifier',
        JSON.stringify({ state: s, verifier: 'mock-verifier-e2e-seeded' }),
      )
    }, state)
  }

  test('exchanges code for token and redirects', async ({ page }) => {
    await seedVerifier(page, 'test-state')
    await page.goto('/callback?code=test-auth-code&state=test-state')
    // After successful token exchange, should redirect to /
    await expect(page).toHaveURL('/', { timeout: 10000 })
  })

  test('shows error when error param present', async ({ page }) => {
    await page.goto('/callback?error=access_denied&error_description=User+denied+access')
    await expect(page.locator('text=User denied access')).toBeVisible()
  })

  test('shows error when no code present', async ({ page }) => {
    await page.goto('/callback')
    await expect(page.locator('text=No authorization code')).toBeVisible()
  })

  test('without a local verifier shows the external-flow notice and never touches the token endpoint', async ({ page }) => {
    let tokenCalled = false
    await page.route('**/oauth2/token', async (route) => {
      tokenCalled = true
      await route.fulfill({ status: 400, contentType: 'application/json', body: '{}' })
    })
    await page.goto('/callback?code=external-flow-code&state=st-ext')
    await expect(page.getByTestId('callback-external-flow')).toBeVisible()
    await page.waitForTimeout(300)
    expect(tokenCalled).toBe(false)
    // Stays on the callback page (nothing to redeem, nowhere to go).
    await expect(page).toHaveURL(/\/callback/)
  })

  test('stale verifier from another flow (state mismatch) is not used to redeem', async ({ page }) => {
    // M6: a stash left behind by an abandoned login must not qualify an
    // externally-initiated landing — state is the correlation key.
    let tokenCalled = false
    await page.route('**/oauth2/token', async (route) => {
      tokenCalled = true
      await route.fulfill({ status: 400, contentType: 'application/json', body: '{}' })
    })
    await seedVerifier(page, 'login-flow-state')
    await page.goto('/callback?code=external-code&state=external-state')
    await expect(page.getByTestId('callback-external-flow')).toBeVisible()
    await page.waitForTimeout(300)
    expect(tokenCalled).toBe(false)
  })

  test('loading spinner shown while exchanging code', async ({ page }) => {
    await seedVerifier(page, 'test-state')
    // Delay token exchange to observe spinner
    await page.route('**/oauth2/token', async (route) => {
      await new Promise(resolve => setTimeout(resolve, 800))
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify(MOCK_SLOW_TOKEN_PAIR),
      })
    })
    await page.goto('/callback?code=valid-code&state=test-state')
    // Spinner should be visible during exchange
    await expect(page.locator('.animate-spin')).toBeVisible({ timeout: 3000 })
    // After exchange completes, redirect happens
    await page.waitForTimeout(1500)
    const url = page.url()
    expect(url).not.toContain('/callback')
  })
})
