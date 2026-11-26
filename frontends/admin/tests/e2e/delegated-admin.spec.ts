import { test, expect, type Page } from '@playwright/test'
import { setupAuthenticatedMocks, ADMIN_USER } from './helpers/mock-api'

// AF-01: permission awareness for delegated (non-admin) accounts.
//
// These specs mock the shape a delegated session has once the backend issued
// it resource scopes: userinfo without the admin role, the users list
// readable, everything else on the admin surface refused with 403. They pin
// the console's OWN behaviour on top of that shape (menu convergence,
// landing page, write gating, refusal when nothing is readable). Whether a
// real deployment issues those scopes is a server-side matter — see
// CONSOLE_LOGIN_SCOPE in src/stores/auth.ts.

const HINT = 'Read-only access: your role can view users but not change them'

const ADMIN_SURFACE_LISTS = [
  '**/api/admin/dashboard/stats*',
  '**/api/admin/clients*',
  '**/api/admin/organizations*',
  '**/api/admin/roles*',
  '**/api/admin/scopes*',
  '**/api/admin/claim-mappings*',
  '**/api/admin/upstream-providers*',
  '**/api/admin/tokens*',
  '**/api/admin/logs*',
  '**/api/admin/email-templates*',
  '**/api/admin/oidc/keys*',
]

function forbiddenBody(message: string) {
  return JSON.stringify({
    error: { code: 'AUTHZ_INSUFFICIENT_PERMISSIONS', category: 'AUTHORIZATION', message },
  })
}

/**
 * Turn the default admin mocks into a helpdesk delegate: no admin role, only
 * the users domain readable. `writable` decides whether the users write
 * probe (POST /api/admin/users with an empty object) reaches the handler
 * (400 = may write) or is refused by the scope gate (403 = read-only).
 * `readable=false` models an account whose token carries no admin-class
 * scope at all.
 */
async function mockDelegate(page: Page, opts: { writable: boolean; readable?: boolean }) {
  await setupAuthenticatedMocks(page)
  // Later registrations win (LIFO), so these override the admin defaults.
  await page.route('**/oauth2/userinfo', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ ...ADMIN_USER, name: 'helpdesk', roles: ['user', 'helpdesk'] }),
    }),
  )
  for (const pattern of ADMIN_SURFACE_LISTS) {
    await page.route(pattern, (route) =>
      route.fulfill({ status: 403, contentType: 'application/json', body: forbiddenBody('Insufficient permissions') }),
    )
  }
  if (opts.readable === false) {
    await page.route('**/api/admin/users?*', (route) =>
      route.fulfill({ status: 403, contentType: 'application/json', body: forbiddenBody('Insufficient scope') }),
    )
  }
  if (!opts.writable) {
    // The write probe posts to the bare path; the list GET carries a query
    // string, so this glob only intercepts the probe.
    await page.route('**/api/admin/users', (route) => {
      if (route.request().method() !== 'POST') return route.continue()
      return route.fulfill({
        status: 403,
        contentType: 'application/json',
        body: forbiddenBody('Insufficient scope'),
      })
    })
  }
}

async function signIn(page: Page) {
  await page.goto('/admin/login')
  await page.fill('input[type="text"]', 'helpdesk')
  await page.fill('input[type="password"]', 'helpdesk')
  await page.click('button[type="submit"]')
}

const sidebarLinks = (page: Page) => page.locator('aside nav a')

test.describe('Delegated administration (AF-01)', () => {
  test.beforeEach(async ({ page }) => {
    await page.route('**/oauth2/logout', (route) => route.fulfill({ status: 200, body: '{}' }))
  })

  test('the console asks for the resource scopes next to admin at login', async ({ page }) => {
    await setupAuthenticatedMocks(page)
    const loginRequest = page.waitForRequest(
      (r) => r.url().endsWith('/oauth2/login') && r.method() === 'POST',
    )
    await page.goto('/admin/login')
    await page.fill('input[type="text"]', 'admin')
    await page.fill('input[type="password"]', 'admin')
    await page.click('button[type="submit"]')

    const body = new URLSearchParams((await loginRequest).postData() || '')
    const scopes = (body.get('scope') || '').split(/\s+/).filter(Boolean)
    // The V045 permission catalog: a delegated account is only issued the
    // resource scopes it asks for, so the console has to name all of them.
    expect(scopes).toEqual([
      'openid', 'profile', 'admin',
      'users:read', 'users:write',
      'clients:read', 'clients:write',
      'tokens:read', 'tokens:write',
      'roles:read', 'roles:write',
      'audit:read',
    ])
    // Admin regression (R1): the fast path keeps the whole menu.
    await page.waitForURL('**/admin/')
    await expect(sidebarLinks(page)).toHaveCount(13)
    await expect(page.locator('aside nav a:has-text("Dashboard")')).toBeVisible()
    await expect(page.locator('aside nav a:has-text("Devices")')).toBeVisible()
    // ... and the admin chrome is unchanged (logo and breadcrumb root on
    // the dashboard, the administrator label in the user menu).
    await expect(page.locator('aside').getByRole('link', { name: 'Fulla' })).toHaveAttribute('href', '/admin/')
    await page.locator('header').getByRole('button', { name: /admin/ }).click()
    await expect(page.locator('header')).toContainText('Administrator')
  })

  test('a read-only users delegate lands on Users with only that menu entry', async ({ page }) => {
    await mockDelegate(page, { writable: false })
    await signIn(page)

    await page.waitForURL('**/admin/users')
    const links = sidebarLinks(page)
    await expect(links).toHaveText(['Users'])
    // The Overview and Monitoring sections have nothing left to show.
    await expect(page.locator('aside nav p')).toHaveText(['Management'])
    await expect(page.locator('h2')).toContainText('Users')
    // Nothing offers a link into the dashboard the delegate cannot read: the
    // breadcrumb has no dashboard root and the logo leads to the landing page.
    const breadcrumb = page.getByRole('navigation', { name: 'Breadcrumb' })
    await expect(breadcrumb).toContainText('Users')
    await expect(breadcrumb.getByRole('link', { name: 'Dashboard' })).toHaveCount(0)
    await expect(page.locator('aside').getByRole('link', { name: 'Fulla' })).toHaveAttribute('href', '/admin/users')
  })

  test('a read-only delegate gets every users write action disabled with a reason', async ({ page }) => {
    await mockDelegate(page, { writable: false })
    await signIn(page)
    await page.waitForURL('**/admin/users')

    const create = page.getByTestId('users-create-button')
    await expect(create).toBeDisabled()
    await expect(create).toHaveAttribute('title', HINT)
    // Reading still works: the list is rendered.
    await expect(page.locator('tbody tr').first()).toBeVisible()
    const assign = page.getByTestId('users-assign-roles-button').first()
    await expect(assign).toBeDisabled()
    await expect(assign).toHaveAttribute('title', HINT)
    const remove = page.getByTestId('users-delete-button').first()
    await expect(remove).toBeDisabled()
    await expect(remove).toHaveAttribute('title', HINT)

    // The detail page is readable, its write actions are not.
    await page.locator('tbody tr').first().getByRole('link', { name: 'Details' }).click()
    await page.waitForURL(/\/admin\/users\/\d+$/)
    await expect(page.getByTestId('user-save-info-button')).toBeDisabled()
    const lock = page.locator('[data-testid="user-disable-button"], [data-testid="user-enable-button"]')
    await expect(lock).toBeDisabled()
    await page.getByRole('button', { name: 'Roles', exact: true }).click()
    await expect(page.getByTestId('user-save-roles-button')).toBeDisabled()
  })

  test('a delegate holding users:write keeps the write actions enabled', async ({ page }) => {
    await mockDelegate(page, { writable: true })
    await signIn(page)
    await page.waitForURL('**/admin/users')

    // Still a delegate: the menu stays converged to the one domain.
    await expect(sidebarLinks(page)).toHaveText(['Users'])
    await expect(page.getByTestId('users-create-button')).toBeEnabled()
    await expect(page.getByTestId('users-assign-roles-button').first()).toBeEnabled()
    await expect(page.getByTestId('users-delete-button').first()).toBeEnabled()
  })

  test('the write probe is an empty JSON object, and nothing is written', async ({ page }) => {
    await mockDelegate(page, { writable: true })
    const probe = page.waitForRequest(
      (r) => r.url().endsWith('/api/admin/users') && r.method() === 'POST',
    )
    await signIn(page)
    await page.waitForURL('**/admin/users')

    const request = await probe
    expect(request.postDataJSON()).toEqual({})
    expect((request.headers()['content-type'] || '')).toContain('application/json')
  })

  test('a restored delegated session keeps its converged menu', async ({ page }) => {
    await mockDelegate(page, { writable: false })
    await signIn(page)
    await page.waitForURL('**/admin/users')

    // A reload restores the session through the probes again — it must not
    // bounce to the login page nor fall back to the full admin menu.
    await page.reload()
    await page.waitForLoadState('networkidle')
    await expect(page).toHaveURL(/\/admin\/users$/)
    await expect(sidebarLinks(page)).toHaveText(['Users'])
    await expect(page.getByTestId('users-create-button')).toBeDisabled()
  })

  test('an account that can read no admin domain is refused the console', async ({ page }) => {
    await mockDelegate(page, { writable: false, readable: false })
    await signIn(page)

    await expect(page).toHaveURL(/\/admin\/login$/)
    await expect(page.getByText('Admin role required to access this console')).toBeVisible()
  })

  test('signing out drops the delegate access map', async ({ page }) => {
    await mockDelegate(page, { writable: false })
    await signIn(page)
    await page.waitForURL('**/admin/users')

    // Open the user menu, then sign out.
    await page.locator('header').getByRole('button', { name: /helpdesk/ }).click()
    // A delegate is not presented as an administrator.
    await expect(page.locator('header')).toContainText('user, helpdesk')
    await expect(page.locator('header')).not.toContainText('Administrator')
    await page.locator('header').getByRole('button', { name: 'Sign out' }).click()
    await page.waitForURL(/\/admin\/login$/)
    await expect(page.locator('aside')).toHaveCount(0)
  })
})
