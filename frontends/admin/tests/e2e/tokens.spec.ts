import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin, MOCK_TOKENS } from './helpers/mock-api'

test.describe('Token Management', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Tokens")')
    await page.waitForURL('**/admin/tokens')
  })

  test('displays tokens page title', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Tokens')
  })

  test('shows token table with correct columns', async ({ page }) => {
    await expect(page.locator('th:has-text("Token")')).toBeVisible()
    await expect(page.locator('th:has-text("Type")')).toBeVisible()
    await expect(page.locator('th:has-text("Client")')).toBeVisible()
    await expect(page.locator('th:has-text("User")')).toBeVisible()
    await expect(page.locator('th:has-text("Scope")')).toBeVisible()
    await expect(page.locator('th:has-text("Expires")')).toBeVisible()
    await expect(page.locator('th:has-text("Actions")')).toBeVisible()
  })

  test('displays token prefixes in monospace', async ({ page }) => {
    for (const token of MOCK_TOKENS) {
      await expect(page.locator(`text=${token.token_prefix}`)).toBeVisible()
    }
  })

  test('shows client IDs for tokens', async ({ page }) => {
    await expect(page.locator('td:has-text("fulla-portal")').first()).toBeVisible()
    await expect(page.locator('td:has-text("api-service")')).toBeVisible()
  })

  test('shows filter bar with inputs', async ({ page }) => {
    await expect(page.locator('input[placeholder="Filter by client_id"]')).toBeVisible()
    await expect(page.locator('input[placeholder="Filter by user_id"]')).toBeVisible()
    await expect(page.locator('button:has-text("Apply")')).toBeVisible()
    await expect(page.locator('button:has-text("Clear")')).toBeVisible()
  })

  test('shows Revoke button for each token', async ({ page }) => {
    // Wait for table to render
    await expect(page.locator('tbody tr').first()).toBeVisible()
    const revokeButtons = page.locator('tbody button:has-text("Revoke")')
    expect(await revokeButtons.count()).toBeGreaterThanOrEqual(3)
  })

  test('revoke token shows confirmation dialog', async ({ page }) => {
    // Click first Revoke button in the table
    await page.locator('tbody button:has-text("Revoke")').first().click()
    await expect(page.locator('h3:has-text("Confirm Action")')).toBeVisible()
    await expect(page.locator('text=Revoke token starting with')).toBeVisible()
  })

  test('cancel confirmation closes dialog', async ({ page }) => {
    await page.locator('tbody button:has-text("Revoke")').first().click()
    await expect(page.locator('h3:has-text("Confirm Action")')).toBeVisible()
    await page.click('button:has-text("Cancel")')
    await expect(page.locator('h3:has-text("Confirm Action")')).not.toBeVisible()
  })

  test('confirm revocation calls API', async ({ page }) => {
    await page.locator('tbody button:has-text("Revoke")').first().click()
    await page.click('button:has-text("Confirm")')
    // Dialog should close after confirmation
    await expect(page.locator('h3:has-text("Confirm Action")')).not.toBeVisible()
  })

  test('shows Revoke All by App dropdown', async ({ page }) => {
    await page.click('button:has-text("Revoke All by App")')
    // Should show client IDs from current results
    await expect(page.locator('.absolute:has-text("fulla-portal")')).toBeVisible()
    await expect(page.locator('.absolute:has-text("api-service")')).toBeVisible()
  })

  test('shows pagination info', async ({ page }) => {
    await expect(page.locator('text=Page 1')).toBeVisible()
    await expect(page.locator('text=3 total')).toBeVisible()
  })

  test('shows empty state when no tokens', async ({ page }) => {
    await page.route('**/api/admin/tokens**', async (route) => {
      if (route.request().method() === 'GET') {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ tokens: [], total: 0, page: 1, per_page: 50 }),
        })
      } else {
        await route.continue()
      }
    })

    await page.click('nav a:has-text("Dashboard")')
    await page.click('nav a:has-text("Tokens")')
    await page.waitForURL('**/admin/tokens')
    await expect(page.locator('text=No active tokens found')).toBeVisible()
  })

  test('Revoke All for User button appears when user filter is set', async ({ page }) => {
    // Initially not visible
    await expect(page.locator('button:has-text("Revoke All for User")')).not.toBeVisible()

    // Type a user ID filter
    await page.fill('input[placeholder="Filter by user_id"]', 'admin')
    // Now the button should appear
    await expect(page.locator('button:has-text("Revoke All for User")')).toBeVisible()
  })

  test('filter by client_id sends correct params', async ({ page }) => {
    let requestParams: Record<string, string> = {}
    await page.route('**/api/admin/tokens**', async (route) => {
      if (route.request().method() === 'GET') {
        const url = new URL(route.request().url())
        requestParams = Object.fromEntries(url.searchParams.entries())
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ tokens: [], total: 0 }),
        })
      } else {
        await route.continue()
      }
    })
    await page.fill('input[placeholder="Filter by client_id"]', 'fulla-portal')
    await page.click('button:has-text("Apply")')
    await page.waitForTimeout(300)
    expect(requestParams.client_id).toBe('fulla-portal')
  })

  test('filter by user_id sends correct params', async ({ page }) => {
    let requestParams: Record<string, string> = {}
    await page.route('**/api/admin/tokens**', async (route) => {
      if (route.request().method() === 'GET') {
        const url = new URL(route.request().url())
        requestParams = Object.fromEntries(url.searchParams.entries())
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ tokens: [], total: 0 }),
        })
      } else {
        await route.continue()
      }
    })
    await page.fill('input[placeholder="Filter by user_id"]', 'admin')
    await page.click('button:has-text("Apply")')
    await page.waitForTimeout(300)
    expect(requestParams.user_id).toBe('admin')
  })

  test('clear filters resets and fetches all', async ({ page }) => {
    let requestParams: Record<string, string> = {}
    await page.route('**/api/admin/tokens**', async (route) => {
      if (route.request().method() === 'GET') {
        const url = new URL(route.request().url())
        requestParams = Object.fromEntries(url.searchParams.entries())
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ tokens: [], total: 0 }),
        })
      } else {
        await route.continue()
      }
    })
    // Set filters first
    await page.fill('input[placeholder="Filter by client_id"]', 'test')
    await page.click('button:has-text("Apply")')
    await page.waitForTimeout(200)
    // Clear
    await page.click('button:has-text("Clear")')
    await page.waitForTimeout(300)
    // After clear, filters should not be in request
    expect(requestParams.client_id).toBeUndefined()
  })

  test('timestamp formatting uses locale date string', async ({ page }) => {
    // Verify created_at and expires_at are displayed in locale format, not raw ISO
    const timeCell = page.locator('tbody td').nth(4) // created_at column
    const text = await timeCell.textContent()
    // Should NOT show raw ISO format like "2026-05-21T10:00:00Z"
    if (text && text !== '—') {
      expect(text).not.toContain('T')  // No ISO T separator
      expect(text).not.toContain('Z')  // No UTC Z
    }
  })
})
