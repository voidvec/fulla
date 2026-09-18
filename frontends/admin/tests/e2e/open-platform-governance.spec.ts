import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin } from './helpers/mock-api'

// v1.4.0: open platform governance UI (owner column, self-service badge,
// suspend/resume) and the admin organizations page.

test.describe('Applications governance (open platform)', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Applications")')
    await page.waitForURL('**/admin/applications')
  })

  test('admin-managed clients show no owner', async ({ page }) => {
    await expect(page.getByText('Admin-managed').first()).toBeVisible()
  })

  test('self-registered app shows owner label and badge', async ({ page }) => {
    await expect(page.getByText('Ada Lovelace')).toBeVisible()
    await expect(page.getByText('SELF', { exact: true }).first()).toBeVisible()
  })

  test('suspend action fires the governance endpoint after confirm', async ({ page }) => {
    let suspendCalled = false
    page.on('request', (req) => {
      if (req.url().includes('/suspend') && req.method() === 'POST') {
        suspendCalled = true
      }
    })
    page.once('dialog', (d) => d.accept())
    await page.getByRole('button', { name: 'Suspend' }).first().click()
    await expect
      .poll(() => suspendCalled, { timeout: 5000 })
      .toBe(true)
  })
})

test.describe('Organizations (admin)', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Organizations")')
    await page.waitForURL('**/admin/organizations')
  })

  test('lists organizations', async ({ page }) => {
    await expect(page.getByText('ACME Inc.')).toBeVisible()
    await expect(page.getByText('@acme').or(page.getByText('acme', { exact: true })).first()).toBeVisible()
  })

  test('create organization form opens with required fields', async ({ page }) => {
    await page.getByRole('button', { name: /Create Organization/ }).click()
    await expect(page.locator('input[pattern]')).toBeVisible()
    await page.locator('input[pattern]').fill('new-org')
    await page.locator('input[maxlength="100"]').fill('New Org')
    await page.getByRole('button', { name: 'Create', exact: true }).click()
    // POST /api/admin/organizations is mocked 201 -> success toast.
    await expect(page.getByText('Organization created')).toBeVisible({ timeout: 5000 })
  })
})
