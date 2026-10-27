import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin, MOCK_CLIENTS } from './helpers/mock-api'

test.describe('Applications Management', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Applications")')
    await page.waitForURL('**/admin/applications')
  })

  test('displays applications list with correct columns', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Applications')
    await expect(page.locator('th:has-text("Name")')).toBeVisible()
    await expect(page.locator('th:has-text("Client ID")')).toBeVisible()
    await expect(page.locator('th:has-text("Type")')).toBeVisible()
    await expect(page.locator('th:has-text("Actions")')).toBeVisible()
  })

  test('shows all registered clients', async ({ page }) => {
    for (const client of MOCK_CLIENTS) {
      await expect(page.locator(`text=${client.client_id}`)).toBeVisible()
      await expect(page.locator(`text=${client.name}`)).toBeVisible()
    }
  })

  test('displays client type badges correctly', async ({ page }) => {
    await expect(page.locator('span:has-text("PUBLIC")')).toBeVisible()
    // v1.4.0 mock set carries multiple CONFIDENTIAL clients (admin + self-
    // registered) — assert presence, not uniqueness.
    await expect(page.locator('span:has-text("CONFIDENTIAL")').first()).toBeVisible()
  })

  test('opens create application modal', async ({ page }) => {
    await page.click('button:has-text("Create Application")')
    await expect(page.locator('h3:has-text("Create Application")')).toBeVisible()
    await expect(page.locator('input[placeholder="My App"]')).toBeVisible()
    await expect(page.locator('select')).toBeVisible()
    // Grant types shown as checkboxes
    await expect(page.locator('label:has-text("Authorization Code")')).toBeVisible()
    await expect(page.locator('label:has-text("Refresh Token")')).toBeVisible()
    await expect(page.locator('label:has-text("Client Credentials")')).toBeVisible()
    await expect(page.locator('label:has-text("Device Code")')).toBeVisible()
  })

  test('creates a new application and shows secret', async ({ page }) => {
    await page.click('button:has-text("Create Application")')

    // Fill form
    await page.fill('input[placeholder="My App"]', 'Test Application')
    await page.selectOption('select', 'CONFIDENTIAL')
    await page.fill('input[placeholder*="myapp.com"]', 'https://test.com/callback')
    // Grant types are checkboxes - authorization_code is checked by default, add refresh_token
    await page.locator('label:has-text("Refresh Token") input[type="checkbox"]').check()

    // Submit - use the form submit button inside the modal
    await page.locator('.fixed button[type="submit"]').click()

    // Should show secret modal
    await expect(page.locator('h3:has-text("Client Secret")')).toBeVisible()
    await expect(page.locator('text=Copy this secret now')).toBeVisible()
    await expect(page.locator('.font-mono.select-all')).toContainText('generated-secret-abc123xyz')
  })

  test('closes secret modal with Done button', async ({ page }) => {
    await page.click('button:has-text("Create Application")')
    await page.fill('input[placeholder="My App"]', 'Test')
    await page.locator('.fixed button[type="submit"]').click()

    await expect(page.locator('h3:has-text("Client Secret")')).toBeVisible()
    await page.click('button:has-text("Done")')
    await expect(page.locator('h3:has-text("Client Secret")')).not.toBeVisible()
  })

  test('cancel button closes create modal', async ({ page }) => {
    await page.click('button:has-text("Create Application")')
    await expect(page.locator('h3:has-text("Create Application")')).toBeVisible()
    await page.click('button:has-text("Cancel")')
    await expect(page.locator('h3:has-text("Create Application")')).not.toBeVisible()
  })

  test('delete client with confirmation', async ({ page }) => {
    await page.click('button:has-text("Delete")')
    // Confirm the shared AppConfirmDialog (#181).
    await page.getByTestId('confirm-dialog-confirm').click()
    // After delete, page should still show (mock returns same list).
    // .first(): the confirm dialog's own h2 ("Confirm action") may still be
    // mid-teardown when this assertion first polls.
    await expect(page.locator('h2').first()).toContainText('Applications')
  })

  test('reset secret for confidential client', async ({ page }) => {
    await page.click('button:has-text("Reset Secret")')
    await page.getByTestId('confirm-dialog-confirm').click()

    // Should show new secret
    await expect(page.locator('h3:has-text("Client Secret")')).toBeVisible()
    await expect(page.locator('.font-mono.select-all')).toContainText('new-secret-after-reset-xyz789')
  })

  test('shows empty state when no clients exist', async ({ page }) => {
    await page.route('**/api/admin/clients', async (route) => {
      if (route.request().method() === 'GET') {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ clients: [] }),
        })
      } else {
        await route.continue()
      }
    })

    // Navigate away and back to trigger a fresh fetch with the new mock
    await page.click('nav a:has-text("Dashboard")')
    await page.click('nav a:has-text("Applications")')
    await page.waitForURL('**/admin/applications')
    await expect(page.locator('text=No applications registered yet')).toBeVisible()
    await expect(page.locator('button:has-text("Create your first application")')).toBeVisible()
  })

  test('no grant type selected shows error', async ({ page }) => {
    await page.click('button:has-text("Create Application")')
    await page.fill('input[placeholder="My App"]', 'No Grant App')
    // Uncheck all grant types inside the modal
    const modal = page.locator('.fixed')
    const checkboxes = modal.locator('input[type="checkbox"]')
    const count = await checkboxes.count()
    for (let i = 0; i < count; i++) {
      if (await checkboxes.nth(i).isChecked()) {
        await checkboxes.nth(i).uncheck()
      }
    }
    await modal.locator('button[type="submit"]').click()
    await page.waitForTimeout(500)
    // The error message should appear (either inside modal or as page-level banner)
    const errorEl = page.locator('.bg-error-50, .text-error-700, [class*="error-"]')
    await expect(errorEl.first()).toBeVisible({ timeout: 3000 })
  })

  test('multiple grant types selection', async ({ page }) => {
    await page.click('button:has-text("Create Application")')
    await page.fill('input[placeholder="My App"]', 'Multi Grant App')
    // Check multiple grant types
    const checkboxes = page.locator('.fixed input[type="checkbox"]')
    const count = await checkboxes.count()
    for (let i = 0; i < Math.min(count, 3); i++) {
      await checkboxes.nth(i).check()
    }
    await page.locator('.fixed button[type="submit"]').click()
    await page.waitForTimeout(500)
    // Should show secret modal (success)
    await expect(page.locator('h3:has-text("Client Secret")')).toBeVisible()
  })

  test('delete client cancel preserves client', async ({ page }) => {
    const deleteButton = page.locator('button:has-text("Delete")').first()
    await deleteButton.click()
    // Cancel in the shared AppConfirmDialog (#181) — no DELETE may fire.
    await page.getByTestId('confirm-dialog-cancel').click()
    await page.waitForTimeout(300)
    // Client should still be visible (dialog was dismissed)
    await expect(page.locator('table tbody tr').first()).toBeVisible()
  })

  // P1-7 (#284): RFC 7591 dynamic client registration form.
  test.describe('RFC 7591 registration', () => {
    test('opens the registration modal with the DCR field set (no logo field)', async ({ page }) => {
      await page.click('button:has-text("Register (RFC 7591)")')
      await expect(page.locator('h3:has-text("Register Application (RFC 7591)")')).toBeVisible()
      // Token-endpoint auth dropdown + post-logout array input are DCR-specific.
      await expect(page.locator('.fixed select').nth(1)).toBeVisible()
      await expect(page.locator('input[placeholder="https://myapp.com/"]')).toBeVisible()
      // The DCR vocabulary has NO device-code grant (the endpoint rejects it)
      // and NO logo_uri field (P1-1 write paths own that). NB: "logo_uri",
      // not "logo" — "Post-logout" contains the substring "logo".
      await expect(page.locator('.fixed label:has-text("Device Code")')).toHaveCount(0)
      await expect(page.locator('.fixed label:has-text("logo_uri")')).toHaveCount(0)
    })

    test('PUBLIC type locks the auth method to none', async ({ page }) => {
      await page.click('button:has-text("Register (RFC 7591)")')
      await page.locator('.fixed select').first().selectOption('PUBLIC')
      const authSelect = page.locator('.fixed select').nth(1)
      await expect(authSelect).toBeDisabled()
      await expect(authSelect).toHaveValue('none')
    })

    test('registers: secret shown once, request shape correct, client lands in the list', async ({ page }) => {
      // Test-scoped routes (registered later = higher precedence) keep a
      // dynamic client list so the acceptance "client visible in the
      // Applications list" is asserted for real. The credential slot is a
      // mock PLACEHOLDER (env-overridable), never a real secret.
      const mockSecret = process.env.E2E_MOCK_DCR_CS ?? 'e2e-mock-value-dcr'
      const registered: Array<Record<string, string>> = []
      let dcrBody: Record<string, unknown> | null = null
      await page.route('**/oauth2/register', async (route) => {
        dcrBody = route.request().postDataJSON() as Record<string, unknown>
        registered.push({
          client_id: 'dcr-e2e-client',
          name: 'E2E DCR App',
          client_type: 'CONFIDENTIAL',
        })
        await route.fulfill({
          status: 201,
          contentType: 'application/json',
          body: JSON.stringify({
            client_id: 'dcr-e2e-client',
            client_secret: mockSecret,
            client_name: 'E2E DCR App',
            client_id_issued_at: 1793000000,
            client_secret_expires_at: 0,
          }),
        })
      })
      await page.route('**/api/admin/clients', async (route) => {
        if (route.request().method() === 'GET') {
          const clients = [...MOCK_CLIENTS, ...registered]
          await route.fulfill({
            status: 200,
            contentType: 'application/json',
            body: JSON.stringify({ clients, total: clients.length }),
          })
        } else {
          await route.continue()
        }
      })

      await page.click('button:has-text("Register (RFC 7591)")')
      await page.fill('input[placeholder="My App"]', 'E2E DCR App')
      await page.fill('input[placeholder*="myapp.com/callback"]', 'https://dcr-e2e.example/callback')
      await page.fill('input[placeholder="https://myapp.com/"]', 'https://dcr-e2e.example/')
      await page.locator('.fixed label:has-text("Refresh Token") input[type="checkbox"]').check()
      await page.locator('.fixed button[type="submit"]').click()

      // Shown-once secret modal (same semantics as create/reset).
      await expect(page.locator('h3:has-text("Client Secret")')).toBeVisible()
      await expect(page.locator('.font-mono.select-all')).toContainText(mockSecret)
      await page.click('button:has-text("Done")')
      await expect(page.locator('h3:has-text("Client Secret")')).not.toBeVisible()

      // The registered client is now in the Applications list.
      await expect(page.locator('table')).toContainText('dcr-e2e-client')

      // The POSTed body matches the documented RFC 7591 contract.
      expect(dcrBody).toMatchObject({
        client_name: 'E2E DCR App',
        client_type: 'CONFIDENTIAL',
        token_endpoint_auth_method: 'client_secret_basic',
        redirect_uris: ['https://dcr-e2e.example/callback'],
        grant_types: ['authorization_code', 'refresh_token'],
        post_logout_redirect_uris: ['https://dcr-e2e.example/'],
      })
    })

    test('confidential without redirect URIs is blocked client-side', async ({ page }) => {
      await page.click('button:has-text("Register (RFC 7591)")')
      await page.fill('input[placeholder="My App"]', 'No Redirect')
      // Leave redirect URIs empty — the inline banner appears and no
      // registration may succeed.
      await page.route('**/oauth2/register', (route) =>
        route.fulfill({ status: 500, contentType: 'application/json', body: '{}' }))
      await page.locator('.fixed button[type="submit"]').click()
      await expect(page.locator('text=at least one redirect URI')).toBeVisible()
      await expect(page.locator('h3:has-text("Client Secret")')).not.toBeVisible()
    })
  })
})
