import { test, expect } from '@playwright/test'
import { setupMocks, loginUser } from './helpers/mock-api'

// UF-01: passkey login entry (T1.1) + UF-03: credential delete (T1.2).
//
// R2 (remediation plan): headless WebAuthn is unstable — the browser API is
// stubbed before any page script runs and both server endpoints are mocked
// per the established pattern. The encode/decode conversions themselves are
// pinned by tests/unit/webauthnService.test.ts; these specs cover the flow.

/**
 * Stub navigator.credentials with a `get()` that answers a full assertion
 * (authenticatorData/clientDataJSON/signature/userHandle as ArrayBuffers —
 * the exact shapes the finish-payload conversion reads).
 *
 * The returned function is handed to page.addInitScript, which re-evaluates
 * its source in the page: it must therefore be self-contained. A reference
 * to anything from this scope (a parameter, an import) is a ReferenceError
 * there, the stub is never installed, and the real browser API is what
 * runs. The assertion without a userHandle is covered by the unit tests.
 */
function stubAssertion() {
  return () => {
    const bytes = (n: number) => new Uint8Array(n).buffer
    const response: Record<string, ArrayBuffer | undefined> = {
      authenticatorData: bytes(37),
      clientDataJSON: bytes(120),
      signature: bytes(70),
      userHandle: bytes(16),
    }
    Object.defineProperty(window, 'PublicKeyCredential', {
      value: window.PublicKeyCredential || class {},
      configurable: true,
    })
    Object.defineProperty(navigator, 'credentials', {
      value: {
        get: async () => ({
          id: 'stub-assertion-id',
          rawId: bytes(32),
          type: 'public-key',
          response,
        }),
      },
      configurable: true,
    })
  }
}

test.describe('Passkey login (UF-01)', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  test('logs in via passkey and reaches the portal with tokens', async ({ page }) => {
    await page.addInitScript(stubAssertion())
    // The post-finish token bridge: the SPA navigates to its own authorize
    // URL with a PKCE stash; the mock echoes the state back through
    // /callback so the CallbackPage exchange (state correlation) succeeds,
    // mirroring "logged-in + prior consent → code" on the real endpoint.
    await page.route('**/oauth2/authorize*', async (route) => {
      const url = new URL(route.request().url())
      const state = url.searchParams.get('state') || 'test'
      await route.fulfill({
        status: 302,
        headers: {
          Location: `http://localhost:5173/callback?code=passkey-code&state=${encodeURIComponent(state)}`,
        },
      })
    })

    const beginRequest = page.waitForRequest('**/oauth2/webauthn/authenticate/begin')
    const finishRequest = page.waitForRequest('**/oauth2/webauthn/authenticate/finish')
    await page.goto('/login')
    await page.getByTestId('login-passkey-button').click()

    // Anonymous + session-carried: begin must NOT carry a Bearer token.
    const begin = await beginRequest
    expect(begin.headers()['authorization']).toBeUndefined()

    const finish = await finishRequest
    const body = finish.postDataJSON()
    // WebAuthnAssertionCredential contract (openapi.yaml:6970): base64url
    // id/rawId, the assertion triple, optional userHandle.
    expect(body.id).toBeTruthy()
    expect(body.rawId).toBe(body.id)
    expect(body.response.authenticatorData).toBeTruthy()
    expect(body.response.clientDataJSON).toBeTruthy()
    expect(body.response.signature).toBeTruthy()
    // userHandle is a top-level field of the credential, not part of
    // response.
    expect(typeof body.userHandle).toBe('string')

    // Session established → the SPA's own authorize round-trip → /callback
    // redeems the code with the stashed verifier → portal home, signed in.
    // The access token lives in memory only (frontend contract); the
    // persisted refresh token is the observable proof that the exchange
    // stored a session.
    await page.waitForURL((url) => new URL(url).pathname === '/', { timeout: 10000 })
    expect(await page.evaluate(() => localStorage.getItem('refresh_token'))).toBeTruthy()
  })

  test('maps a rejected assertion to the generic credential copy (anti-enumeration)', async ({ page }) => {
    await page.addInitScript(stubAssertion())
    await page.route('**/oauth2/webauthn/authenticate/finish', async (route) => {
      await route.fulfill({
        status: 401,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: 'authenticateFinish: failed', numeric_code: 4001, request_id: 'req-e2e-passkey-reject' } }),
      })
    })

    await page.goto('/login')
    await page.getByTestId('login-passkey-button').click()
    // The catalog copy — NOT a passkey-specific reason (the backend answers
    // every failure identically on purpose).
    await expect(page.getByTestId('login-passkey-error')).toHaveText('Incorrect username or password')
    // Still on the login page.
    await expect(page).toHaveURL(/\/login/)
  })

  test('renders local copy when the browser cancels the request', async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(window, 'PublicKeyCredential', {
        value: window.PublicKeyCredential || class {},
        configurable: true,
      })
      Object.defineProperty(navigator, 'credentials', {
        value: {
          get: async () => {
            throw new DOMException('The operation was aborted', 'NotAllowedError')
          },
        },
        configurable: true,
      })
    })
    await page.goto('/login')
    await page.getByTestId('login-passkey-button').click()
    await expect(page.getByTestId('login-passkey-error')).toHaveText('Passkey request timed out or was cancelled.')
  })

  test('hides the entry when WebAuthn is unavailable', async ({ page }) => {
    await page.addInitScript(() => {
      // @ts-expect-error test shim: simulate a WebAuthn-less browser
      delete window.PublicKeyCredential
    })
    await page.goto('/login')
    await expect(page.getByTestId('login-passkey-button')).toHaveCount(0)
  })
})

test.describe('Passkey credential delete (UF-03)', () => {
  test.beforeEach(async ({ page }) => {
    await setupMocks(page)
  })

  test('deletes a passkey after confirmation and refreshes the list', async ({ page }) => {
    await loginUser(page)
    await page.goto('/security')
    await expect(page.locator('text=My Passkey')).toBeVisible()

    const deleteRequest = page.waitForRequest(
      (req) => req.method() === 'DELETE' && req.url().includes('/api/me/webauthn/credentials/cred-1'),
    )
    await page.getByTestId('passkey-delete-button').click()
    // AppConfirmDialog guards the destructive action (stable testids).
    await page.getByTestId('confirm-dialog-confirm').click()
    await deleteRequest

    await expect(page.locator('text=Passkey "My Passkey" deleted.')).toBeVisible()
    await expect(page.locator('text=My Passkey').first()).toBeVisible()
  })

  test('shows the catalog copy when the credential answers 404', async ({ page }) => {
    await loginUser(page)
    await page.route('**/api/me/webauthn/credentials/*', async (route) => {
      if (route.request().method() === 'DELETE') {
        await route.fulfill({
          status: 404,
          contentType: 'application/json',
          body: JSON.stringify({ error: { code: 'VALIDATION_RESOURCE_NOT_FOUND', category: 'VALIDATION', message: 'Credential not found', numeric_code: 1004, request_id: 'req-e2e-passkey-404' } }),
        })
      } else {
        await route.continue()
      }
    })
    await page.goto('/security')
    await page.getByTestId('passkey-delete-button').click()
    await page.getByTestId('confirm-dialog-confirm').click()
    // Anti-enumeration 404 → the generic not-found copy, as-is.
    await expect(page.locator('text=Resource not found')).toBeVisible()
  })
})
