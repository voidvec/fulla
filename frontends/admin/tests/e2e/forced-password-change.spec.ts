import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin } from './helpers/mock-api'

// PR #157 review MINOR 9: e2e coverage for the forced first-login password
// change surfaces — the login password_change_required branch, the
// ?must_change_password=1 query branch (server 302 landing), the
// /oauth2/password/change request contract (JSON body), and the device
// approval page's ?user_code= prefill (#146).

test.describe('Forced password change (admin console)', () => {
  test('login password_change_required shows the inline form; change posts JSON and succeeds', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ password_change_required: true, message: 'Password change required.' }),
      })
    })
    const changeRequest = page.waitForRequest('**/oauth2/password/change')
    await page.route('**/oauth2/password/change', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ message: 'Password changed successfully.' }),
      })
    })

    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'admin')
    await page.fill('input[type="password"]', 'admin')
    await page.click('button[type="submit"]')

    // The inline change form replaces the login form.
    await expect(page.locator('#old-password-field')).toBeVisible()

    await page.fill('#old-password-field', 'admin')
    await page.fill('#new-password-field', 'NewPassw0rd!')
    await page.fill('#confirm-password-field', 'NewPassw0rd!')
    await page.click('button:has-text("Change password")')

    // Contract: JSON body with old_password/new_password (form-encoded
    // bodies are rejected by the endpoint).
    const request = await changeRequest
    expect((request.headers()['content-type'] || '').toLowerCase()).toContain('application/json')
    const body = JSON.parse(request.postData() || '{}')
    expect(body.old_password).toBe('admin')
    expect(body.new_password).toBe('NewPassw0rd!')

    await expect(page.getByTestId('forced-password-change-done')).toBeVisible()
  })

  test('?must_change_password=1 shows the change form without a login round-trip', async ({ page }) => {
    // The server's authorize gate 302s flagged fulla-admin-console users here.
    await page.goto('/admin/login?must_change_password=1')
    await expect(page.locator('#old-password-field')).toBeVisible()
    await expect(page.locator('input[type="text"]')).toBeHidden()
  })

  test('mismatched confirmation shows an error and sends nothing', async ({ page }) => {
    await page.route('**/oauth2/login', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ password_change_required: true }),
      })
    })
    let changeCalls = 0
    await page.route('**/oauth2/password/change', async (route) => {
      changeCalls++
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{}' })
    })

    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'admin')
    await page.fill('input[type="password"]', 'admin')
    await page.click('button[type="submit"]')
    await expect(page.locator('#old-password-field')).toBeVisible()

    await page.fill('#old-password-field', 'admin')
    await page.fill('#new-password-field', 'NewPassw0rd!')
    await page.fill('#confirm-password-field', 'DifferentPass1!')
    await page.click('button:has-text("Change password")')

    await expect(page.locator('.text-error-700, [role="alert"]').first()).toBeVisible()
    expect(changeCalls).toBe(0)
  })
})

test.describe('Device approval prefill (#146)', () => {
  test('verification_uri_complete ?user_code prefills the code input', async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.goto('/admin/devices?user_code=wdjb-mjht')
    await expect(page.locator('#device-user-code')).toHaveValue('wdjb-mjht')
  })
})
