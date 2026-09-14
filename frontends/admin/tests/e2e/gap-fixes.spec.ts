import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin } from './helpers/mock-api'

// Coverage for gap-analysis P0/P1 fixes: device approval page (moved from the
// user portal), logs actor_id filter + target columns, users delete/disable/
// enable first-ever click-through, tokens revoke-by-client execution, and the
// dashboard honest health rendering.

test.describe('Device approval page (gap-fix E2)', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
  })

  test('approves a device code with the form-encoded contract', async ({ page }) => {
    const approveRequest = page.waitForRequest('**/oauth2/device/approve')
    await page.click('nav a:has-text("Devices")')
    await page.waitForURL('**/admin/devices')

    await page.fill('#device-user-code', 'wdjb-mjht')
    await page.click('button:has-text("Approve device")')

    const request = await approveRequest
    const body = request.postData() || ''
    expect(body).toContain('user_code=WDJB-MJHT')
    expect(body).toContain('user_id=')
    expect(request.headers()['content-type']).toContain('application/x-www-form-urlencoded')
    await expect(page.getByTestId('device-approve-success')).toBeVisible()
  })

  test('shows an error for an invalid code', async ({ page }) => {
    await page.route('**/oauth2/device/approve', async (route) => {
      await route.fulfill({
        status: 400,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'VALIDATION_INVALID_INPUT', category: 'VALIDATION', message: 'Invalid or expired device code', numeric_code: 1002, request_id: 'req-e2e-dev' } }),
      })
    })
    await page.click('nav a:has-text("Devices")')
    await page.waitForURL('**/admin/devices')
    await page.fill('#device-user-code', 'BAD-CODE')
    await page.click('button:has-text("Approve device")')
    await expect(page.getByTestId('device-approve-error')).toContainText('Invalid input')
  })
})

test.describe('Audit logs enhancements (gap-fix P1)', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Audit Logs")')
    await page.waitForURL('**/admin/logs')
  })

  test('renders the target column', async ({ page }) => {
    await expect(page.locator('th:has-text("Target")')).toBeVisible()
  })

  test('actor_id filter is sent as a query param', async ({ page }) => {
    // Let the initial load's own /api/admin/logs round-trip finish so the
    // waitForRequest below cannot catch that request instead.
    await page.waitForLoadState('networkidle')
    const logsRequest = page.waitForRequest('**/api/admin/logs*')
    await page.fill('input[placeholder="filter by actor uuid"]', '6cb8d705-a0a8')
    await page.click('button:has-text("Apply")')

    const request = await logsRequest
    expect(request.url()).toContain('actor_id=6cb8d705-a0a8')
  })
})

test.describe('User management click-through (gap-fix P1 tests)', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Users")')
    await page.waitForURL('**/admin/users')
  })

  test('deletes a user after confirmation', async ({ page }) => {
    // Native confirm() must be accepted for the flow to proceed.
    page.on('dialog', (dialog) => dialog.accept())
    const deleteRequest = page.waitForRequest('**/api/admin/users/*')
    await page.locator('button:has-text("Delete")').first().click()

    const request = await deleteRequest
    expect(request.method()).toBe('DELETE')
    await expect(page.locator('text=User deleted successfully')).toBeVisible()
  })

  test('disables a user from the detail page', async ({ page }) => {
    page.on('dialog', (dialog) => dialog.accept())
    await page.locator('a:has-text("Details")').first().click()
    await page.waitForURL('**/admin/users/*')

    const disableRequest = page.waitForRequest('**/api/admin/users/*/disable')
    await page.locator('button:has-text("Disable Account")').click()
    const request = await disableRequest
    expect(request.method()).toBe('PUT')
  })

  test('surfaces roles_failed as an error instead of blanket success', async ({ page }) => {
    // exact:true — otherwise the header's "+ Create User" also matches and
    // the click lands behind the modal overlay.
    await page.getByRole('button', { name: '+ Create User' }).click()
    await page.fill('input[placeholder="newuser"]', 'brandnew')
    await page.fill('input[type="password"]', 'Password123!')
    // Role assignment will fail server-side; the page must not report
    // "User created successfully".
    await page.route('**/api/admin/users', async (route) => {
      if (route.request().method() === 'POST') {
        await route.fulfill({
          status: 201,
          contentType: 'application/json',
          body: JSON.stringify({ status: 'success', user: { id: 99 }, roles_assigned: [], roles_failed: ['admin'], warning: 'Some roles could not be assigned' }),
        })
      } else {
        route.continue()
      }
    })
    await page.getByRole('button', { name: 'Create User', exact: true }).click()
    await expect(page.locator('text=role assignment failed for: admin')).toBeVisible()
  })
})

test.describe('Bulk token revocation (gap-fix P1 tests)', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Tokens")')
    await page.waitForURL('**/admin/tokens')
  })

  test('revokes all tokens for a client from the bulk menu', async ({ page }) => {
    // Local override registered after setupAuthenticatedMocks — the shared
    // `**/api/admin/tokens/**` handler continues non-DELETE verbs to the
    // proxy, so this test provides its own success fulfillment.
    await page.route('**/api/admin/tokens/revoke-by-client', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ status: 'success', count: 5 }) })
    })
    await page.click('button:has-text("Revoke All by App")')
    const revokeRequest = page.waitForRequest('**/api/admin/tokens/revoke-by-client')
    await page.locator('button:has-text("fulla-portal")').click()

    // The custom confirm dialog guards the destructive action; the request
    // fires only after Confirm.
    await page.click('button:has-text("Confirm")')
    const request = await revokeRequest
    expect(request.method()).toBe('POST')
    expect(request.postDataJSON()).toEqual({ client_id: 'fulla-portal' })
    // Gap-fix: the success banner reports the backend count.
    await expect(page.getByTestId('tokens-success')).toContainText('Revoked 5 token')
  })
})

test.describe('Dashboard honest health rendering (gap-fix E3)', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
  })

  test('renders a degraded system status instead of binary green/red', async ({ page }) => {
    await page.route('**/health/ready', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'degraded', database: 'connected', redis: 'disconnected' }),
      })
    })
    await page.goto('/admin/')
    await expect(page.getByTestId('system-status')).toHaveText('Degraded')
    await expect(page.getByTestId('redis-status')).toHaveText('disconnected')
  })

  test('shows logs_today from the stats endpoint', async ({ page }) => {
    await page.goto('/admin/')
    await expect(page.locator('text=Logs Today')).toBeVisible()
    await expect(page.locator('text=Audit events')).toBeVisible()
  })
})
