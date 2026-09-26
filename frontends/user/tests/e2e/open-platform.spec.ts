import { test, expect } from '@playwright/test'
import { setupMocks, loginUser, mockOrgConsentRequests, type OrgConsentRequestState } from './helpers/mock-api'

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

// v1.5.0 M2: the org-consents panel (list / revoke / grant-link) — fixtures
// live at module scope so the consent-request approval walkthrough below can
// reuse the same consents route (fullyParallel gives each test its own
// worker, i.e. its own module instance).
const MOCK_CONSENTS: any[] = [
  {
    client_id: 'app_orgmock1',
    scopes: [
      { scope: 'openid', granted_by: 1, granted_at: '2026-09-23 10:00:00' },
      { scope: 'profile', granted_by: 1, granted_at: '2026-09-23 10:00:00' },
    ],
  },
]

async function setupOrgConsentMocks(page: any) {
  await setupOpenPlatformMocks(page)
  MOCK_ORGS.length = 0
  MOCK_ORGS.push({ id: 7, slug: 'qa-consent-org', name: 'QA Consent Org', role: 'owner' })
  // NOTE: Playwright globs' single '*' does not cross '/', so the
  // pattern needs '**' to also capture the /{clientId} DELETE.
  await page.route('**/api/me/organizations/qa-consent-org/consents**', async (route: any) => {
    const method = route.request().method()
    if (method === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ slug: 'qa-consent-org', consents: [...MOCK_CONSENTS], total: MOCK_CONSENTS.length }),
      })
      return
    }
    if (method === 'DELETE') {
      MOCK_CONSENTS.length = 0
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ slug: 'qa-consent-org', client_id: 'app_orgmock1', revoked: 2 }),
      })
      return
    }
    await route.fulfill({ status: 404, body: '{}' })
  })
  // The panel also fetches the approval queue when it opens — default it to
  // empty so the pending section stays hidden unless a test seeds requests
  // (the walkthrough below registers a seeded route, which wins via LIFO).
  await mockOrgConsentRequests(page, 'qa-consent-org', { requests: [], calls: [] })
}

// v1.5.0 M2: the org-consents panel (list / revoke / grant-link).
test.describe('My Organizations — org authorizations', () => {
  test.beforeEach(async ({ page }) => {
    await setupOrgConsentMocks(page)
    await loginUser(page)
    await page.click('nav a:has-text("My Organizations")')
    await page.waitForURL('/organizations')
    await page.click('button:has-text("Org authorizations")')
    await expect(page.getByTestId('org-consents-panel')).toBeVisible()
  })

  test('lists grouped org consents (owner/admin only)', async ({ page }) => {
    await expect(page.getByText('app_orgmock1')).toBeVisible()
    await expect(page.getByText('openid, profile')).toBeVisible()
  })

  test('revoke removes the client group', async ({ page }) => {
    await page.getByTestId('revoke-org-consent').click()
    await page.getByTestId('confirm-dialog-confirm').click()
    await expect(page.getByText('No organization authorizations yet')).toBeVisible()
  })

  test('grant-link generation carries org_id, prompt=consent and a PKCE challenge', async ({ page }) => {
    await page.getByPlaceholder('Client ID of an org application').fill('app_orgmock1')
    await page
      .getByPlaceholder('Registered redirect URI')
      .fill('https://qa.example/callback')
    await page.click('button:has-text("Generate link")')
    const link = page.getByTestId('org-grant-link')
    await expect(link).toBeVisible()
    const text = (await link.textContent()) || ''
    expect(text).toContain('/oauth2/authorize?')
    expect(text).toContain('org_id=qa-consent-org')
    expect(text).toContain('prompt=consent')
    expect(text).toContain('client_id=app_orgmock1')
    expect(text).toContain(encodeURIComponent('https://qa.example/callback'))
    // The link must clear the consent POST's PKCE gate (43+ char S256
    // challenge) or the org grant can never complete.
    const marker = 'code_challenge='
    const start = text.indexOf(marker)
    expect(start).toBeGreaterThan(-1)
    const challenge = text.slice(start + marker.length).split('&')[0]
    expect(challenge.length).toBeGreaterThanOrEqual(43)
    expect(text).toContain('code_challenge_method=S256')
  })
})

// v1.6: manager-side approval queue — the org panel now lists filed
// consent-requests and approves them through the shared confirm dialog.
test.describe('My Organizations — consent-request approvals (manager)', () => {
  const STATE: OrgConsentRequestState = { requests: [], calls: [] }

  async function setupApprovalMocks(page: any) {
    await setupOrgConsentMocks(page)
    STATE.calls = []
    STATE.requests = [
      {
        id: 'cr_1',
        client_id: 'app_orgmock1',
        requested_by: 42,
        requester_username: 'member-lee',
        requested_at: '2026-09-26 10:00:00',
        status: 'pending',
      },
    ]
    // Registered AFTER setupOrgConsentMocks' default-empty queue → LIFO wins.
    await mockOrgConsentRequests(page, 'qa-consent-org', STATE)
  }

  test.beforeEach(async ({ page }) => {
    await setupApprovalMocks(page)
    await loginUser(page)
    await page.click('nav a:has-text("My Organizations")')
    await page.waitForURL('/organizations')
    await page.click('button:has-text("Org authorizations")')
    await expect(page.getByTestId('org-consents-panel')).toBeVisible()
  })

  test('manager approves a pending consent request', async ({ page }) => {
    const pending = page.getByTestId('org-pending-requests')
    await expect(pending).toBeVisible()
    await expect(pending.getByText('Pending approval requests')).toBeVisible()
    await expect(pending.getByText('member-lee')).toBeVisible()
    await expect(pending.getByText('app_orgmock1')).toBeVisible()
    await expect(pending.getByText('2026-09-26 10:00')).toBeVisible()

    await page.getByTestId('approve-consent-request').click()
    await page.getByTestId('confirm-dialog-confirm').click()

    // ...the page's standard success feedback appeared (which also proves
    // the POST round-tripped and was recorded)...
    await expect(page.getByText('Request approved')).toBeVisible()
    // ...the confirm pattern fired the approve POST with the expected shape
    // (approve takes no body — axios sends an empty one, parsed as null)...
    expect(STATE.calls).toContainEqual({
      method: 'POST',
      path: '/api/me/organizations/qa-consent-org/consent-requests/cr_1/approve',
      body: null,
    })
    // ...and the auto-resolved request vanished from the refetched queue.
    await expect(pending).toHaveCount(0)
  })
})

// v1.6: member-side filing — the applications list files a consent-request
// for an app against one of the user's organizations via the picker dialog.
test.describe('My Applications — request organization authorization (member)', () => {
  test.beforeEach(async ({ page }) => {
    await setupOpenPlatformMocks(page)
    MOCK_APPS.length = 0
    MOCK_APPS.push({
      client_id: 'app_member1',
      name: 'Member App',
      client_type: 'PUBLIC',
      status: 'active',
      redirect_uris: [],
      scopes: [],
    })
    MOCK_ORGS.length = 0
    MOCK_ORGS.push({ id: 9, slug: 'qa-member-org', name: 'QA Member Org', role: 'member' })
    await loginUser(page)
    await page.click('nav a:has-text("My Applications")')
    await page.waitForURL('/apps')
  })

  test('files an organization consent request for an app', async ({ page }) => {
    const STATE: OrgConsentRequestState = { requests: [], calls: [] }
    await mockOrgConsentRequests(page, 'qa-member-org', STATE)

    await page.getByTestId('request-org-auth').click()
    const dialog = page.getByRole('dialog')
    await expect(dialog).toBeVisible()
    await expect(dialog.getByText('Request organization authorization')).toBeVisible()

    await dialog.locator('select').selectOption('qa-member-org')
    await page.getByTestId('request-org-auth-submit').click()

    // The backend's message ("awaiting a manager decision") is surfaced
    // verbatim in the success alert — also proving the POST round-tripped.
    await expect(page.getByText('awaiting a manager decision')).toBeVisible()
    // The POST carried exactly {client_id}:
    expect(STATE.calls).toContainEqual({
      method: 'POST',
      path: '/api/me/organizations/qa-member-org/consent-requests',
      body: { client_id: 'app_member1' },
    })
    await expect(dialog).toHaveCount(0)
  })

  test('no organizations shows the join hint instead of an empty select', async ({ page }) => {
    MOCK_ORGS.length = 0
    await page.getByTestId('request-org-auth').click()
    await expect(page.getByTestId('request-org-auth-no-orgs')).toBeVisible()
    await expect(page.locator('[role="dialog"] select')).toHaveCount(0)
  })
})
