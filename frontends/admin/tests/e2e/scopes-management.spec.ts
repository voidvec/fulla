import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin, MOCK_SCOPES } from './helpers/mock-api'

test.describe('Scopes Management Page', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Scopes")')
    await page.waitForURL('**/admin/scopes')
  })

  test('displays scopes list', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Scopes')
    await expect(page.locator('tbody tr').filter({ hasText: 'openid' }).first()).toBeVisible()
    await expect(page.locator('tbody tr').filter({ hasText: 'profile' }).first()).toBeVisible()
    await expect(page.locator('tbody tr').filter({ hasText: 'admin' }).first()).toBeVisible()
  })

  test('shows built-in badge for system scopes', async ({ page }) => {
    await expect(page.locator('text=built-in').first()).toBeVisible()
  })

  test('shows flags for scopes', async ({ page }) => {
    // openid and profile are default
    await expect(page.locator('text=default').first()).toBeVisible()
    // admin requires admin role
    await expect(page.locator('text=admin only')).toBeVisible()
  })

  test('shows Edit button for all scopes', async ({ page }) => {
    await expect(page.locator('button:has-text("Edit")').first()).toBeVisible()
  })

  test('Delete button not shown for built-in scopes', async ({ page }) => {
    const openidRow = page.locator('tbody tr').filter({ hasText: 'openid' })
    await expect(openidRow.locator('button:has-text("Delete")')).not.toBeVisible()
  })

  test('can open Create Scope modal', async ({ page }) => {
    await page.click('button:has-text("+ Create Scope")')
    await expect(page.locator('h3:has-text("Create Scope")')).toBeVisible()
    await expect(page.locator('input[placeholder="e.g. reports:read"]')).toBeVisible()
  })

  test('can create a new scope', async ({ page }) => {
    await page.click('button:has-text("+ Create Scope")')
    await page.fill('input[placeholder="e.g. reports:read"]', 'reports:read')
    await page.fill('input[placeholder="What this scope grants access to"]', 'Read reports')
    await page.locator('.fixed button:has-text("Create")').click()
    await expect(page.locator('text=Scope "reports:read" created')).toBeVisible()
  })

  test('Create button disabled when name is empty', async ({ page }) => {
    await page.click('button:has-text("+ Create Scope")')
    const createBtn = page.locator('.fixed button:has-text("Create")')
    await expect(createBtn).toBeDisabled()
  })

  test('can cancel Create Scope modal', async ({ page }) => {
    await page.click('button:has-text("+ Create Scope")')
    await page.click('button:has-text("Cancel")')
    await expect(page.locator('h3:has-text("Create Scope")')).not.toBeVisible()
  })

  test('can open Edit Scope modal', async ({ page }) => {
    await page.locator('button:has-text("Edit")').first().click()
    await expect(page.locator('h3:has-text("Edit Scope")')).toBeVisible()
  })

  test('can save scope changes', async ({ page }) => {
    await page.locator('button:has-text("Edit")').first().click()
    await expect(page.locator('h3:has-text("Edit Scope")')).toBeVisible()
    // Description field in edit modal (first input after the heading)
    const descInput = page.locator('.fixed input').first()
    await descInput.fill('Updated description')
    await page.locator('.fixed button:has-text("Save")').click()
    await expect(page.locator('text=Scope updated')).toBeVisible()
  })

  test('edit modal shows checkboxes for flags', async ({ page }) => {
    await page.locator('button:has-text("Edit")').first().click()
    await expect(page.locator('text=Default scope')).toBeVisible()
    await expect(page.locator('text=Requires admin role')).toBeVisible()
  })
})

test.describe('Scopes Management - Custom Scope', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)

    // Add a custom scope to the mock
    await page.route('**/api/admin/scopes', async (route) => {
      if (route.request().method() === 'GET') {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({
            scopes: [
              ...MOCK_SCOPES,
              { id: 10, name: 'reports:read', description: 'Read reports', mapped_role: 'user', is_default: false, requires_admin_role: false },
            ],
          }),
        })
      } else {
        await route.continue()
      }
    })

    await loginAsAdmin(page)
    await page.click('nav a:has-text("Scopes")')
    await page.waitForURL('**/admin/scopes')
  })

  test('shows Delete button for custom scopes', async ({ page }) => {
    const customRow = page.locator('tbody tr').filter({ hasText: 'reports:read' })
    await expect(customRow.locator('button:has-text("Delete")')).toBeVisible()
  })

  test('can delete a custom scope with confirmation', async ({ page }) => {
    const customRow = page.locator('tbody tr').filter({ hasText: 'reports:read' })
    await customRow.locator('button:has-text("Delete")').click()
    await page.getByTestId('confirm-dialog-confirm').click()
    await expect(page.locator('text=Scope "reports:read" deleted')).toBeVisible()
  })
})
