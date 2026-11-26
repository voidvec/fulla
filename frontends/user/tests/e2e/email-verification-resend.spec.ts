import { test, expect, type Page } from '@playwright/test'
import { setupMocks } from './helpers/mock-api'

// UF-05: with auth.require_email_verification on, /oauth2/login refuses an
// account whose address is not verified with 403 AUTHZ_ACCESS_DENIED — the
// only thing on that path answered with that code. The login page offers a
// new verification link through the anonymous resend-by-email endpoint.

const CONFIRMATION =
  'If the email exists and is unverified, a verification link has been sent.'

/** The backend's answer to a correct password on an unverified account. */
async function refuseLoginAsUnverified(page: Page) {
  await page.route('**/oauth2/login', (route) =>
    route.fulfill({
      status: 403,
      contentType: 'application/json',
      body: JSON.stringify({
        error: { code: 'AUTHZ_ACCESS_DENIED', category: 'AUTHORIZATION', message: 'login: email not verified' },
      }),
    }),
  )
}

async function submitSignIn(page: Page, identifier: string) {
  await page.goto('/login')
  await page.locator('input[autocomplete="username"]').fill(identifier)
  await page.locator('input[autocomplete="current-password"]').fill('correct-password')
  await page.getByRole('button', { name: 'Sign In', exact: true }).click()
}

test.describe('Email verification recovery at sign-in (UF-05)', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  test('a refused sign-in offers a new verification link and sends it to the address', async ({ page }) => {
    await refuseLoginAsUnverified(page)
    const resend = page.waitForRequest(
      (r) => r.url().endsWith('/api/verify-email/resend-by-email') && r.method() === 'POST',
    )
    await submitSignIn(page, 'someone@example.com')

    const panel = page.getByTestId('verify-email-panel')
    await expect(panel).toBeVisible()
    await expect(panel).toContainText('Email address not verified')
    // The sign-in field takes a username too; an address typed there is
    // carried over so the usual case needs no second typing.
    await expect(panel.locator('input[type="email"]')).toHaveValue('someone@example.com')

    await panel.getByRole('button', { name: 'Resend verification email' }).click()
    const request = await resend
    expect(request.postDataJSON()).toEqual({ email: 'someone@example.com' })
    expect((request.headers()['content-type'] || '')).toContain('application/json')

    // The confirmation is the same for any address (anti-enumeration).
    await expect(page.getByTestId('verify-email-sent')).toHaveText(CONFIRMATION)
    await expect(page.getByTestId('verify-email-form')).toHaveCount(0)
    await expect(page).toHaveURL(/\/login/)
  })

  test('a username typed at sign-in leaves the address to be filled in', async ({ page }) => {
    await refuseLoginAsUnverified(page)
    await submitSignIn(page, 'someone')

    const panel = page.getByTestId('verify-email-panel')
    await expect(panel).toBeVisible()
    const address = panel.locator('input[type="email"]')
    await expect(address).toHaveValue('')
    await expect(panel.getByRole('button', { name: 'Resend verification email' })).toBeDisabled()

    await address.fill('someone@example.com')
    await expect(panel.getByRole('button', { name: 'Resend verification email' })).toBeEnabled()
  })

  test('the resend request travels without a session', async ({ page }) => {
    await refuseLoginAsUnverified(page)
    const resend = page.waitForRequest((r) => r.url().endsWith('/api/verify-email/resend-by-email'))
    await submitSignIn(page, 'someone@example.com')
    await page.getByTestId('verify-email-panel').getByRole('button', { name: 'Resend verification email' }).click()

    const request = await resend
    expect(request.headers()['authorization']).toBeUndefined()
  })

  test('a rate-limited resend shows the catalog copy and no confirmation', async ({ page }) => {
    await refuseLoginAsUnverified(page)
    await page.route('**/api/verify-email/resend-by-email', (route) =>
      route.fulfill({
        status: 429,
        contentType: 'application/json',
        body: JSON.stringify({
          error: { code: 'VALIDATION_RATE_LIMITED', category: 'VALIDATION', message: 'Too many requests' },
        }),
      }),
    )
    await submitSignIn(page, 'someone@example.com')
    await page.getByTestId('verify-email-panel').getByRole('button', { name: 'Resend verification email' }).click()

    await expect(page.getByTestId('verify-email-error')).toHaveText('Too many requests. Please try again later')
    await expect(page.getByTestId('verify-email-sent')).toHaveCount(0)
  })

  test('wrong credentials do not offer a verification link', async ({ page }) => {
    // The default mock answers password=wrong with 401 AUTH_INVALID_CREDENTIALS.
    await page.goto('/login')
    await page.locator('input[autocomplete="username"]').fill('someone@example.com')
    await page.locator('input[autocomplete="current-password"]').fill('wrong')
    await page.getByRole('button', { name: 'Sign In', exact: true }).click()

    await expect(page.getByText('Incorrect username or password')).toBeVisible()
    await expect(page.getByTestId('verify-email-panel')).toHaveCount(0)
  })
})
