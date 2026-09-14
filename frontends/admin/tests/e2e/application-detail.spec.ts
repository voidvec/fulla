import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin, MOCK_CLIENT_DETAIL, MOCK_SCOPES } from './helpers/mock-api'

test.describe('Application Detail Page', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    // Navigate to applications then click into detail
    await page.click('nav a:has-text("Applications")')
    await page.waitForURL('**/admin/applications')
    // Click the client name link to go to detail
    await page.click('a:has-text("Vue Frontend")')
    await page.waitForURL('**/admin/applications/fulla-portal')
  })

  test('displays application title and back link', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Vue Frontend')
    await expect(page.locator('text=← Back to Applications')).toBeVisible()
  })

  test('shows all tabs', async ({ page }) => {
    await expect(page.locator('button:has-text("Info")')).toBeVisible()
    await expect(page.locator('button:has-text("Auth Config")')).toBeVisible()
    await expect(page.locator('button:has-text("Scopes")')).toBeVisible()
    await expect(page.locator('button:has-text("Credentials")')).toBeVisible()
  })

  test('Info tab shows client details', async ({ page }) => {
    // Info tab is active by default
    await expect(page.locator('text=Client ID')).toBeVisible()
    await expect(page.locator('[data-testid="client-id-chip"]')).toContainText('fulla-portal')
    await expect(page.locator('text=PUBLIC')).toBeVisible()
    await expect(page.locator('input[placeholder="Application name"]')).toHaveValue('Vue Frontend')
  })

  test('Info tab has copy button for client_id', async ({ page }) => {
    await expect(page.locator('button:has-text("Copy")')).toBeVisible()
  })

  test('Auth Config tab shows redirect URIs and grant types', async ({ page }) => {
    await page.click('button:has-text("Auth Config")')
    await expect(page.locator('textarea')).toBeVisible()
    await expect(page.locator('label:has-text("Authorization Code")')).toBeVisible()
    await expect(page.locator('label:has-text("Refresh Token")')).toBeVisible()
  })

  test('Scopes tab shows available scopes with checkboxes', async ({ page }) => {
    await page.click('button:has-text("Scopes")')
    // Should show all scopes from the system
    for (const scope of MOCK_SCOPES) {
      await expect(page.locator(`text=${scope.name}`).first()).toBeVisible()
    }
    // Should have Save Scopes button
    await expect(page.locator('button:has-text("Save Scopes")')).toBeVisible()
  })

  test('Scopes tab saves scope changes', async ({ page }) => {
    await page.click('button:has-text("Scopes")')
    await page.click('button:has-text("Save Scopes")')
    await expect(page.locator('text=Scopes updated successfully')).toBeVisible()
  })

  test('Credentials tab shows reset button for confidential client', async ({ page }) => {
    // Override mock to return CONFIDENTIAL client
    await page.route('**/api/admin/clients/*', async (route) => {
      const url = route.request().url()
      if (url.includes('/scopes') || url.includes('/reset-secret')) {
        await route.continue()
        return
      }
      if (route.request().method() === 'GET') {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ ...MOCK_CLIENT_DETAIL, client_type: 'CONFIDENTIAL', client_id: 'api-service' }),
        })
      } else {
        await route.continue()
      }
    })

    // Navigate away and back to reload with new mock
    await page.click('text=← Back to Applications')
    await page.click('a:has-text("API Service")')
    await page.waitForURL('**/admin/applications/api-service')

    await page.click('button:has-text("Credentials")')
    await expect(page.locator('button:has-text("Reset Client Secret")')).toBeVisible()
  })

  test('Save Changes button visible on Info tab', async ({ page }) => {
    await expect(page.locator('button:has-text("Save Changes")')).toBeVisible()
  })

  test('Save Changes works on Info tab', async ({ page }) => {
    await page.fill('input[placeholder="Application name"]', 'Updated Name')
    await page.click('button:has-text("Save Changes")')
    await expect(page.locator('text=Changes saved successfully')).toBeVisible()
  })

  test('back link navigates to applications list', async ({ page }) => {
    await page.click('text=← Back to Applications')
    await expect(page).toHaveURL(/\/admin\/applications$/)
  })

  test('save with no changes shows message', async ({ page }) => {
    await page.locator('button:has-text("Save Changes")').click()
    await page.waitForTimeout(500)
    await expect(page.locator('text=No changes')).toBeVisible({ timeout: 3000 })
  })
})
