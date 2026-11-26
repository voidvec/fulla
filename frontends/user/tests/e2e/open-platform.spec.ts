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
  // Trailing `**` (not `*`): a single star does not cross '/', so it would
  // only match the collection path and let the per-application requests
  // (PATCH /{id} here) fall through to the network.
  await page.route('**/api/me/applications**', async (route: any) => {
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
        allowed_grant_types: body?.allowed_grant_types || ['authorization_code'],
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
    // T1.3 (UF-04a): PATCH {id} — partial update semantics (absent keys
    // unchanged). The mock mutates the stored row so the follow-up GET
    // re-renders the edited values, mirroring the real controller.
    const patchMatch = path.match(/^\/api\/me\/applications\/([^/]+)$/)
    if (route.request().method() === 'PATCH' && patchMatch) {
      const app = MOCK_APPS.find((a) => a.client_id === decodeURIComponent(patchMatch[1]))
      if (!app) {
        await route.fulfill({
          status: 404,
          contentType: 'application/json',
          body: JSON.stringify({ error: { code: 'VALIDATION_RESOURCE_NOT_FOUND', category: 'VALIDATION', message: 'Application not found', numeric_code: 1004, request_id: 'req-e2e-patch-404' } }),
        })
        return
      }
      if (app.status === 'suspended') {
        await route.fulfill({
          status: 403,
          contentType: 'application/json',
          body: JSON.stringify({ error: { code: 'AUTHZ_ACCESS_DENIED', category: 'AUTHORIZATION', message: 'application is suspended; contact the administrator', numeric_code: 5001, request_id: 'req-e2e-patch-403' } }),
        })
        return
      }
      const body = route.request().postDataJSON()
      if (body?.name !== undefined) app.name = body.name
      if (body?.redirect_uris !== undefined) app.redirect_uris = body.redirect_uris
      if (body?.allowed_grant_types !== undefined) app.allowed_grant_types = body.allowed_grant_types
      if (body?.scopes !== undefined) app.scopes = body.scopes
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ message: 'updated' }),
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
    // Contract regression guard (pre-release review P0): the mutation must go
    // out as JSON — the axios instance defaults to form-encoded and the
    // backend handler hard-requires getJsonObject(). A mock route fulfills
    // either shape, so only this request-level assertion can catch a drift
    // back to form encoding.
    const createRequest = page.waitForRequest(
      (r) => r.url().endsWith('/api/me/applications') && r.method() === 'POST',
    )
    await page.click('button:has-text("Register application")')
    await page.fill('input[maxlength="100"]', 'QA App')
    await page.selectOption('select', 'CONFIDENTIAL')
    await page.getByPlaceholder('https://my-app.example/callback').fill('https://qa.example/callback')
    await page.click('button[type="submit"]')
    const request = await createRequest
    expect((request.headers()['content-type'] || '').toLowerCase()).toContain('application/json')
    const body = JSON.parse(request.postData() || '{}')
    expect(body.name).toBe('QA App')
    expect(body.client_type).toBe('CONFIDENTIAL')
    await expect(page.getByText('Secret for app_qamock123')).toBeVisible()
    await expect(page.getByText('once-shown-secret')).toBeVisible()
    await expect(page.getByText('QA App')).toBeVisible()
  })

  test('nav shows organizations entry', async ({ page }) => {
    await expect(page.locator('nav a:has-text("My Organizations")')).toBeVisible()
  })

  // T1.3 (UF-04a) — edit modal (PATCH, partial semantics). Each test resets
  // the module-level store first: MOCK_APPS persists across tests in this
  // file, and the edit tests need a deterministic single-card list.
  test('editing only the name sends a name-only PATCH (partial semantics)', async ({ page }) => {
    MOCK_APPS.splice(0)
    MOCK_APPS.push({
      client_id: 'app_edit1',
      name: 'Editable App',
      client_type: 'PUBLIC',
      status: 'active',
      redirect_uris: ['https://edit.example/callback'],
      allowed_grant_types: ['authorization_code'],
    })
    const patchRequest = page.waitForRequest(
      (r) => r.url().endsWith('/api/me/applications/app_edit1') && r.method() === 'PATCH',
    )
    await page.reload()
    await page.getByTestId('app-edit-button').click()
    await page.getByTestId('app-edit-name').fill('Renamed App')
    await page.getByTestId('app-edit-submit').click()
    const request = await patchRequest
    expect((request.headers()['content-type'] || '').toLowerCase()).toContain('application/json')
    const body = JSON.parse(request.postData() || '{}')
    // R5: absent keys stay unchanged — the body must carry ONLY the dirty field.
    expect(body).toEqual({ name: 'Renamed App' })
    await expect(page.getByText('Application "Renamed App" updated')).toBeVisible()
    await expect(page.getByText('Renamed App').first()).toBeVisible()
  })

  test('editing redirect URIs refreshes the list with the new value', async ({ page }) => {
    MOCK_APPS.splice(0)
    MOCK_APPS.push({
      client_id: 'app_edit2',
      name: 'Uri App',
      client_type: 'PUBLIC',
      status: 'active',
      redirect_uris: ['https://old.example/callback'],
      allowed_grant_types: ['authorization_code'],
    })
    await page.reload()
    await page.getByTestId('app-edit-button').click()
    await page.getByTestId('app-edit-redirect-uris').fill('https://new.example/callback')
    await page.getByTestId('app-edit-submit').click()
    await expect(page.getByText('https://new.example/callback').first()).toBeVisible()
  })

  test('rejects a non-https redirect URI client-side without sending PATCH', async ({ page }) => {
    MOCK_APPS.splice(0)
    MOCK_APPS.push({
      client_id: 'app_edit3',
      name: 'Bad Uri App',
      client_type: 'PUBLIC',
      status: 'active',
      redirect_uris: ['https://ok.example/callback'],
      allowed_grant_types: ['authorization_code'],
    })
    let patchSeen = false
    page.on('request', (r) => {
      if (r.method() === 'PATCH') patchSeen = true
    })
    await page.reload()
    await page.getByTestId('app-edit-button').click()
    await page.getByTestId('app-edit-redirect-uris').fill('http://plain.example/callback')
    await page.getByTestId('app-edit-submit').click()
    await expect(page.getByText('Invalid redirect URI "http://plain.example/callback"')).toBeVisible()
    expect(patchSeen).toBe(false)
  })

  test('suspended application freezes its edit entry', async ({ page }) => {
    MOCK_APPS.splice(0)
    MOCK_APPS.push({
      client_id: 'app_susp',
      name: 'Suspended App',
      client_type: 'PUBLIC',
      status: 'suspended',
      redirect_uris: [],
      allowed_grant_types: ['authorization_code'],
    })
    await page.reload()
    const button = page.getByTestId('app-edit-button')
    await expect(button).toBeDisabled()
    // Even a forced PATCH answers 403 frozen (mock mirrors the backend).
    await expect(page.getByText('Suspended App').first()).toBeVisible()
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
    // Contract regression guard (pre-release review P0): JSON body, never the
    // axios default form encoding (the handler hard-requires getJsonObject()).
    const createRequest = page.waitForRequest(
      (r) => r.url().endsWith('/api/me/organizations') && r.method() === 'POST',
    )
    await page.click('button:has-text("Create organization")')
    await page.locator('input[pattern]').fill('qa-org-e2e')
    await page.locator('input[maxlength="100"]').fill('QA Org E2E')
    await page.click('button[type="submit"]')
    const request = await createRequest
    expect((request.headers()['content-type'] || '').toLowerCase()).toContain('application/json')
    const body = JSON.parse(request.postData() || '{}')
    expect(body.slug).toBe('qa-org-e2e')
    expect(body.name).toBe('QA Org E2E')
    await expect(page.getByText('@qa-org-e2e')).toBeVisible()
    await expect(page.getByText('owner', { exact: true })).toBeVisible()
  })

  test('accept invitation section visible', async ({ page }) => {
    await expect(page.getByText('Accept an invitation')).toBeVisible()
  })
})

// v1.4.0 M2: the org-consents panel (list / revoke / grant-link) — fixtures
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

// v1.4.0 M2: the org-consents panel (list / revoke / grant-link).
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

  // UF-07: the requester withdraws their own pending request. A row is
  // recognised as the user's by requester_username against the profile
  // (mocked as 'testuser'); the default queue row belongs to 'member-lee'.
  test("another member's request offers no withdrawal", async ({ page }) => {
    // Wait for the profile round trip the recognition depends on, so the
    // count below is not taken before the button could have appeared.
    await page.reload()
    const profile = page.waitForResponse((r) => new URL(r.url()).pathname === '/api/me')
    await page.click('button:has-text("Org authorizations")')
    await profile

    const pending = page.getByTestId('org-pending-requests')
    await expect(pending.getByText('member-lee')).toBeVisible()
    await expect(pending.getByTestId('withdraw-consent-request')).toHaveCount(0)
    await expect(pending.getByTestId('approve-consent-request')).toBeVisible()
  })

  test('a manager withdraws their own request from the queue', async ({ page }) => {
    STATE.requests.push({
      id: 'cr_mine',
      client_id: 'app_orgmock1',
      requested_by: 7,
      requester_username: 'testuser',
      requested_at: '2026-09-26 11:00:00',
      status: 'pending',
    })
    await page.reload()
    await page.click('button:has-text("Org authorizations")')

    const pending = page.getByTestId('org-pending-requests')
    // Exactly one row carries a withdrawal: the user's own.
    await expect(pending.getByTestId('withdraw-consent-request')).toHaveCount(1)
    await expect(pending.getByTestId('approve-consent-request')).toHaveCount(2)

    await pending.getByTestId('withdraw-consent-request').click()
    await expect(page.getByTestId('confirm-dialog-confirm')).toBeVisible()
    expect(STATE.calls.filter((c) => c.method === 'DELETE')).toEqual([])
    await page.getByTestId('confirm-dialog-confirm').click()

    await expect(page.getByText('Authorization request withdrawn')).toBeVisible()
    expect(STATE.calls).toContainEqual({
      method: 'DELETE',
      path: '/api/me/organizations/qa-consent-org/consent-requests/cr_mine',
      body: null,
    })
    // The user's row is gone from the refetched queue; the other stays.
    await expect(pending.getByText('testuser')).toHaveCount(0)
    await expect(pending.getByText('member-lee')).toBeVisible()
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
    // verbatim in the page's success alert — also proving the POST
    // round-tripped — and again inside the dialog, which stays open.
    await expect(page.locator('.bg-success-50').first()).toContainText('awaiting a manager decision')
    await expect(page.getByTestId('request-org-auth-filed-message')).toHaveText('awaiting a manager decision')
    // The POST carried exactly {client_id}:
    expect(STATE.calls).toContainEqual({
      method: 'POST',
      path: '/api/me/organizations/qa-member-org/consent-requests',
      body: { client_id: 'app_member1' },
    })
    // UF-07: the dialog stays on its filed state, so the request can still
    // be taken back — a plain member is told the id only here.
    await expect(page.getByTestId('request-org-auth-filed')).toBeVisible()
    await expect(page.getByTestId('request-org-auth-withdraw')).toBeVisible()
  })

  test('a request just filed can be withdrawn on the spot', async ({ page }) => {
    const STATE: OrgConsentRequestState = { requests: [], calls: [] }
    await mockOrgConsentRequests(page, 'qa-member-org', STATE)

    await page.getByTestId('request-org-auth').click()
    await page.getByRole('dialog').locator('select').selectOption('qa-member-org')
    await page.getByTestId('request-org-auth-submit').click()
    await expect(page.getByTestId('request-org-auth-filed')).toBeVisible()

    await page.getByTestId('request-org-auth-withdraw').click()
    await expect(page.getByTestId('request-org-auth-withdrawn')).toHaveText('Request withdrawn.')
    // The withdrawal names the id the filing answered with.
    expect(STATE.calls).toContainEqual({
      method: 'DELETE',
      path: '/api/me/organizations/qa-member-org/consent-requests/cr_1',
      body: null,
    })
    expect(STATE.requests).toHaveLength(0)
    // Withdrawn: the button is gone, only finishing is left.
    await expect(page.getByTestId('request-org-auth-withdraw')).toHaveCount(0)
    await page.getByTestId('request-org-auth-done').click()
    await expect(page.getByRole('dialog')).toHaveCount(0)
  })

  test('closing the filed dialog keeps the request', async ({ page }) => {
    const STATE: OrgConsentRequestState = { requests: [], calls: [] }
    await mockOrgConsentRequests(page, 'qa-member-org', STATE)

    await page.getByTestId('request-org-auth').click()
    await page.getByRole('dialog').locator('select').selectOption('qa-member-org')
    await page.getByTestId('request-org-auth-submit').click()
    await expect(page.getByTestId('request-org-auth-filed')).toBeVisible()
    await page.getByTestId('request-org-auth-done').click()

    await expect(page.getByRole('dialog')).toHaveCount(0)
    expect(STATE.calls.filter((c) => c.method === 'DELETE')).toEqual([])
    expect(STATE.requests).toHaveLength(1)
  })

  test('no organizations shows the join hint instead of an empty select', async ({ page }) => {
    MOCK_ORGS.length = 0
    await page.getByTestId('request-org-auth').click()
    await expect(page.getByTestId('request-org-auth-no-orgs')).toBeVisible()
    await expect(page.locator('[role="dialog"] select')).toHaveCount(0)
  })
})
