import { test, expect, type Page } from '@playwright/test'
import { setupMocks, loginUser } from './helpers/mock-api'

// T2.2 (UF-04b/c): who owns a self-registered application — chosen at
// create, moved later. Wire contract mirrored here (openplatform/
// ApplicationService):
//   GET  /api/me/organizations                      -> caller's memberships with roles
//   POST /api/me/applications                       -> accepts org_slug, allowed_grant_types
//   POST /api/me/applications/{id}/transfer         -> body {org_slug: string | null}
//                                                     (key required; null = personal)
// An application may only sit under the caller's own account or an
// organization where they are owner/admin; any other slug is refused.

const ORGS = [
  { id: 1, slug: 'qa-org', name: 'QA Org', role: 'owner' },
  { id: 2, slug: 'member-org', name: 'Member Org', role: 'member' },
  { id: 3, slug: 'admin-org', name: 'Admin Org', role: 'admin' },
]

const DEFAULT_MEMBERS: Record<string, { user_id: number; username: string; role: string }[]> = {
  'qa-org': [
    { user_id: 1, username: 'testuser', role: 'owner' },
    { user_id: 42, username: 'alice', role: 'admin' },
  ],
  'admin-org': [
    { user_id: 1, username: 'testuser', role: 'admin' },
  ],
}

// A placeholder for the mock's one-time secret; the same obviously fake value
// the open-platform spec uses.
const ONCE_SHOWN_SECRET = 'once-shown-secret'

interface AppState {
  apps: {
    client_id: string
    name: string
    client_type: string
    status: string
    org_id: number | null
    creator_user_id?: number
    allowed_grant_types: string[]
  }[]
  creates: Record<string, unknown>[]
  transfers: { path: string; body: unknown }[]
  /** When set, the transfer endpoint answers with this instead of moving. */
  refuseTransferWith: { status: number; body: string } | null
  membersBySlug?: Record<string, { user_id: number; username: string; role: string }[]>
  failMembersLookup?: boolean
}

async function mockApplications(page: Page, state: AppState) {
  await setupMocks(page)
  await page.route('**/api/me/organizations', (route) => {
    if (route.request().method() !== 'GET') return route.fallback()
    return route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ organizations: ORGS, total: ORGS.length }),
    })
  })
  await page.route('**/api/me/organizations/*/members*', (route) => {
    if (route.request().method() !== 'GET') return route.fallback()
    if (state.failMembersLookup) {
      return route.fulfill({ status: 500, contentType: 'application/json', body: '{}' })
    }
    const match = new URL(route.request().url()).pathname.match(/^\/api\/me\/organizations\/([^/]+)\/members$/)
    const slug = match ? decodeURIComponent(match[1]) : ''
    const members = state.membersBySlug?.[slug] ?? DEFAULT_MEMBERS[slug] ?? []
    return route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ members, total: members.length }),
    })
  })
  await page.route('**/api/me/applications', (route) => {
    if (route.request().method() !== 'POST') return route.fallback()
    const body = route.request().postDataJSON()
    state.creates.push(body)
    const org = ORGS.find((o) => o.slug === body.org_slug)
    state.apps.push({
      client_id: `app_created_${state.apps.length + 1}`,
      name: body.name,
      client_type: body.client_type,
      status: 'active',
      org_id: org ? org.id : null,
      creator_user_id: 1,
      allowed_grant_types: body.allowed_grant_types,
    })
    return route.fulfill({
      status: 201,
      contentType: 'application/json',
      body: JSON.stringify({
        ...state.apps[state.apps.length - 1],
        client_secret: body.client_type === 'CONFIDENTIAL' ? ONCE_SHOWN_SECRET : undefined,
        message: 'created',
      }),
    })
  })
  await page.route('**/api/me/applications**', (route) => {
    const url = new URL(route.request().url())
    if (route.request().method() === 'GET' && url.pathname === '/api/me/applications') {
      return route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ applications: state.apps, total: state.apps.length }),
      })
    }
    const transfer = url.pathname.match(/^\/api\/me\/applications\/([^/]+)\/transfer$/)
    if (route.request().method() === 'POST' && transfer) {
      const body = route.request().postDataJSON()
      state.transfers.push({ path: url.pathname, body })
      if (state.refuseTransferWith) {
        return route.fulfill({
          status: state.refuseTransferWith.status,
          contentType: 'application/json',
          body: state.refuseTransferWith.body,
        })
      }
      const app = state.apps.find((a) => a.client_id === decodeURIComponent(transfer[1]))
      if (!app) {
        return route.fulfill({ status: 404, contentType: 'application/json', body: '{}' })
      }
      // Backend ApplicationService::transfer only supports personal -> org and
      // org -> personal; org -> org and personal -> personal are rejected 400.
      if (app.org_id != null && body.org_slug != null) {
        return route.fulfill({
          status: 400,
          contentType: 'application/json',
          body: JSON.stringify({
            error: {
              code: 'VALIDATION_INVALID_INPUT',
              category: 'VALIDATION',
              message: 'transfer: target organization required (application already belongs to one)',
            },
          }),
        })
      }
      if (app.org_id == null && body.org_slug == null) {
        return route.fulfill({
          status: 400,
          contentType: 'application/json',
          body: JSON.stringify({
            error: {
              code: 'VALIDATION_INVALID_INPUT',
              category: 'VALIDATION',
              message: 'transfer: application is already personal',
            },
          }),
        })
      }
      // The server refuses a slug the caller does not manage, the same way
      // for an unknown one.
      const org = body.org_slug == null ? null : ORGS.find((o) => o.slug === body.org_slug)
      if (body.org_slug != null && (!org || org.role === 'member')) {
        return route.fulfill({ status: 404, contentType: 'application/json', body: '{}' })
      }
      if (org) {
        app.org_id = org.id
      } else {
        app.org_id = null
        // Returning to personal preserves creator_user_id; another admin loses access.
        if (app.creator_user_id != null && app.creator_user_id !== 1) {
          state.apps = state.apps.filter((a) => a.client_id !== app.client_id)
        }
      }
      return route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ message: 'Application transferred', org_id: app.org_id }),
      })
    }
    return route.fallback()
  })
}

function stateWith(apps: AppState['apps'] = []): AppState {
  return { apps, creates: [], transfers: [], refuseTransferWith: null }
}

const personal = (n: number) => ({
  client_id: `app_personal_${n}`,
  name: `Personal App ${n}`,
  client_type: 'PUBLIC',
  status: 'active',
  org_id: null,
  creator_user_id: 1,
  allowed_grant_types: ['authorization_code'],
})

async function openApps(page: Page) {
  await loginUser(page)
  await page.goto('/apps')
  await expect(page.locator('[data-testid="app-owner"]').first()).toBeVisible()
}

test.describe('Application owner at create (UF-04c)', () => {
  test('the owner list offers the account itself and only the organizations the caller manages', async ({ page }) => {
    await mockApplications(page, stateWith())
    await loginUser(page)
    await page.goto('/apps')
    await page.getByRole('button', { name: 'Register application' }).click()

    const options = page.getByTestId('app-create-owner').locator('option')
    await expect(options).toHaveText([
      'Personal (no organization)',
      'QA Org',
      'Admin Org',
    ])
  })

  test('creating under an organization sends org_slug and the grant types shown', async ({ page }) => {
    const state = stateWith()
    await mockApplications(page, state)
    await loginUser(page)
    await page.goto('/apps')
    await page.getByRole('button', { name: 'Register application' }).click()

    await page.locator('input[maxlength="100"]').fill('Org App')
    await page.getByTestId('app-create-owner').selectOption('qa-org')
    // authorization_code is the only type checked, so it cannot be cleared
    // until another one is picked.
    await page.getByTestId('app-create-grant-device_code').check()
    await page.getByTestId('app-create-grant-authorization_code').uncheck()
    await page.getByRole('button', { name: 'Register', exact: true }).click()

    await expect(page.getByText('Org App')).toBeVisible()
    expect(state.creates).toHaveLength(1)
    // What the form showed is exactly what was asked for.
    expect(state.creates[0]).toMatchObject({
      name: 'Org App',
      org_slug: 'qa-org',
      allowed_grant_types: ['device_code'],
    })
    expect(state.creates[0]).not.toHaveProperty('redirect_uris')
  })

  test('grant types default to authorization_code and the last one checked cannot be cleared', async ({ page }) => {
    const state = stateWith()
    await mockApplications(page, state)
    await loginUser(page)
    await page.goto('/apps')
    await page.getByRole('button', { name: 'Register application' }).click()

    const code = page.getByTestId('app-create-grant-authorization_code')
    await expect(code).toBeChecked()
    await expect(code).toBeDisabled()

    await page.locator('input[maxlength="100"]').fill('Default App')
    await page.getByRole('button', { name: 'Register', exact: true }).click()
    await expect(page.getByText('Default App')).toBeVisible()

    expect(state.creates[0]).toMatchObject({
      name: 'Default App',
      allowed_grant_types: ['authorization_code'],
    })
    // Personal by default: no owner is named.
    expect(state.creates[0]).not.toHaveProperty('org_slug')
  })

  test('client_credentials is offered for a CONFIDENTIAL client only', async ({ page }) => {
    await mockApplications(page, stateWith())
    await loginUser(page)
    await page.goto('/apps')
    await page.getByRole('button', { name: 'Register application' }).click()

    const cc = page.getByTestId('app-create-grant-client_credentials')
    await expect(cc).toBeDisabled()
    await page.selectOption('select', 'CONFIDENTIAL')
    await expect(cc).toBeEnabled()
    await cc.check()
    // Back to PUBLIC: the choice cannot stand, so it is dropped.
    await page.selectOption('select', 'PUBLIC')
    await expect(cc).toBeDisabled()
    await expect(cc).not.toBeChecked()
  })
})

test.describe('Application transfer (UF-04b)', () => {
  test('the cards say who manages each application', async ({ page }) => {
    await mockApplications(page, stateWith([
      personal(1),
      { client_id: 'app_org_1', name: 'Org App', client_type: 'PUBLIC', status: 'active', org_id: 1, allowed_grant_types: ['authorization_code'] },
    ]))
    await loginUser(page)
    await page.goto('/apps')

    await expect(page.getByTestId('app-owner')).toHaveText(['Managed by: you (personal)', 'Managed by: QA Org'])
  })

  test('a personal application moves to an organization the caller manages', async ({ page }) => {
    const state = stateWith([personal(1)])
    await mockApplications(page, state)
    await openApps(page)

    await page.getByTestId('app-transfer-button').click()
    const dialog = page.getByTestId('app-transfer-dialog')
    await expect(dialog).toContainText('Move this application to a different owner')
    // The target list leaves out the organization the caller only belongs to,
    // and starts on a disabled placeholder so choosing nothing cannot submit.
    await expect(dialog.locator('option')).toHaveText(['Select an organization…', 'QA Org', 'Admin Org'])
    await expect(dialog.locator('option').first()).toBeDisabled()
    await expect(page.getByTestId('app-transfer-submit')).toBeDisabled()

    await page.getByTestId('app-transfer-target').selectOption('admin-org')
    await page.getByTestId('app-transfer-submit').click()

    await expect(page.getByTestId('app-owner')).toHaveText('Managed by: Admin Org')
    await expect(page.getByText('Application "Personal App 1" is now managed by Admin Org')).toBeVisible()
    expect(state.transfers).toEqual([
      { path: '/api/me/applications/app_personal_1/transfer', body: { org_slug: 'admin-org' } },
    ])
    await expect(page.getByTestId('app-transfer-dialog')).toHaveCount(0)
  })

  test('org-owned app created by caller only offers returning to you (personal) and sends org_slug: null', async ({ page }) => {
    const state = stateWith([
      { client_id: 'app_org_1', name: 'Org App', client_type: 'PUBLIC', status: 'active', org_id: 1, creator_user_id: 1, allowed_grant_types: ['authorization_code'] },
    ])
    await mockApplications(page, state)
    await openApps(page)

    await page.getByTestId('app-transfer-button').click()
    const dialog = page.getByTestId('app-transfer-dialog')
    // Org -> org is unsupported by backend; only personal ownership is offered.
    await expect(dialog.locator('option')).toHaveText(['Personal — you (personal)'])
    await expect(page.getByTestId('app-transfer-no-targets')).toHaveCount(0)
    await expect(page.getByTestId('app-transfer-creator-warning')).toHaveCount(0)
    await expect(page.getByTestId('app-transfer-submit')).toBeEnabled()

    await page.getByTestId('app-transfer-submit').click()

    await expect(page.getByTestId('app-owner')).toHaveText('Managed by: you (personal)')
    await expect(page.getByText('Application "Org App" is now managed by you (personal)')).toBeVisible()
    expect(state.transfers).toHaveLength(1)
    expect(state.transfers[0].body).toEqual({ org_slug: null })
    expect(Object.hasOwn(state.transfers[0].body as object, 'org_slug')).toBe(true)
  })

  test('org-owned app created by another user warns with original creator name before confirmation and in success banner', async ({ page }) => {
    const state = stateWith([
      { client_id: 'app_org_other', name: 'Alice Org App', client_type: 'PUBLIC', status: 'active', org_id: 1, creator_user_id: 42, allowed_grant_types: ['authorization_code'] },
    ])
    await mockApplications(page, state)
    await openApps(page)

    await page.getByTestId('app-transfer-button').click()
    const dialog = page.getByTestId('app-transfer-dialog')
    await expect(dialog.locator('option')).toHaveText(['Original creator: alice (#42) (personal)'])
    const warning = page.getByTestId('app-transfer-creator-warning')
    await expect(warning).toBeVisible()
    await expect(warning).toContainText('alice (#42)')
    await expect(warning).toContainText('lose management access')
    await expect(warning).toContainText('disappear from your list')

    await page.getByTestId('app-transfer-submit').click()

    await expect(page.getByText('Application "Alice Org App" was returned to its original creator (alice (#42))')).toBeVisible()
    await expect(page.getByText('you (personal)')).toHaveCount(0)
    expect(state.transfers).toEqual([
      { path: '/api/me/applications/app_org_other/transfer', body: { org_slug: null } },
    ])
  })

  test('org-owned app falls back conservatively when creator lookup fails', async ({ page }) => {
    const state = stateWith([
      { client_id: 'app_org_unknown', name: 'Unknown Creator App', client_type: 'PUBLIC', status: 'active', org_id: 1, creator_user_id: 99, allowed_grant_types: ['authorization_code'] },
    ])
    state.failMembersLookup = true
    await mockApplications(page, state)
    await openApps(page)

    await page.getByTestId('app-transfer-button').click()
    const dialog = page.getByTestId('app-transfer-dialog')
    await expect(dialog.locator('option')).toHaveText(['Original creator #99 (personal)'])
    const warning = page.getByTestId('app-transfer-creator-warning')
    await expect(warning).toBeVisible()
    await expect(warning).toContainText('#99')
    await expect(warning).toContainText('lose management access')
    await expect(dialog).not.toContainText('you (personal)')

    await page.getByTestId('app-transfer-submit').click()

    await expect(page.getByText('Application "Unknown Creator App" was returned to its original creator (#99)')).toBeVisible()
    await expect(page.getByText('you (personal)')).toHaveCount(0)
  })

  test('cancelling the dialog sends nothing', async ({ page }) => {
    const state = stateWith([personal(1)])
    await mockApplications(page, state)
    await openApps(page)

    await page.getByTestId('app-transfer-button').click()
    await page.getByTestId('app-transfer-target').selectOption('qa-org')
    await page.getByTestId('app-transfer-dialog').getByRole('button', { name: 'Cancel' }).click()

    await expect(page.getByTestId('app-transfer-dialog')).toHaveCount(0)
    await expect(page.getByTestId('app-owner')).toHaveText('Managed by: you (personal)')
    expect(state.transfers).toEqual([])
  })

  test('a suspended application cannot be transferred', async ({ page }) => {
    await mockApplications(page, stateWith([
      { ...personal(1), status: 'suspended' },
    ]))
    await openApps(page)

    await expect(page.getByTestId('app-transfer-button')).toBeDisabled()
  })

  test('a refusal from the server is shown and the application stays put', async ({ page }) => {
    const state = stateWith([personal(1)])
    state.refuseTransferWith = {
      status: 403,
      body: JSON.stringify({ error: { code: 'AUTHZ_ACCESS_DENIED', category: 'AUTHORIZATION', message: 'frozen' } }),
    }
    await mockApplications(page, state)
    await openApps(page)

    await page.getByTestId('app-transfer-button').click()
    await page.getByTestId('app-transfer-target').selectOption('qa-org')
    await page.getByTestId('app-transfer-submit').click()

    await expect(page.getByText('Access denied')).toBeVisible()
    await expect(page.getByTestId('app-owner')).toHaveText('Managed by: you (personal)')
  })

  test('with no organization to offer, the dialog says so and cannot submit', async ({ page }) => {
    const state = stateWith([personal(1)])
    await mockApplications(page, state)
    // The caller belongs to organizations but manages none of them.
    await page.route('**/api/me/organizations', (route) =>
      route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ organizations: [{ ...ORGS[1] }], total: 1 }),
      }),
    )
    await openApps(page)

    await page.getByTestId('app-transfer-button').click()
    await expect(page.getByTestId('app-transfer-no-targets')).toBeVisible()
    await expect(page.getByTestId('app-transfer-submit')).toBeDisabled()
    expect(state.transfers).toEqual([])
  })
})

test.describe('Application edit modal grants', () => {
  test('disables client_credentials for PUBLIC clients and keeps at least one grant checked', async ({ page }) => {
    const state = stateWith([
      personal(1),
      {
        client_id: 'app_conf_1',
        name: 'Conf App',
        client_type: 'CONFIDENTIAL',
        status: 'active',
        org_id: null,
        creator_user_id: 1,
        allowed_grant_types: ['authorization_code'],
      },
    ])
    await mockApplications(page, state)
    await openApps(page)

    // PUBLIC client: client_credentials is disabled; single checked grant is disabled.
    await page.getByTestId('app-edit-button').first().click()
    await expect(page.getByTestId('app-edit-grant-client_credentials')).toBeDisabled()
    await expect(page.getByTestId('app-edit-grant-authorization_code')).toBeChecked()
    await expect(page.getByTestId('app-edit-grant-authorization_code')).toBeDisabled()
    await page.getByTestId('app-edit-form').getByRole('button', { name: 'Cancel' }).click()

    // CONFIDENTIAL client: client_credentials is enabled and unlocks unchecking authorization_code.
    await page.getByTestId('app-edit-button').nth(1).click()
    const cc = page.getByTestId('app-edit-grant-client_credentials')
    const code = page.getByTestId('app-edit-grant-authorization_code')
    await expect(cc).toBeEnabled()
    await expect(code).toBeDisabled()
    await cc.check()
    await expect(code).toBeEnabled()
  })
})
