import { test, expect } from '@playwright/test'
import { setupMocks } from './helpers/mock-api'

for (const provider of ['github', 'google', 'wechat']) {
  test(`${provider}: unsolicited callback never exchanges or links and clears secrets`, async ({ page }) => {
    await setupMocks(page)
    let exchanged = false
    await page.route(`**/api/${provider}/login`, route => { exchanged = true; return route.abort() })
    await page.route(`**/api/me/social/links/${provider}`, route => { exchanged = true; return route.abort() })
    await page.goto(`/callback/${provider}?code=canary-secret&state=${'a'.repeat(64)}`)
    await expect(page.getByText('This sign-in request is invalid or expired. Please start again.')).toBeVisible()
    expect(exchanged).toBe(false)
    await expect(page).toHaveURL(`/callback/${provider}`)
  })
}

test('login callback sends code and state once without bearer refresh on failure', async ({ page }) => {
  await setupMocks(page)
  await page.goto('/login')
  const state = 'b'.repeat(64)
  await page.evaluate(s => sessionStorage.setItem('fulla_social_flow:' + s, JSON.stringify({
    purpose: 'login', provider: 'github', redirectUri: location.origin + '/callback/github', createdAt: Date.now(),
  })), state)
  let exchanges = 0
  let refreshes = 0
  await page.route('**/oauth2/token', route => { refreshes++; return route.abort() })
  await page.route('**/api/github/login', async route => {
    exchanges++
    expect(route.request().postDataJSON()).toEqual({ code: 'callback-code', state })
    expect(route.request().headers()['authorization']).toBeUndefined()
    await route.fulfill({ status: 401, contentType: 'application/json', body: JSON.stringify({ error: { code: 'AUTH_INVALID_CREDENTIALS', message: 'rejected' } }) })
  })
  await page.goto(`/callback/github?code=callback-code&state=${state}`)
  await expect(page).toHaveURL('/callback/github')
  await expect.poll(() => exchanges).toBe(1)
  expect(refreshes).toBe(0)
  await page.goto(`/callback/github?code=callback-code&state=${state}`)
  await expect(page.getByText('This sign-in request is invalid or expired. Please start again.')).toBeVisible()
  expect(exchanges).toBe(1)
})
