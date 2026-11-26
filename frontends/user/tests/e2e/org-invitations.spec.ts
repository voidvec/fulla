import { test, expect, type Page } from '@playwright/test'
import { setupMocks, loginUser } from './helpers/mock-api'

// UF-02: an organization's pending invitations can be listed and revoked
// from the members panel. Wire contract mirrored here (OrgMemberService):
//   GET    .../{slug}/invitations          -> {invitations:[{id,email,role,expires_at}], total}
//   POST   .../{slug}/invitations          -> 201/200 {token, ...} (token shown once)
//   DELETE .../{slug}/invitations/{id}     -> 200 {message}, 404 when already gone
// Listing and revoking are for the org owner/admin only.

// expires_at is the server's own "YYYY-MM-DD HH:MM:SS" string with no zone;
// the page shows it verbatim (date and minutes) rather than reading it as a
// Date. Generated, not written as a literal, so the exact digits are known
// to the assertions.
const EXPIRES = '2026-11-29 10:30:00'

interface InviteState {
  invitations: { id: number; email: string; role: string; expires_at: string }[]
  calls: { method: string; path: string }[]
}

async function mockOrg(page: Page, role: 'owner' | 'admin' | 'member', state: InviteState) {
  await setupMocks(page)
  await page.route('**/api/me/organizations', (route) => {
    if (route.request().method() !== 'GET') return route.fallback()
    return route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        organizations: [{ id: 12, slug: 'qa-invite-org', name: 'QA Invite Org', role }],
        total: 1,
      }),
    })
  })
  await page.route('**/api/me/organizations/*/members', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        members: [{ user_id: 1, username: 'testuser', role: 'owner' }],
        total: 1,
      }),
    }),
  )
  await page.route('**/api/me/organizations/*/invitations', (route) => {
    const method = route.request().method()
    state.calls.push({ method, path: new URL(route.request().url()).pathname })
    if (method === 'GET') {
      return route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ invitations: state.invitations, total: state.invitations.length }),
      })
    }
    const body = route.request().postDataJSON()
    const created = { id: 100 + state.invitations.length + 1, email: body.email, role: body.role, expires_at: EXPIRES }
    state.invitations.push(created)
    return route.fulfill({
      status: 201,
      contentType: 'application/json',
      body: JSON.stringify({ ...created, token: `invite-once-${created.id}` }),
    })
  })
  await page.route('**/api/me/organizations/*/invitations/*', (route) => {
    const path = new URL(route.request().url()).pathname
    state.calls.push({ method: route.request().method(), path })
    const id = Number(path.slice(path.lastIndexOf('/') + 1))
    const index = state.invitations.findIndex((inv) => inv.id === id)
    if (route.request().method() !== 'DELETE' || index < 0) {
      return route.fulfill({
        status: 404,
        contentType: 'application/json',
        body: JSON.stringify({
          error: { code: 'VALIDATION_RESOURCE_NOT_FOUND', category: 'VALIDATION', message: 'invitation not found' },
        }),
      })
    }
    state.invitations.splice(index, 1)
    return route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ message: 'Invitation revoked' }),
    })
  })
}

async function openMembersPanel(page: Page) {
  await loginUser(page)
  await page.goto('/organizations')
  await page.getByRole('button', { name: 'Members', exact: true }).click()
}

test.describe('Organization invitations (UF-02)', () => {
  test('the members panel lists pending invitations with role and expiry', async ({ page }) => {
    const state: InviteState = {
      invitations: [
        { id: 21, email: 'new-hire@example.com', role: 'member', expires_at: EXPIRES },
        { id: 22, email: 'co-admin@example.com', role: 'admin', expires_at: EXPIRES },
      ],
      calls: [],
    }
    await mockOrg(page, 'owner', state)
    await openMembersPanel(page)

    const list = page.getByTestId('pending-invitations')
    await expect(list).toBeVisible()
    const rows = list.getByTestId('pending-invitation')
    await expect(rows).toHaveCount(2)
    // Verbatim server timestamp, cut to date and minutes — not re-read as a
    // Date, which would depend on the browser's zone.
    await expect(rows.nth(0)).toContainText('new-hire@example.com')
    await expect(rows.nth(0)).toContainText('Expires 2026-11-29 10:30')
    await expect(rows.nth(1)).toContainText('co-admin@example.com')
    await expect(rows.nth(1)).toContainText('admin')
    expect(state.calls).toContainEqual({ method: 'GET', path: '/api/me/organizations/qa-invite-org/invitations' })
  })

  test('revoking asks for confirmation, then removes the invitation', async ({ page }) => {
    const state: InviteState = {
      invitations: [{ id: 21, email: 'wrong-address@example.com', role: 'member', expires_at: EXPIRES }],
      calls: [],
    }
    await mockOrg(page, 'owner', state)
    await openMembersPanel(page)

    await page.getByTestId('revoke-invitation').click()
    // Nothing is sent until the shared confirm dialog is accepted.
    expect(state.calls.filter((c) => c.method === 'DELETE')).toEqual([])
    await expect(page.getByTestId('confirm-dialog-confirm')).toBeVisible()
    await page.getByTestId('confirm-dialog-confirm').click()

    await expect(page.getByTestId('pending-invitations')).toHaveCount(0)
    await expect(page.getByText('Invitation for "wrong-address@example.com" revoked')).toBeVisible()
    expect(state.calls).toContainEqual({
      method: 'DELETE',
      path: '/api/me/organizations/qa-invite-org/invitations/21',
    })
  })

  test('cancelling the confirmation leaves the invitation in place', async ({ page }) => {
    const state: InviteState = {
      invitations: [{ id: 21, email: 'someone@example.com', role: 'member', expires_at: EXPIRES }],
      calls: [],
    }
    await mockOrg(page, 'admin', state)
    await openMembersPanel(page)

    await page.getByTestId('revoke-invitation').click()
    await page.getByTestId('confirm-dialog-cancel').click()

    await expect(page.getByTestId('pending-invitation')).toHaveCount(1)
    expect(state.calls.filter((c) => c.method === 'DELETE')).toEqual([])
  })

  test('a new invitation appears in the pending list, and its token is flagged as shown once', async ({ page }) => {
    const state: InviteState = { invitations: [], calls: [] }
    await mockOrg(page, 'owner', state)
    await openMembersPanel(page)
    await expect(page.getByTestId('pending-invitations')).toHaveCount(0)

    await page.getByPlaceholder('Email to invite').fill('fresh@example.com')
    await page.getByRole('button', { name: 'Invite', exact: true }).click()

    await expect(page.getByText('Invitation token — shown only this once. Share it with the invitee now.')).toBeVisible()
    await expect(page.getByTestId('pending-invitation')).toHaveCount(1)
    await expect(page.getByTestId('pending-invitation')).toContainText('fresh@example.com')
  })

  test('revoking an invitation that is already gone shows the not-found copy', async ({ page }) => {
    const state: InviteState = {
      invitations: [{ id: 21, email: 'gone@example.com', role: 'member', expires_at: EXPIRES }],
      calls: [],
    }
    await mockOrg(page, 'owner', state)
    await openMembersPanel(page)
    // The row is on screen first; then it disappears on the server, between
    // listing and revoking.
    await expect(page.getByTestId('pending-invitation')).toHaveCount(1)
    state.invitations.splice(0)

    await page.getByTestId('revoke-invitation').click()
    await page.getByTestId('confirm-dialog-confirm').click()

    await expect(page.getByText('Resource not found')).toBeVisible()
  })

  test('a plain member neither sees nor asks for the invitation list', async ({ page }) => {
    const state: InviteState = {
      invitations: [{ id: 21, email: 'someone@example.com', role: 'member', expires_at: EXPIRES }],
      calls: [],
    }
    await mockOrg(page, 'member', state)
    await openMembersPanel(page)

    // The members list itself is visible, so the panel did open.
    await expect(page.getByText('testuser')).toBeVisible()
    await expect(page.getByTestId('pending-invitations')).toHaveCount(0)
    await expect(page.getByRole('button', { name: 'Invite', exact: true })).toHaveCount(0)
    expect(state.calls).toEqual([])
  })
})
