// P1-5 #282 — demo e2e smoke: the FOURTH interop driver (interop.yml).
//
// Unlike the three json-mode RP drivers (openid-client / authlib /
// go-oidc), this one drives a REAL BROWSER through the demo app: landing
// page -> fulla login page (real credentials form) -> redirect back ->
// /profile (userinfo) -> protected API -> RP-initiated logout. It proves
// the SDK's handwritten flow works when the browser, cookies and redirects
// are all real.

import { expect, test } from '@playwright/test';

const DEMO = process.env.DEMO_BASE_URL ?? 'http://localhost:8080';
const IDP = process.env.FULLA_BASE_URL ?? 'http://localhost:5555';
const USER = process.env.FULLA_RP_USER ?? 'admin';
const PASSWORD = process.env.FULLA_RP_PASSWORD ?? 'admin';

test('browser: login -> protected page + API -> logout', async ({ page }) => {
  // 1. Landing -> /api/login -> fulla authorize -> login page
  //    (GET /login carries the oauth params as hidden form fields).
  await page.goto(DEMO);
  await page.click('#login');
  await page.waitForURL(new RegExp(`${IDP.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}/login`), { timeout: 15000 });
  await expect(page.locator('#username')).toBeVisible();

  // 2. First factor (dev seed credentials).
  await page.fill('#username', USER);
  await page.fill('#password', PASSWORD);
  await page.click('button[type="submit"]');

  // 3. Back on the demo callback -> /profile. There is no consent
  //    interstitial for the seeded fulla-portal scopes (the json-mode
  //    drivers observe the same: login POST returns the code directly).
  await page.waitForURL(`${DEMO}/profile`, { timeout: 15000 });
  await expect(page.getByTestId('signed-in')).toContainText('Signed in as');

  // 4. Protected API with the same Bearer (server-side, cookie-fed).
  const apiResponse = await page.request.get(`${DEMO}/api/me`);
  expect(apiResponse.status()).toBe(200);
  const body = (await apiResponse.json()) as Record<string, unknown>;
  expect(body['source']).toBe('protected-api');
  expect(typeof body['sub']).toBe('string');

  // 5. RP-initiated logout: server end-session, then back to the demo root
  //    (FULLA_POST_LOGOUT_URI) with the session cookie gone.
  await page.click('#logout');
  await page.waitForURL(new RegExp(`^${DEMO}/?($|\\?)`), { timeout: 15000 });
  const afterLogout = await page.request.get(`${DEMO}/api/me`);
  expect(afterLogout.status()).toBe(401);
});
