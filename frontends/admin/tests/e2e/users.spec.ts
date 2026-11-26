import { test, expect } from '@playwright/test'
import { setupAuthenticatedMocks, loginAsAdmin, MOCK_ROLES, MOCK_USERS } from './helpers/mock-api'

test.describe('User Management', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    await page.click('nav a:has-text("Users")')
    await page.waitForURL('**/admin/users')
  })

  test('displays users list with correct columns', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Users')
    await expect(page.locator('th:has-text("Username")')).toBeVisible()
    await expect(page.locator('th:has-text("Email")')).toBeVisible()
    await expect(page.locator('th:has-text("Verified")')).toBeVisible()
    await expect(page.locator('th:has-text("MFA")')).toBeVisible()
    await expect(page.locator('th:has-text("Actions")')).toBeVisible()
  })

  test('shows all users with correct data', async ({ page }) => {
    // Check usernames in the table - use exact text matching
    const tableBody = page.locator('tbody')
    await expect(tableBody.getByRole('cell', { name: 'admin', exact: true })).toBeVisible()
    await expect(tableBody.getByRole('cell', { name: 'testuser', exact: true })).toBeVisible()
  })

  test('displays email verification status badges', async ({ page }) => {
    await expect(page.locator('span:has-text("Verified")')).toBeVisible()
    await expect(page.locator('span:has-text("Pending")')).toBeVisible()
  })

  test('displays MFA status badges', async ({ page }) => {
    await expect(page.locator('span:has-text("Enabled")')).toBeVisible()
    await expect(page.locator('span:has-text("Off")')).toBeVisible()
  })

  test('opens role assignment modal', async ({ page }) => {
    await page.locator('button:has-text("Assign Roles")').first().click()
    await expect(page.locator('h3:has-text("Assign Roles")')).toBeVisible()
    // AF-02: roles are checkboxes taken from the catalog, one per role —
    // no free-text field to mistype a role name into.
    const boxes = page.getByTestId('assign-user-roles').locator('input[type="checkbox"]')
    await expect(boxes).toHaveCount(MOCK_ROLES.length)
    await expect(page.getByTestId('assign-user-roles').locator('input[type="text"]')).toHaveCount(0)
  })

  test('role modal shows selected username', async ({ page }) => {
    // Click first "Assign Roles" button (admin user)
    await page.locator('button:has-text("Assign Roles")').first().click()
    await expect(page.locator('strong:has-text("admin")')).toBeVisible()
  })

  test('assigns the roles that end up checked', async ({ page }) => {
    let roleRequestBody: any = null
    await page.route('**/api/admin/users/*/roles', async (route) => {
      roleRequestBody = JSON.parse(route.request().postData() || '{}')
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ message: 'Roles updated' }),
      })
    })

    await page.locator('button:has-text("Assign Roles")').first().click()
    // The row's current roles are checked to begin with.
    await expect(page.getByTestId('assign-user-role-admin')).toBeChecked()
    await expect(page.getByTestId('assign-user-role-user')).toBeChecked()
    await page.getByTestId('assign-user-role-user').uncheck()
    await page.click('button:has-text("Save Roles")')

    // Modal should close
    await expect(page.locator('h3:has-text("Assign Roles")')).not.toBeVisible()
    // Verify request body
    expect(roleRequestBody).toEqual({ roles: ['admin'] })
  })

  test('cancel closes role modal', async ({ page }) => {
    await page.locator('button:has-text("Assign Roles")').first().click()
    await expect(page.locator('h3:has-text("Assign Roles")')).toBeVisible()
    await page.click('button:has-text("Cancel")')
    await expect(page.locator('h3:has-text("Assign Roles")')).not.toBeVisible()
  })

  test('assigns several roles across the catalog', async ({ page }) => {
    let roleRequestBody: any = null
    await page.route('**/api/admin/users/*/roles', async (route) => {
      if (route.request().method() === 'PUT') {
        roleRequestBody = JSON.parse(route.request().postData() || '{}')
        await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ message: 'Roles updated' }) })
      } else {
        await route.continue()
      }
    })
    // The second row (testuser) holds the user role only.
    await page.locator('button:has-text("Assign Roles")').nth(1).click()
    await expect(page.getByTestId('assign-user-role-admin')).not.toBeChecked()
    await page.getByTestId('assign-user-role-admin').check()
    await page.click('button:has-text("Save Roles")')
    await expect(page.locator('h3:has-text("Assign Roles")')).not.toBeVisible()
    expect(roleRequestBody).toEqual({ roles: ['user', 'admin'] })
  })

  test('with no role checked the dialog cannot save', async ({ page }) => {
    let apiCalled = false
    await page.route('**/api/admin/users/*/roles', async (route) => {
      if (route.request().method() === 'PUT') {
        apiCalled = true
      }
      await route.continue()
    })
    await page.locator('button:has-text("Assign Roles")').first().click()
    await page.getByTestId('assign-user-role-admin').uncheck()
    await page.getByTestId('assign-user-role-user').uncheck()
    await expect(page.getByTestId('assign-user-roles-save')).toBeDisabled()
    expect(apiCalled).toBe(false)
  })

  // A-USR-RL-005: a role the server refuses must surface the backend error
  // through the Frontend_Error_Module. normalizeError maps the error CODE to
  // a localized message (Requirement 8.6) — the raw backend message is not
  // shown. VALIDATION_RESOURCE_NOT_FOUND is the catalog code for a missing
  // resource. The catalog can no longer be mistyped into a request, but it
  // can be stale — a role removed after the list was loaded.
  test('a refused role shows the catalog message (A-USR-RL-005)', async ({ page }) => {
    await page.route('**/api/admin/users/*/roles', async (route) => {
      if (route.request().method() === 'PUT') {
        await route.fulfill({
          status: 400,
          contentType: 'application/json',
          body: JSON.stringify({
            error: {
              code: 'VALIDATION_RESOURCE_NOT_FOUND',
              category: 'VALIDATION',
              message: 'Role does not exist',
            },
          }),
        })
      } else {
        await route.continue()
      }
    })
    await page.locator('button:has-text("Assign Roles")').nth(1).click()
    await page.getByTestId('assign-user-role-admin').check()
    await page.click('button:has-text("Save Roles")')
    // VALIDATION_RESOURCE_NOT_FOUND is mapped to "Resource not found"; the
    // raw backend message is intentionally NOT surfaced.
    const errorEl = page.locator('.bg-error-50')
    await expect(errorEl.first()).toBeVisible()
    await expect(errorEl.first()).toContainText('Resource not found')
    await expect(errorEl.first()).not.toContainText('Role does not exist')
  })

  test('API error displays error message', async ({ page }) => {
    // Trailing `*` so the override also matches the list call's query string
    // (?page=1&per_page=50) — without it the base mock answers 200 instead.
    await page.route('**/api/admin/users*', async (route) => {
      await route.fulfill({
        status: 500,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'INTERNAL_ERROR', message: 'Server error' } }),
      })
    })
    // Navigate away and back to trigger re-fetch
    await page.click('nav a:has-text("Dashboard")')
    await page.click('nav a:has-text("Users")')
    await page.waitForLoadState('networkidle')
    const errorEl = page.locator('.bg-error-50, .text-error-700')
    await expect(errorEl.first()).toBeVisible()
  })

  test('opens Create User modal', async ({ page }) => {
    await page.click('button:has-text("Create User")')
    await expect(page.locator('h3:has-text("Create User")')).toBeVisible()
    await expect(page.locator('input[placeholder="newuser"]')).toBeVisible()
  })

  test('creates a user successfully', async ({ page }) => {
    let createRequestBody: any = null
    await page.route('**/api/admin/users', async (route) => {
      if (route.request().method() === 'POST') {
        createRequestBody = JSON.parse(route.request().postData() || '{}')
        await route.fulfill({
          status: 201,
          contentType: 'application/json',
          body: JSON.stringify({ status: 'success', message: 'User created successfully', user: { id: 99, username: 'newuser_test', email: 'new@test.com' } }),
        })
      } else {
        await route.continue()
      }
    })
    await page.click('button:has-text("Create User")')
    await page.fill('input[placeholder="newuser"]', 'newuser_test')
    await page.fill('input[placeholder="••••••••"]', 'TestPass123!')
    // The modal-specific create button (the page-level button is behind the
    // modal overlay and would intercept pointer events).
    await page.locator('.fixed button:has-text("Create User")').click()
    await expect(page.locator('.bg-success-50')).toBeVisible({ timeout: 5000 })
    expect(createRequestBody.username).toBe('newuser_test')
  })

  test('search filters user list', async ({ page }) => {
    // Override the users route to return a filtered result for "testuser"
    await page.route('**/api/admin/users**', async (route) => {
      const url = new URL(route.request().url())
      const q = url.searchParams.get('q')
      if (q && q.length > 0) {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ status: 'success', page: 1, per_page: 50, total: 1, total_pages: 1, users: [MOCK_USERS[1]] }),
        })
      } else {
        await route.continue()
      }
    })
    await page.fill('input[placeholder="Search username or email..."]', 'testuser')
    await page.click('button:has-text("Search")')
    await page.waitForLoadState('networkidle')
    const tableBody = page.locator('tbody')
    await expect(tableBody.getByRole('cell', { name: 'testuser', exact: true })).toBeVisible()
    // admin user should NOT be present
    await expect(tableBody.getByRole('cell', { name: 'admin', exact: true })).toBeHidden()
  })

  test('duplicate username shows error', async ({ page }) => {
    await page.route('**/api/admin/users', async (route) => {
      if (route.request().method() === 'POST') {
        await route.fulfill({
          status: 400,
          contentType: 'application/json',
          body: JSON.stringify({ error: { code: 'VALIDATION_USERNAME_TAKEN', message: 'Username already exists' } }),
        })
      } else {
        await route.continue()
      }
    })
    await page.click('button:has-text("Create User")')
    await page.fill('input[placeholder="newuser"]', 'admin')
    await page.fill('input[placeholder="••••••••"]', 'TestPass123!')
    await page.locator('.fixed button:has-text("Create User")').click()
    const errorEl = page.locator('.bg-error-50')
    await expect(errorEl.first()).toBeVisible({ timeout: 5000 })
  })

  // AF-02 / AF-03: the role catalog drives the filter and the create form,
  // and a new account can be required to change its password at first
  // sign-in. The sign-in side of that flag (password_change_required) is
  // covered by the forced-password-change specs of both consoles.
  const AUDITOR = { id: 3, name: 'auditor', description: 'Read-only audit access', user_count: 0, created_at: '2026-05-01T00:00:00Z' }

  async function mockRoleCatalog(page: any, roles: object[]) {
    await page.route('**/api/admin/roles', (route: any) =>
      route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', roles, total: roles.length }),
      }),
    )
    // The catalog is read when the page mounts.
    await page.click('nav a:has-text("Dashboard")')
    await page.click('nav a:has-text("Users")')
  }

  async function openCreateForm(page: any, username: string) {
    await page.click('button:has-text("Create User")')
    await page.fill('input[placeholder="newuser"]', username)
    await page.fill('input[placeholder="••••••••"]', 'TestPass123!')
  }

  test('the role filter lists the catalog, a custom role included', async ({ page }) => {
    await mockRoleCatalog(page, [...MOCK_ROLES, AUDITOR])
    let listQuery = ''
    await page.route('**/api/admin/users?*', async (route) => {
      listQuery = new URL(route.request().url()).search
      await route.fallback()
    })

    const filter = page.getByTestId('users-role-filter')
    await expect(filter.locator('option')).toHaveText(['All roles', 'admin', 'user', 'auditor'])
    await filter.selectOption('auditor')
    await expect.poll(() => listQuery).toContain('role=auditor')
  })

  test('the create form offers the catalog roles, none checked', async ({ page }) => {
    await mockRoleCatalog(page, [...MOCK_ROLES, AUDITOR])
    await page.click('button:has-text("Create User")')

    const boxes = page.getByTestId('create-user-roles').locator('input[type="checkbox"]')
    await expect(boxes).toHaveCount(3)
    for (const box of await boxes.all()) await expect(box).not.toBeChecked()
    // The old free-text field is gone.
    await expect(page.locator('.fixed input[placeholder="user"]')).toHaveCount(0)
  })

  test('a new account is created with the checked roles and the first-sign-in change', async ({ page }) => {
    await mockRoleCatalog(page, [...MOCK_ROLES, AUDITOR])
    let createRequestBody: any = null
    await page.route('**/api/admin/users', async (route) => {
      if (route.request().method() !== 'POST') return route.fallback()
      createRequestBody = route.request().postDataJSON()
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'User created successfully', user: { id: 99, username: 'auditor_bob' } }),
      })
    })
    await openCreateForm(page, 'auditor_bob')
    await page.getByTestId('create-user-role-auditor').check()
    await page.getByTestId('create-user-must-change-password').check()
    await page.locator('.fixed button:has-text("Create User")').click()

    await expect(page.locator('.bg-success-50')).toBeVisible({ timeout: 5000 })
    expect(createRequestBody).toMatchObject({
      username: 'auditor_bob',
      roles: ['auditor'],
      must_change_password: true,
    })
  })

  test('an untouched create form sends neither roles nor the change flag', async ({ page }) => {
    let createRequestBody: any = null
    await page.route('**/api/admin/users', async (route) => {
      if (route.request().method() !== 'POST') return route.fallback()
      createRequestBody = route.request().postDataJSON()
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'User created successfully', user: { id: 98, username: 'plain_user' } }),
      })
    })
    await openCreateForm(page, 'plain_user')
    await page.locator('.fixed button:has-text("Create User")').click()

    await expect(page.locator('.bg-success-50')).toBeVisible({ timeout: 5000 })
    // The server applies its defaults: the user role, no forced change.
    expect(createRequestBody).not.toHaveProperty('roles')
    expect(createRequestBody).not.toHaveProperty('must_change_password')
  })

  test('without access to the role catalog the filter keeps the built-in roles', async ({ page }) => {
    // A delegated account may hold users:* without roles:read.
    await page.route('**/api/admin/roles', (route) =>
      route.fulfill({
        status: 403,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'AUTHZ_INSUFFICIENT_PERMISSIONS', category: 'AUTHORIZATION', message: 'Insufficient permissions' } }),
      }),
    )
    await page.click('nav a:has-text("Dashboard")')
    await page.click('nav a:has-text("Users")')

    await expect(page.getByTestId('users-role-filter').locator('option')).toHaveText(['All roles', 'admin', 'user'])
    // Not an error worth a banner.
    await expect(page.locator('.bg-error-50')).toHaveCount(0)
  })

  test('without access to the role catalog the dialog keeps the current roles and says so', async ({ page }) => {
    let roleRequestBody: any = null
    let putSeen = false
    await page.route('**/api/admin/users/*/roles', async (route) => {
      if (route.request().method() === 'PUT') {
        putSeen = true
        roleRequestBody = route.request().postDataJSON()
      }
      await route.continue()
    })
    await page.route('**/api/admin/roles', (route) =>
      route.fulfill({
        status: 403,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'AUTHZ_INSUFFICIENT_PERMISSIONS', category: 'AUTHORIZATION', message: 'Insufficient permissions' } }),
      }),
    )
    await page.click('nav a:has-text("Dashboard")')
    await page.click('nav a:has-text("Users")')
    await page.locator('button:has-text("Assign Roles")').first().click()

    await expect(page.getByTestId('assign-user-roles-unavailable')).toBeVisible()
    // Nothing can be typed, and the roles the user holds cannot be ticked
    // off behind the administrator's back.
    await expect(page.getByTestId('assign-user-role-admin')).toBeChecked()
    await expect(page.getByTestId('assign-user-role-admin')).toBeDisabled()
    await expect(page.getByTestId('assign-user-role-user')).toBeDisabled()
    expect(putSeen).toBe(false)
  })
})
