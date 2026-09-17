import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin, MOCK_SCOPES } from './helpers/mock-api'

test.describe('Settings & Scopes', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Settings")')
    await page.waitForURL('**/admin/settings')
  })

  test('displays settings page title', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Settings & Scopes')
  })

  test('shows OAuth2 Scopes section', async ({ page }) => {
    await expect(page.locator('h3:has-text("OAuth2 Scopes")')).toBeVisible()
  })

  test('displays scope table with correct columns', async ({ page }) => {
    await expect(page.locator('th:has-text("Name")')).toBeVisible()
    await expect(page.locator('th:has-text("Description")')).toBeVisible()
    await expect(page.locator('th:has-text("Mapped Role")')).toBeVisible()
    await expect(page.locator('th:has-text("Default")')).toBeVisible()
    await expect(page.locator('th:has-text("Admin Only")')).toBeVisible()
  })

  test('shows all scopes with correct data', async ({ page }) => {
    for (const scope of MOCK_SCOPES) {
      await expect(page.locator(`text=${scope.name}`).first()).toBeVisible()
    }
  })

  test('displays scope descriptions', async ({ page }) => {
    await expect(page.locator('text=OpenID Connect')).toBeVisible()
    await expect(page.locator('text=User profile')).toBeVisible()
    await expect(page.locator('text=Admin access')).toBeVisible()
  })

  test('shows default scope indicators', async ({ page }) => {
    const openidRow = page.locator('tbody tr').first()
    const defaultCell = openidRow.locator('td').nth(3)
    await expect(defaultCell).not.toContainText('—')
  })

  test('shows admin-only scope indicators', async ({ page }) => {
    const adminRow = page.locator('tbody tr').nth(2)
    const adminOnlyCell = adminRow.locator('td').nth(4)
    await expect(adminOnlyCell).not.toContainText('—')
  })

  test('shows mapped role for admin scope', async ({ page }) => {
    const adminRow = page.locator('tr:has-text("Admin access")')
    await expect(adminRow.locator('td').nth(2)).toContainText('admin')
  })

  // OIDC Signing Keys section
  test('shows OIDC Signing Keys section', async ({ page }) => {
    await expect(page.locator('h3:has-text("OIDC Signing Keys")')).toBeVisible()
  })

  test('displays key metadata', async ({ page }) => {
    // #110-B multi-key rendering: summary + per-key rows. The summary also
    // renders 'Active Key ID (kid)' and the active kid value, so bare
    // substring text= locators hit strict-mode collisions — match exact
    // strings instead.
    await expect(page.getByText('Key ID (kid)', { exact: true })).toBeVisible()
    await expect(page.getByText('default-key-1', { exact: true }).first()).toBeVisible()
    await expect(page.getByText('Key Type (kty)', { exact: true })).toBeVisible()
    await expect(page.getByText('RSA', { exact: true })).toBeVisible()
    await expect(page.getByText('Algorithm (alg)', { exact: true })).toBeVisible()
    await expect(page.getByText('RS256', { exact: true })).toBeVisible()
    await expect(page.getByText('(active)', { exact: true })).toBeVisible()
  })

  test('displays JWKS and Discovery URLs', async ({ page }) => {
    await expect(page.locator('text=JWKS Endpoint')).toBeVisible()
    await expect(page.getByText('.well-known/jwks.json')).toBeVisible()
    await expect(page.locator('text=Discovery Endpoint')).toBeVisible()
    await expect(page.getByText('.well-known/openid-configuration')).toBeVisible()
  })

  test('shows key status badge', async ({ page }) => {
    // Exact match: 'Active Key ID (kid)' (summary) and '(active)' (key
    // marker) both substring-match a bare text=active locator.
    await expect(page.getByText('active', { exact: true })).toBeVisible()
  })

  test('shows key rotation note', async ({ page }) => {
    await expect(page.locator('text=Key rotation is not yet implemented')).toBeVisible()
  })

  test('has copy buttons for URLs', async ({ page }) => {
    // The OIDC section has buttons for copying URLs
    await expect(page.locator('button:has-text("Copy")').first()).toBeVisible()
  })
})
