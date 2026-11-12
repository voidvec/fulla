import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin } from './helpers/mock-api'

// P1-3 (#280) claim-mapping admin console — the declared UI follow-up from
// #293, landed as audit wave B. The mock mirrors the server's red lines
// (reserved claim names / the openid trigger scope -> catalog 400s).
test.describe('Claim Mappings Page', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Claim Mappings")')
    await page.waitForURL('**/admin/claim-mappings')
  })

  test('displays the page title and seeded mappings', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Claim Mappings')
    await expect(page.locator('text=tenant_id')).toBeVisible()
    await expect(page.locator('text=custom_attributes.tenant')).toBeVisible()
    // .first(): the claim name cell also substring-matches the
    // user.display_name source cell in the other row.
    await expect(page.locator('text=display_name').first()).toBeVisible()
    await expect(page.locator('text=user.display_name')).toBeVisible()
  })

  test('create sends the exact contract body and refreshes the list', async ({ page }) => {
    const createRequest = page.waitForRequest(
      (req) => req.url().includes('/api/admin/claim-mappings') && req.method() === 'POST',
    )
    await page.click('button:has-text("+ Create Mapping")')
    await page.fill('input[placeholder*="tenant_id"]', 'region_code')
    await page.fill('input[placeholder*="profile"]', 'profile')
    await page.fill('input[placeholder*="custom_attributes"]', 'custom_attributes.region')
    await page.click('button:has-text("Create"):not(:has-text("Mapping"))')
    const req = await createRequest
    expect(req.method()).toBe('POST')
    expect(JSON.parse(req.postData() || '{}')).toMatchObject({
      claim_name: 'region_code',
      scope_name: 'profile',
      source_path: 'custom_attributes.region',
      include_in: 'both',
    })
    // The mock's in-memory row appears after the refresh.
    await expect(page.locator('text=region_code')).toBeVisible()
  })

  test('reserved claim name surfaces the localized catalog error', async ({ page }) => {
    await page.click('button:has-text("+ Create Mapping")')
    await page.fill('input[placeholder*="tenant_id"]', 'sub')
    await page.fill('input[placeholder*="profile"]', 'profile')
    await page.fill('input[placeholder*="custom_attributes"]', 'custom_attributes.region')
    await page.click('button:has-text("Create"):not(:has-text("Mapping"))')
    await expect(page.locator('p, div').filter({ hasText: 'reserved and cannot shadow' }).first()).toBeVisible()
  })

  test('enabled toggle sends the full-replace body with the flipped flag', async ({ page }) => {
    const putRequest = page.waitForRequest(
      (req) => req.url().match(/\/api\/admin\/claim-mappings\/\d+$/) !== null && req.method() === 'PUT',
    )
    // The seeded display_name row starts disabled (mock fixture).
    const row = page.locator('tr', { hasText: 'display_name' })
    await row.locator('input[type="checkbox"]').check()
    const req = await putRequest
    expect(JSON.parse(req.postData() || '{}')).toMatchObject({
      claim_name: 'display_name',
      scope_name: 'email',
      source_path: 'user.display_name',
      enabled: true,
    })
  })

  test('delete asks for confirmation and removes the row', async ({ page }) => {
    await page.locator('tr', { hasText: 'tenant_id' }).locator('button:has-text("Delete")').click()
    await expect(page.locator('text=Delete claim mapping "tenant_id"?')).toBeVisible()
    await page.getByTestId('confirm-dialog-confirm').click()
    await expect(page.locator('tr', { hasText: 'tenant_id' })).toHaveCount(0)
  })
})
