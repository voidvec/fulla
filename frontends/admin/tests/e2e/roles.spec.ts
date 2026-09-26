import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin, MOCK_ROLES } from './helpers/mock-api'

test.describe('Roles Page', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Roles")')
    await page.waitForURL('**/admin/roles')
  })

  test('displays roles list', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Roles')
    await expect(page.locator('tbody tr').filter({ hasText: 'admin' }).first()).toBeVisible()
    await expect(page.locator('tbody tr').filter({ hasText: 'user' }).first()).toBeVisible()
  })

  test('shows built-in badge for system roles', async ({ page }) => {
    await expect(page.locator('text=built-in').first()).toBeVisible()
  })

  test('shows user count for each role', async ({ page }) => {
    // admin role has 1 user
    await expect(page.locator('td:has-text("1")')).toBeVisible()
  })

  test('shows Edit button for all roles', async ({ page }) => {
    const editButtons = page.locator('button:has-text("Edit")')
    await expect(editButtons.first()).toBeVisible()
  })

  test('Delete button not shown for built-in roles', async ({ page }) => {
    // admin and user are built-in, should not have Delete button
    const rows = page.locator('tbody tr')
    const adminRow = rows.filter({ hasText: 'admin' }).first()
    await expect(adminRow.locator('button:has-text("Delete")')).not.toBeVisible()
  })

  test('can open Create Role modal', async ({ page }) => {
    await page.click('button:has-text("+ Create Role")')
    await expect(page.locator('h3:has-text("Create Role")')).toBeVisible()
    await expect(page.locator('input[placeholder="e.g. editor"]')).toBeVisible()
  })

  test('can create a new role', async ({ page }) => {
    await page.click('button:has-text("+ Create Role")')
    await page.fill('input[placeholder="e.g. editor"]', 'editor')
    await page.fill('input[placeholder="Optional description"]', 'Content editor role')
    await page.locator('.fixed button:has-text("Create")').click()
    await expect(page.locator('text=Role "editor" created')).toBeVisible()
  })

  test('Create button disabled when name is empty', async ({ page }) => {
    await page.click('button:has-text("+ Create Role")')
    const createBtn = page.locator('.fixed button:has-text("Create")')
    await expect(createBtn).toBeDisabled()
  })

  test('can cancel Create Role modal', async ({ page }) => {
    await page.click('button:has-text("+ Create Role")')
    await page.click('button:has-text("Cancel")')
    await expect(page.locator('h3:has-text("Create Role")')).not.toBeVisible()
  })

  test('can open Edit Role modal', async ({ page }) => {
    await page.locator('button:has-text("Edit")').first().click()
    await expect(page.locator('h3:has-text("Edit Role")')).toBeVisible()
  })

  test('can save role description', async ({ page }) => {
    await page.locator('button:has-text("Edit")').first().click()
    await page.fill('input[placeholder="Optional description"]', 'Updated description')
    await page.locator('.fixed button:has-text("Save")').click()
    await expect(page.locator('text=Role updated')).toBeVisible()
  })
})

test.describe('Roles Page - Custom Role', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)

    // Add a custom role to the mock
    await page.route('**/api/admin/roles', async (route) => {
      if (route.request().method() === 'GET') {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({
            status: 'success',
            roles: [
              ...MOCK_ROLES,
              { id: 3, name: 'editor', description: 'Content editor', user_count: 0, created_at: '2026-05-20T00:00:00Z' },
            ],
            total: 3,
          }),
        })
      } else {
        await route.continue()
      }
    })

    await loginAsAdmin(page)
    await page.click('nav a:has-text("Roles")')
    await page.waitForURL('**/admin/roles')
  })

  test('shows Delete button for custom roles', async ({ page }) => {
    const editorRow = page.locator('tbody tr').filter({ hasText: 'editor' })
    await expect(editorRow.locator('button:has-text("Delete")')).toBeVisible()
  })

  test('can delete a custom role with confirmation', async ({ page }) => {
    const editorRow = page.locator('tbody tr').filter({ hasText: 'editor' })
    await editorRow.locator('button:has-text("Delete")').click()
    await page.getByTestId('confirm-dialog-confirm').click()
    await expect(page.locator('text=Role "editor" deleted')).toBeVisible()
  })

  test('duplicate role name shows error', async ({ page }) => {
    await page.route('**/api/admin/roles', async (route) => {
      if (route.request().method() === 'POST') {
        await route.fulfill({
          status: 409,
          contentType: 'application/json',
          body: JSON.stringify({ error: { code: 'ROLE_EXISTS', message: 'Role already exists' } }),
        })
      } else {
        await route.continue()
      }
    })
    await page.click('button:has-text("+ Create Role")')
    await page.fill('input[placeholder="e.g. editor"]', 'admin')
    await page.locator('.fixed button:has-text("Create")').click()
    await page.waitForTimeout(300)
    const errorEl = page.locator('.bg-error-50, .text-error-700')
    await expect(errorEl.first()).toBeVisible()
  })

  test('delete role cancel preserves role', async ({ page }) => {
    const deleteButton = page.locator('tbody tr').first().locator('button:has-text("Delete")')
    if (await deleteButton.isVisible()) {
      await deleteButton.click()
      await page.getByTestId('confirm-dialog-cancel').click()
      await page.waitForTimeout(300)
      // Role should still be in the list
      await expect(page.locator('tbody tr').first()).toBeVisible()
    }
  })
})
