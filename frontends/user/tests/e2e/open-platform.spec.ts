import { test, expect } from '@playwright/test'
import { setupMocks, loginUser } from './helpers/mock-api'

// v1.4.0: open platform (My Applications) + organizations self-service pages.
// The backend mocks here mirror the API contract shipped in this tranche:
//   GET/POST  /api/me/applications
//   POST      /api/me/applications/{id}/rotate-secret
//   DELETE    /api/me/applications/{id}
//   GET/POST  /api/me/organizations
//   POST      /api/me/org-invitations/accept

const MOCK_APPS: any[] = []
const MOCK_ORGS: any[] = []

async function setupOpenPlatformMocks(page: any) {
  await setupMocks(page)
  await page.route('**/api/me/applications*', async (route: any) => {
    const url = new URL(route.request().url())
    const path = url.pathname
    if (route.request().method() === 'GET' && path.endsWith('/api/me/applications')) {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ applications: MOCK_APPS, total: MOCK_APPS.length }),
      })
      return
    }
    if (route.request().method() === 'POST' && path === '/api/me/applications') {
      const body = route.request().postDataJSON()
      const created = {
        client_id: 'app_qamock123',
        name: body?.name || 'mock-app',
        client_type: body?.client_type || 'PUBLIC',
        status: 'active',
        redirect_uris: body?.redirect_uris || [],
        scopes: body?.scopes || [],
      }
      MOCK_APPS.push(created)
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({
          ...created,
          client_secret:
            created.client_type === 'CONFIDENTIAL' ? 'once-shown-secret' : undefined,
          message: 'created',
        }),
      })
      return
    }
    await route.fulfill({ status: 404, body: '{}' })
  })
  await page.route('**/api/me/organizations*', async (route: any) => {
    const url = new URL(route.request().url())
    const path = url.pathname
    if (route.request().method() === 'GET' && path === '/api/me/organizations') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ organizations: MOCK_ORGS, total: MOCK_ORGS.length }),
      })
      return
    }
    if (route.request().method() === 'POST' && path === '/api/me/organizations') {
      const body = route.request().postDataJSON()
      MOCK_ORGS.push({ id: 1, slug: body?.slug || 'qa-org', name: body?.name || 'QA Org', role: 'owner' })
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({ slug: body?.slug, role: 'owner', message: 'Organization created' }),
      })
      return
    }
    await route.fulfill({ status: 404, body: '{}' })
  })
}

test.describe('My Applications', () => {
  test.beforeEach(async ({ page }) => {
    await setupOpenPlatformMocks(page)
    await loginUser(page)
    await page.click('nav a:has-text("My Applications")')
    await page.waitForURL('/apps')
  })

  test('displays empty state', async ({ page }) => {
    await expect(page.getByText('No applications yet')).toBeVisible()
  })

  test('create form registers a CONFIDENTIAL app and shows the secret once', async ({ page }) => {
    await page.click('button:has-text("Register application")')
    await page.fill('input[maxlength="100"]', 'QA App')
    await page.selectOption('select', 'CONFIDENTIAL')
    await page.getByPlaceholder('https://my-app.example/callback').fill('https://qa.example/callback')
    await page.click('button[type="submit"]')
    await expect(page.getByText('Secret for app_qamock123')).toBeVisible()
    await expect(page.getByText('once-shown-secret')).toBeVisible()
    await expect(page.getByText('QA App')).toBeVisible()
  })

  test('nav shows organizations entry', async ({ page }) => {
    await expect(page.locator('nav a:has-text("My Organizations")')).toBeVisible()
  })
})

test.describe('My Organizations', () => {
  test.beforeEach(async ({ page }) => {
    await setupOpenPlatformMocks(page)
    await loginUser(page)
    await page.click('nav a:has-text("My Organizations")')
    await page.waitForURL('/organizations')
  })

  test('displays empty state', async ({ page }) => {
    await expect(page.getByText('No organizations')).toBeVisible()
  })

  test('create organization flow', async ({ page }) => {
    await page.click('button:has-text("Create organization")')
    await page.locator('input[pattern]').fill('qa-org-e2e')
    await page.locator('input[maxlength="100"]').fill('QA Org E2E')
    await page.click('button[type="submit"]')
    await expect(page.getByText('@qa-org-e2e')).toBeVisible()
    await expect(page.getByText('owner', { exact: true })).toBeVisible()
  })

  test('accept invitation section visible', async ({ page }) => {
    await expect(page.getByText('Accept an invitation')).toBeVisible()
  })
})
