import { test, expect } from '@playwright/test'
import { setupMocks, loginUser } from './helpers/mock-api'

// v1.5.0 M3: ownership succession on the organizations page -- the
// owner's nominate/withdraw surface and the nominee's accept banner.
// Backend contract mirrored here:
//   GET  /api/me/organizations  (successor_nomination per org,
//                                pending_succession_nominations top-level)
//   POST /api/me/organizations/{slug}/successor-nomination {user_id}
//   DELETE /api/me/organizations/{slug}/successor-nomination
//   POST /api/me/organizations/{slug}/successor-nomination/accept

let nomination: any = null

async function setupSuccessionMocks(page: any, mode: 'owner' | 'nominee') {
  await setupMocks(page)
  await page.route('**/api/me/organizations', async (route: any) => {
    if (route.request().method() !== 'GET') {
      await route.fulfill({ status: 404, body: '{}' })
      return
    }
    const org = {
      id: 9,
      slug: 'qa-succ-org',
      name: 'QA Succession Org',
      role: mode === 'owner' ? 'owner' : 'member',
      successor_nomination: mode === 'owner' ? nomination : null,
    }
    const body =
      mode === 'owner'
        ? { organizations: [org], total: 1, pending_succession_nominations: [] }
        : {
            organizations: [org],
            total: 1,
            pending_succession_nominations: nomination
              ? [
                  {
                    organization_id: 9,
                    slug: 'qa-succ-org',
                    name: 'QA Succession Org',
                    created_at: '2026-09-23 10:00:00',
                  },
                ]
              : [],
          }
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(body),
    })
  })
  await page.route('**/api/me/organizations/qa-succ-org/successor-nomination**', async (route: any) => {
    const method = route.request().method()
    const path = new URL(route.request().url()).pathname
    if (method === 'POST' && path.endsWith('/accept')) {
      nomination = null
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ slug: 'qa-succ-org', owner_user_id: 42, message: 'transferred' }),
      })
      return
    }
    if (method === 'POST') {
      nomination = {
        nominee_user_id: route.request().postDataJSON()?.user_id ?? 0,
        nominated_by: 1,
        created_at: '2026-09-23 10:00:00',
      }
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ slug: 'qa-succ-org', nominee_user_id: 42, message: 'nominated' }),
      })
      return
    }
    if (method === 'DELETE') {
      nomination = null
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ slug: 'qa-succ-org', message: 'withdrawn' }),
      })
      return
    }
    await route.fulfill({ status: 404, body: '{}' })
  })
  await page.route('**/api/me/organizations/qa-succ-org/members*', async (route: any) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        members: [{ user_id: 1, username: 'owner', display_name: '', role: 'owner' }],
        total: 1,
      }),
    })
  })
}

test.describe('Ownership succession', () => {
  test.beforeEach(() => {
    nomination = null
  })

  test('owner nominates a successor by user id and sees the pending row', async ({ page }) => {
    await setupSuccessionMocks(page, 'owner')
    await loginUser(page)
    await page.click('nav a:has-text("My Organizations")')
    await page.waitForURL('/organizations')

    await page.click('button:has-text("Members")')
    const panel = page.getByTestId('succession-nominate')
    await expect(panel).toBeVisible()
    await expect(page.getByTestId('succession-banner')).toHaveCount(0)

    await page.getByPlaceholder('Nominate successor (user ID)').fill('42')
    await page.click('button:has-text("Nominate")')
    await expect(panel.getByText('Pending successor nomination: user 42')).toBeVisible()
  })

  test('nominee sees the accept banner and accepts', async ({ page }) => {
    nomination = { nominee_user_id: 42, nominated_by: 1, created_at: '2026-09-23 10:00:00' }
    await setupSuccessionMocks(page, 'nominee')
    await loginUser(page)
    await page.click('nav a:has-text("My Organizations")')
    await page.waitForURL('/organizations')

    const banner = page.getByTestId('succession-banner')
    await expect(banner).toBeVisible()
    await expect(page.getByText('You have been nominated as the successor owner of "QA Succession Org".')).toBeVisible()

    await page.click('button:has-text("Accept ownership")')
    await expect(banner).toHaveCount(0)
  })
})
