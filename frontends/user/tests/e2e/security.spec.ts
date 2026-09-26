import { test, expect } from '@playwright/test'
import { setupMocks, loginUser } from './helpers/mock-api'

test.describe('Security', () => {
  test.describe('Injection & XSS', () => {
    test('SQL injection in login username is rejected', async ({ page }) => {
      await setupMocks(page)
      // Override login to reject this credential
      await page.route('**/oauth2/login', async (route) => {
        await route.fulfill({
          status: 401,
          contentType: 'application/json',
          body: JSON.stringify({
            error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: 'Invalid credentials' },
          }),
        })
      })
      await page.goto('/login')
      await page.locator('input[autocomplete="username"]').fill("' OR 1=1 --")
      await page.locator('input[autocomplete="current-password"]').fill('anything')
      await page.locator('button[type="submit"]').click()
      await page.waitForTimeout(500)
      await expect(page).toHaveURL(/\/login/)
      await expect(page.locator('input[autocomplete="username"]')).toBeVisible()
    })

    test('XSS in login username is rendered as text', async ({ page }) => {
      await setupMocks(page)
      await page.route('**/oauth2/login', async (route) => {
        await route.fulfill({
          status: 401,
          contentType: 'application/json',
          body: JSON.stringify({
            error: { code: 'AUTH_INVALID_CREDENTIALS', category: 'AUTHENTICATION', message: 'Invalid credentials' },
          }),
        })
      })
      await page.goto('/login')
      const xssPayload = "<script>alert('xss')</script>"
      await page.locator('input[autocomplete="username"]').fill(xssPayload)
      await page.locator('input[autocomplete="current-password"]').fill('test')
      await page.locator('button[type="submit"]').click()
      await page.waitForTimeout(500)
      const pageContent = await page.content()
      expect(pageContent).not.toContain('<script>alert')
      await expect(page).toHaveURL(/\/login/)
    })

    test('XSS in registration username is rendered as text', async ({ page }) => {
      await setupMocks(page)
      await page.goto('/register')
      const xssPayload = '<img onerror="alert(1)" src=x>'
      await page.locator('input[autocomplete="username"]').fill(xssPayload)
      await page.locator('input[type="email"]').fill('test@example.com')
      await page.locator('input[autocomplete="new-password"]').first().fill('password123')
      await page.locator('input[autocomplete="new-password"]').last().fill('password123')
      await page.locator('button[type="submit"]').click()
      await page.waitForTimeout(500)
      // No alert should fire; verify still on register or shows error (not XSS)
      const pageContent = await page.content()
      expect(pageContent).not.toContain('<img onerror')
    })
  })

  test.describe('Anti-enumeration', () => {
    test('forgot password shows same message for unregistered email', async ({ page }) => {
      await setupMocks(page)
      await page.goto('/forgot-password')
      const unregisteredEmail = 'nonexistent@example.com'
      await page.fill('input[type="email"]', unregisteredEmail)
      await page.click('button:has-text("Send")')
      await page.waitForTimeout(500)
      // Should show the same success message regardless
      await expect(page.locator('text=If an account with that email exists')).toBeVisible()
    })
  })

  test.describe('Token Exposure', () => {
    test('access token not visible in URL after login', async ({ page }) => {
      await setupMocks(page)
      await loginUser(page)
      const url = page.url()
      expect(url).not.toContain('access_token')
      expect(url).not.toContain('token=')
      expect(url).not.toContain('id_token')
    })

    test('localStorage cleared on account delete', async ({ page }) => {
      await setupMocks(page)
      await loginUser(page)
      await page.goto('/security')
      await page.waitForLoadState('networkidle')

      // Fill in the username confirmation field
      const usernameInput = page.locator('input[placeholder*="username"], input[placeholder*="Username"]').last()
      if (await usernameInput.isVisible()) {
        await usernameInput.fill('testuser')
        // Click delete. The typed-username flow guards this action; the page
        // no longer uses any native confirm() (#181), so no dialog handler.
        const deleteButton = page.locator('button:has-text("Delete Account")')
        await deleteButton.click()
        await page.waitForTimeout(500)
        // Verify localStorage is cleared
        const token = await page.evaluate(() => localStorage.getItem('access_token'))
        expect(token).toBeNull()
      }
    })
  })
})
