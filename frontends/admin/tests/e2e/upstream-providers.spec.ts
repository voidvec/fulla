import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin } from './helpers/mock-api'

// P2-1d: the upstream identity-provider registry console. Mock mirrors the
// server contract (write-only secret, reserved-key CREATE rejection).
test.describe('Upstream Providers Page', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Upstream Providers")')
    await page.waitForURL('**/admin/upstream-providers')
  })

  test('displays the title and seeded rows with type badges', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Upstream Identity Providers')
    await expect(page.locator('text=entra-prod')).toBeVisible()
    await expect(page.locator('text=Entra ID (Prod)')).toBeVisible()
    // Legacy social row carries the `social` badge; registry rows `oidc`.
    await expect(page.locator('tr:has-text("github") .font-mono.bg-neutral-200')).toBeVisible()
    await expect(page.locator('tr:has-text("entra-prod") .font-mono.bg-brand-100')).toBeVisible()
    // The secret never appears anywhere in the table.
    await expect(page.locator('tbody')).not.toContainText('client_secret')
  })

  test('create sends the exact contract body and refreshes the list', async ({ page }) => {
    const createRequest = page.waitForRequest(
      (req) => req.url().includes('/api/admin/upstream-providers') && req.method() === 'POST',
    )
    await page.click('button:has-text("+ Register Provider")')
    await page.fill('[data-testid="new-provider-key"]', 'okta-staging')
    await page.fill('[data-testid="new-provider-name"]', 'Okta (Staging)')
    await page.fill('[data-testid="new-provider-issuer"]', 'https://okta.example.test')
    await page.locator('form .grid input.font-mono').first().fill('cid-okta')
    await page.locator('form .grid input[type="password"]').fill('throwaway-e2e-value')
    await page.click('[data-testid="create-provider-submit"]')
    const req = await createRequest
    const body = JSON.parse(req.postData() || '{}')
    expect(body.provider_key).toBe('okta-staging')
    expect(body.display_name).toBe('Okta (Staging)')
    expect(body.issuer).toBe('https://okta.example.test')
    expect(body.client_id).toBe('cid-okta')
    expect(body.scopes).toBe('openid profile email')
    await expect(page.locator('text=okta-staging')).toBeVisible()
    await expect(page.locator('text=Provider okta-staging registered')).toBeVisible()
  })

  test('create with a reserved social key surfaces the catalog error', async ({ page }) => {
    await page.click('button:has-text("+ Register Provider")')
    await page.fill('[data-testid="new-provider-key"]', 'google')
    await page.fill('[data-testid="new-provider-name"]', 'Dup')
    await page.fill('[data-testid="new-provider-issuer"]', 'https://x.test')
    await page.locator('form .grid input.font-mono').first().fill('x')
    await page.locator('form .grid input[type="password"]').fill('y')
    await page.click('[data-testid="create-provider-submit"]')
    // The mock answers VALIDATION_FORMAT_ERROR; the adapter renders the
    // catalog text (exact wording asserted in error-handling.spec.ts).
    await expect(page.locator('.bg-error-50')).toBeVisible()
  })

  test('edit keeps the secret when left empty and sends the full replace', async ({ page }) => {
    const putRequest = page.waitForRequest(
      (req) => req.url().includes('/api/admin/upstream-providers/keycloak-dev') && req.method() === 'PUT',
    )
    await page.locator('tr:has-text("keycloak-dev") [data-testid="edit-provider"]').click()
    await page.fill('[data-testid="edit-provider-name"]', 'Keycloak (Dev, renamed)')
    // Secret left empty = keep current.
    await page.click('[data-testid="edit-provider-submit"]')
    const req = await putRequest
    const body = JSON.parse(req.postData() || '{}')
    expect(body.display_name).toBe('Keycloak (Dev, renamed)')
    expect(body.client_secret).toBe('')
    expect(body.enabled).toBe(false)  // row-level state rides along
    await expect(page.locator('text=Provider keycloak-dev updated')).toBeVisible()
  })

  test('row toggle PUTs the flipped enabled flag', async ({ page }) => {
    const putRequest = page.waitForRequest(
      (req) => req.url().includes('/api/admin/upstream-providers/entra-prod') && req.method() === 'PUT',
    )
    await page.locator('tr:has-text("entra-prod") button[aria-label*="entra-prod"]').click()
    const req = await putRequest
    const body = JSON.parse(req.postData() || '{}')
    expect(body.enabled).toBe(false)
    await expect(page.locator('text=Provider entra-prod disabled')).toBeVisible()
  })

  test('delete asks for confirmation and removes the row', async ({ page }) => {
    await page.locator('tr:has-text("entra-prod") [data-testid="delete-provider"]').click()
    await expect(page.locator('text=Delete upstream provider "entra-prod"?')).toBeVisible()
    // The row Delete button stays in the DOM -- target the dialog's own
    // confirm button (same pattern as claim-mappings.spec.ts).
    await page.getByTestId('confirm-dialog-confirm').click()
    await expect(page.locator('text=Provider entra-prod deleted')).toBeVisible()
    await expect(page.locator('tbody')).not.toContainText('entra-prod')
  })
})
