import { test, expect, type Page } from '@playwright/test'
import { setupMocks } from './helpers/mock-api'

// UF-08: a provider identity that has no local account (and automatic account
// creation is off) is answered with AUTH_SOCIAL_ACCOUNT_NOT_LINKED. The
// catalog has real copy for that code, and both callback pages offer the way
// forward — the link flow on the security page — instead of a dead end.

const NOT_LINKED = JSON.stringify({
  error: { code: 'AUTH_SOCIAL_ACCOUNT_NOT_LINKED', category: 'AUTHORIZATION', message: 'not linked' },
})
const NOT_LINKED_COPY =
  'This third-party account is not linked to a user yet. Link it in Security settings first, then try again'

/** Seeds the in-page context of a login this SPA initiated, then makes the
 * exchange fail the given way. */
async function seedFailingLogin(page: Page, provider: string, status: number, body: string) {
  await setupMocks(page)
  await page.goto('/login')
  const state = 'c'.repeat(64)
  await page.evaluate(({ state, provider }) => {
    sessionStorage.setItem('fulla_social_flow:' + state, JSON.stringify({
      purpose: 'login',
      provider,
      redirectUri: location.origin + '/callback/' + provider,
      createdAt: Date.now(),
    }))
  }, { state, provider })
  await page.route(`**/api/${provider}/login`, (route) =>
    route.fulfill({ status, contentType: 'application/json', body }),
  )
  return state
}

const CALLBACK_PAGES = [
  { provider: 'github', path: '/callback/github' },
  { provider: 'google', path: '/callback/google' },
]

for (const { provider, path } of CALLBACK_PAGES) {
  test(`${provider}: an unlinked account shows the catalog copy and the way to link it`, async ({ page }) => {
    const state = await seedFailingLogin(page, provider, 403, NOT_LINKED)
    await page.goto(`${path}?code=callback-code&state=${state}`)

    // Real catalog copy, not the unknown-error fallback.
    await expect(page.getByText(NOT_LINKED_COPY)).toBeVisible()
    const guide = page.getByTestId('social-link-guide')
    await expect(guide).toBeVisible()
    await expect(guide).toHaveAttribute('href', '/security')
    await expect(page).toHaveURL(new RegExp(`${path}$`))
  })
}

test('github: a different failure does not offer the link guide', async ({ page }) => {
  const state = await seedFailingLogin(page, 'github', 401, JSON.stringify({
    error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: 'rejected' },
  }))
  await page.goto(`/callback/github?code=callback-code&state=${state}`)

  await expect(page.getByText('Incorrect username or password')).toBeVisible()
  await expect(page.getByTestId('social-link-guide')).toHaveCount(0)
})
