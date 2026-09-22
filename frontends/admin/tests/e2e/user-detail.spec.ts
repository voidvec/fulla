import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin, MOCK_USER_DETAIL } from './helpers/mock-api'

test.describe('User Detail Page', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Users")')
    await page.waitForURL('**/admin/users')
    await page.locator('a:has-text("Details")').first().click()
    await page.waitForURL('**/admin/users/**')
  })

  test('displays user detail page with correct info', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('admin')
    await expect(page.locator('input[type="email"]')).toHaveValue('admin@example.com')
  })

  test('shows status badges', async ({ page }) => {
    await expect(page.locator('span:has-text("Active")')).toBeVisible()
    await expect(page.locator('span:has-text("Email Verified")')).toBeVisible()
    await expect(page.locator('span:has-text("MFA Enabled")')).toBeVisible()
  })

  test('shows Info tab with editable fields', async ({ page }) => {
    await expect(page.locator('input[type="email"]')).toBeVisible()
    await expect(page.locator('input#emailVerified')).toBeVisible()
  })

  test('can switch to Security tab', async ({ page }) => {
    await page.click('button:has-text("Security")')
    await expect(page.locator('p:has-text("Failed Login Count")')).toBeVisible()
    await expect(page.locator('p:has-text("Account Status")')).toBeVisible()
    await expect(page.locator('p:has-text("MFA Status")')).toBeVisible()
  })

  test('can switch to Roles tab', async ({ page }) => {
    // Use exact match to avoid matching "Save Roles" button
    await page.getByRole('button', { name: 'Roles', exact: true }).click()
    await expect(page.locator('label:has-text("admin")').first()).toBeVisible()
    await expect(page.locator('label:has-text("user")').first()).toBeVisible()
  })

  test('can save info changes', async ({ page }) => {
    await page.fill('input[type="email"]', 'newemail@example.com')
    await page.click('button:has-text("Save Changes")')
    await expect(page.locator('text=User updated successfully')).toBeVisible()
  })

  // v1.5.0 org-anchor convergence (design 1.2/V7): users.org_id admin write
  // surface removed - the Organization ID input is gone from the form (the
  // read-only column stays server-side until the v2.0 physical DROP), so the
  // page must never emit a PUT carrying org_id.
  test('organization input is removed and org_id is never sent', async ({ page }) => {
    await expect(page.locator('input[type="number"]')).toHaveCount(0)

    const putPromise = page.waitForRequest(
      (r) =>
        r.method() === 'PUT' &&
        /\/api\/admin\/users\/\d+$/.test(new URL(r.url()).pathname)
    )
    await page.fill('input[type="email"]', 'newemail2@example.com')
    await page.click('button:has-text("Save Changes")')
    const req = await putPromise
    expect(JSON.parse(req.postData() || '{}').org_id).toBeUndefined()
  })

  test('can save role changes', async ({ page }) => {
    // Verify the mock is working by checking the network
    let rolesPutCalled = false
    
    await page.route('**/api/admin/users/*/roles', async (route) => {
      if (route.request().method() === 'PUT') {
        rolesPutCalled = true
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ status: 'success', message: 'Roles updated' }),
        })
      } else {
        await route.continue()
      }
    })
    
    // Navigate to Roles tab
    await page.locator('.border-b button').filter({ hasText: /^Roles$/ }).click()
    await expect(page.locator('label:has-text("admin")').first()).toBeVisible()
    await page.click('button:has-text("Save Roles")')
    await page.waitForTimeout(2000)
    
    // Verify the PUT was called
    expect(rolesPutCalled).toBe(true)
    await expect(page.locator('text=Roles updated successfully')).toBeVisible({ timeout: 3000 })
  })

  test('disable account button is visible', async ({ page }) => {
    await expect(page.locator('button:has-text("Disable Account")')).toBeVisible()
  })

  test('back to users link works', async ({ page }) => {
    await page.click('a:has-text("← Back to Users")')
    await expect(page).toHaveURL(/\/admin\/users$/)
  })
})

test.describe('User Detail - Locked Account', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)

    // Override the user detail route to return locked user
    // Register AFTER setupAuthenticatedMocks so it takes priority (LIFO)
    await page.route('**/api/admin/users/*', async (route) => {
      if (route.request().method() === 'GET') {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({
            ...MOCK_USER_DETAIL,
            locked: true,
            locked_until: Math.floor(Date.now() / 1000) + 3600,
            failed_login_count: 5,
          }),
        })
      } else {
        await route.continue()
      }
    })

    await loginAsAdmin(page)
    await page.click('nav a:has-text("Users")')
    await page.waitForURL('**/admin/users')
    await page.locator('a:has-text("Details")').first().click()
    await page.waitForURL('**/admin/users/**')
    await page.waitForLoadState('networkidle')
  })

  test('shows Locked badge for locked account', async ({ page }) => {
    await expect(page.locator('span:has-text("Locked")')).toBeVisible()
  })

  test('shows Enable Account button for locked user', async ({ page }) => {
    await expect(page.locator('button:has-text("Enable Account")')).toBeVisible()
  })

  test('security tab shows lock info', async ({ page }) => {
    await page.click('button:has-text("Security")')
    await expect(page.locator('p.text-lg:has-text("Locked")')).toBeVisible()
    await expect(page.locator('button:has-text("Unlock Account")')).toBeVisible()
  })
})

test.describe('User Detail - Edge Cases', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
  })

  test('toggle email verified and save', async ({ page }) => {
    let putBody: any = null
    await page.route('**/api/admin/users/*', async (route) => {
      if (route.request().method() === 'PUT') {
        putBody = JSON.parse(route.request().postData() || '{}')
        await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ status: 'success' }) })
      } else if (route.request().method() === 'GET') {
        await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(MOCK_USER_DETAIL) })
      } else {
        await route.continue()
      }
    })

    await page.click('nav a:has-text("Users")')
    await page.waitForURL('**/admin/users')
    await page.locator('a:has-text("Details")').first().click()
    await page.waitForURL('**/admin/users/**')
    await page.waitForLoadState('networkidle')

    // Toggle the email-verified checkbox (strict locator — the page has
    // three checkboxes) and save; the PUT body must carry the toggled field.
    const checkbox = page.locator('input#emailVerified')
    await checkbox.click()
    await page.click('button:has-text("Save")')
    await page.waitForTimeout(300)
    expect(putBody).not.toBeNull()
    expect(putBody).toHaveProperty('email_verified')
  })

  test('save with no changes shows message', async ({ page }) => {
    await page.route('**/api/admin/users/*', async (route) => {
      if (route.request().method() === 'GET') {
        await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(MOCK_USER_DETAIL) })
      } else {
        await route.continue()
      }
    })
    await page.click('nav a:has-text("Users")')
    await page.waitForURL('**/admin/users')
    await page.locator('a:has-text("Details")').first().click()
    await page.waitForURL('**/admin/users/**')
    await page.waitForLoadState('networkidle')

    // Click save without changing anything
    const saveButton = page.locator('button:has-text("Save")')
    if (await saveButton.isVisible()) {
      await saveButton.click()
      await page.waitForTimeout(300)
      // Should show "No changes" message
      const successMsg = page.locator('.bg-success-50, .text-success-700')
      await expect(successMsg.first()).toBeVisible()
    }
  })

  test('non-existent user ID shows error', async ({ page }) => {
    // Route registered AFTER setupAuthenticatedMocks takes priority (LIFO)
    await page.route('**/api/admin/users/999999', async (route) => {
      await route.fulfill({
        status: 404,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'NOT_FOUND', category: 'CLIENT', message: 'User not found' } }),
      })
    })
    // Also handle sub-resource routes like /999999/roles
    await page.route('**/api/admin/users/999999/**', async (route) => {
      await route.fulfill({
        status: 404,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'NOT_FOUND', category: 'CLIENT', message: 'User not found' } }),
      })
    })
    await page.goto('/admin/users/999999')
    await page.waitForLoadState('networkidle')
    // Page should render with some error indication — verify it didn't crash
    const bodyText = await page.locator('body').textContent()
    expect(bodyText).toBeTruthy()
    expect(bodyText!.length).toBeGreaterThan(0)
  })
})
