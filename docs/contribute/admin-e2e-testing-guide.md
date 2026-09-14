# Playwright E2E Automated Testing Integration Guide

> Summarized from the OAuth2 Admin project's practices, as a reference for adopting Playwright E2E testing in other frontend projects.

---

## Table of Contents

1. [Core Principles](#1-core-principles)
2. [Project Setup](#2-project-setup)
3. [Mock API Layer Design](#3-mock-api-layer-design)
4. [Test File Organization](#4-test-file-organization)
5. [Test Writing Patterns](#5-test-writing-patterns)
6. [Advanced Techniques](#6-advanced-techniques)
7. [CI/CD Integration](#7-cicd-integration)
8. [FAQ](#8-faq)

---

## 1. Core Principles

### 1.1 Why request interception instead of a real backend

| Approach | Pros | Cons |
|------|------|------|
| **Request interception mocks** | No backend dependency, fast execution (&lt;5s), stable and non-flaky, full control over responses | Does not verify frontend-backend integration |
| **Real backend** | Verifies end-to-end integration | Requires database/cache/services, slow (minutes), many environment dependencies, hard data isolation |
| **MSW (Mock Service Worker)** | Intercepts at the browser layer, closer to real behavior | Complex configuration, requires Service Worker support |

This project uses **Playwright's native `page.route()` request interception**, for the following reasons:

- Zero extra dependencies (built into Playwright)
- Clean and intuitive API
- Interception happens before the network layer, delivering the best performance
- Supports precise URL pattern and HTTP method matching
- Supports overriding the global mocks within a single test

### 1.2 How request interception works

```
┌──────────────┐     HTTP request     ┌──────────────────┐
│  Frontend    │ ───────────────────→ │  page.route()    │
│  app code    │                      │  URL pattern     │
│  (browser)   │ ←─────────────────── │  route.fulfill() │
└──────────────┘    Mock response     └──────────────────┘
                                          ↑ bypasses the
                                    network layer entirely
                                    (no real HTTP connection)
```

Playwright's `page.route()` intercepts requests at the browser's network layer, so **requests never leave the browser process**. This means:

- No backend service needs to be running
- No network connection required
- Responses are immediate, with zero latency
- Tests are fully deterministic, with no network flakiness

### 1.3 Three-layer architecture

```
tests/e2e/
├── helpers/
│   └── mock-api.ts          ← Layer 1: mock data + interceptors
├── auth.spec.ts             ← Layer 2: test cases
├── applications.spec.ts
└── ...

playwright.config.ts         ← Layer 3: Playwright configuration
```

| Layer | Responsibility | Change frequency |
|------|------|---------|
| **Mock layer** | Defines mock data constants + the `setupAuthenticatedMocks()` global interception function | When the backend API changes |
| **Test layer** | Contains the actual test cases and calls functions provided by the mock layer | When new features/pages are added |
| **Config layer** | Playwright runtime options, browsers, webServer | At project setup |

---

## 2. Project Setup

### 2.1 Install dependencies

```bash
npm install -D @playwright/test
npx playwright install chromium
```

Chromium alone is enough; there is no need to install WebKit/Firefox. The goal of E2E tests is to verify functional logic, not cross-browser compatibility.

### 2.2 Playwright configuration

Create `playwright.config.ts`:

```typescript
import { defineConfig, devices } from '@playwright/test'

export default defineConfig({
  testDir: './tests/e2e',        // test file directory
  fullyParallel: true,            // run fully in parallel (enable when tests have no interdependencies)
  forbidOnly: !!process.env.CI,   // forbid test.only on CI (prevents accidental commits)
  retries: process.env.CI ? 2 : 0, // retry twice on CI (mitigates flakiness)
  workers: process.env.CI ? 1 : undefined, // single worker on CI (avoids resource contention)
  reporter: 'html',               // HTML test report

  use: {
    baseURL: 'http://localhost:5174',  // application base URL (used with page.goto())
    trace: 'on-first-retry',           // record a trace on failed retries (for debugging)
  },

  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],

  webServer: {
    command: 'npm run dev',              // start the dev server automatically
    url: 'http://localhost:5174/',       // wait until this URL is reachable
    reuseExistingServer: !process.env.CI, // reuse an already-running server locally
    timeout: 30000,                      // 30s startup timeout
  },
})
```

**Key configuration notes:**

| Option | Purpose | Recommended value |
|--------|------|--------|
| `baseURL` | Prefix automatically prepended when calling `page.goto('/path')` | Dev server address |
| `webServer` | Starts/reuses the dev server automatically | Distinct settings for dev mode vs CI mode |
| `trace` | Generates a trace file on failure, inspectable with `npx playwright show-trace` | `on-first-retry` |
| `fullyParallel` | Runs multiple test files in parallel | `true` (safe with mocking) |
| `retries` | Number of retries on failure | CI: 2, local: 0 |

### 2.3 package.json scripts

```json
{
  "scripts": {
    "test:e2e": "playwright test",
    "test:e2e:headed": "playwright test --headed",
    "test:e2e:ui": "playwright test --ui"
  }
}
```

### 2.4 Directory structure

```
your-project/
├── playwright.config.ts
├── package.json
├── src/                        ← application source
└── tests/
    └── e2e/
        ├── helpers/
        │   └── mock-api.ts     ← mock data + interception functions
        ├── auth.spec.ts        ← authentication-related tests
        ├── page-a.spec.ts      ← page A tests
        └── page-b.spec.ts      ← page B tests
```

---

## 3. Mock API Layer Design

The mock API layer is the **core** of the entire testing system. Design this layer well and writing test cases becomes straightforward.

### 3.1 File structure

`tests/e2e/helpers/mock-api.ts` consists of three parts:

```
Part 1: Mock data constants       →  fake data for all APIs
Part 2: setupAuthenticatedMocks() →  registers global route interception
Part 3: Helper functions          →  common operations such as loginAsAdmin()
```

### 3.2 Mock data constants

**Principle: keep the data as realistic as possible, with fields matching the backend API responses.**

```typescript
// ✅ Good design: field names and types match the real API
export const MOCK_USERS = [
  {
    id: '550e8400-e29b-41d4-a716-446655440000',
    username: 'admin',
    email: 'admin@example.com',
    email_verified: true,    // boolean, not a string
    mfa_enabled: true,
  },
  {
    id: '660e8400-e29b-41d4-a716-446655440001',
    username: 'testuser',
    email: 'test@example.com',
    email_verified: false,
    mfa_enabled: false,
  },
]

// ❌ Bad design: arbitrary field names, unrealistic data
export const users = [
  { uid: 1, name: 'a', mail: 'a@b' },  // field names don't match the real API
]
```

**Why multiple data sets?**

Prepare at least two states in the mock data to cover different UI presentations:

- `email_verified: true` + `false` → test the "verified" and "pending verification" badges
- `mfa_enabled: true` + `false` → test the "enabled" and "disabled" badges

### 3.3 Route interception: setupAuthenticatedMocks()

This is the most critical function; it intercepts every API request the frontend makes.

```typescript
import { Page } from '@playwright/test'

export async function setupAuthenticatedMocks(page: Page) {
  // Interception rule: ** is a wildcard that matches any origin
  await page.route('**/api/users', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ users: MOCK_USERS }),
    })
  })
}
```

**URL matching patterns:**

| Pattern | What it matches | Example |
|------|---------|------|
| `**/api/users` | Any origin + exact path match | `http://localhost:5174/api/users` ✅ |
| `**/api/admin/logs**` | Path prefix match (including query parameters) | `/api/admin/logs?page=2` ✅ |
| `**/api/admin/clients/*` | Path + single-segment wildcard | `/api/admin/clients/fulla-portal` ✅ |
| `**/api/admin/clients/*/reset-secret` | Multi-segment path combination | `/api/admin/clients/fulla-portal/reset-secret` ✅ |

**Same URL, different HTTP methods:**

```typescript
await page.route('**/api/admin/clients', async (route) => {
  if (route.request().method() === 'GET') {
    await route.fulfill({ status: 200, body: JSON.stringify({ clients: MOCK_CLIENTS }) })
  } else if (route.request().method() === 'POST') {
    await route.fulfill({ status: 201, body: JSON.stringify({ client_id: 'new-123' }) })
  } else {
    // Unexpected method: pass to the next handler or the real network
    await route.continue()
  }
})
```

**Sub-resource route priority:**

Playwright matches routes in registration order, so **more specific routes should be registered first**:

```typescript
// ✅ Correct: register more specific routes first
await page.route('**/api/admin/clients/*/reset-secret', ...)  // matched first
await page.route('**/api/admin/clients/*', ...)                // matched later (fallback)

// In practice, Playwright's wildcard matching has an implicit priority,
// but explicitly checking inside the handler is safer:
await page.route('**/api/admin/clients/*', async (route) => {
  const url = route.request().url()
  if (url.includes('/scopes') || url.includes('/reset-secret')) {
    await route.continue()  // skip; let a more specific handler deal with it
    return
  }
  // ... handle DELETE / GET / PUT
})
```

### 3.4 Helper function: loginAsAdmin()

```typescript
export async function loginAsAdmin(page: Page) {
  await page.goto('/login')
  await page.fill('input[type="text"]', 'admin')
  await page.fill('input[type="password"]', 'admin')
  await page.click('button[type="submit"]')
  await page.waitForURL('**/dashboard')  // wait for the successful-login redirect
}
```

**Design notes:**

- Logs in through UI interactions (simulating a real user)
- Relies on `setupAuthenticatedMocks()` having intercepted the authentication APIs
- `waitForURL` guarantees the login has completed, so subsequent tests run in an authenticated state

### 3.5 Mock template for adapting to other projects

```typescript
// === helpers/mock-api.ts template ===

import { Page } from '@playwright/test'

// ---- Part 1: mock data ----

export const CURRENT_USER = {
  id: '1',
  name: 'Test User',
  email: 'test@example.com',
  role: 'admin',
}

export const MOCK_ITEMS = [
  { id: '1', title: 'Item A', status: 'active' },
  { id: '2', title: 'Item B', status: 'inactive' },
]

// ---- Part 2: global route interception ----

export async function setupAuthenticatedMocks(page: Page) {
  // Authentication
  await page.route('**/auth/login', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ token: 'mock-jwt-token', user: CURRENT_USER }),
    })
  })

  await page.route('**/auth/me', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(CURRENT_USER),
    })
  })

  // Business data
  await page.route('**/api/items', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ items: MOCK_ITEMS }),
      })
    } else if (route.request().method() === 'POST') {
      const body = JSON.parse(route.request().postData() || '{}')
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({ id: 'new-' + Date.now(), ...body }),
      })
    } else {
      await route.continue()
    }
  })

  // Single-item operations (GET / PUT / DELETE)
  await page.route('**/api/items/*', async (route) => {
    if (route.request().method() === 'DELETE') {
      await route.fulfill({ status: 204 })
    } else {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify(MOCK_ITEMS[0]),
      })
    }
  })
}

// ---- Part 3: helper functions ----

export async function loginAs(page: Page, username = 'admin', password = 'admin') {
  await page.goto('/login')
  await page.fill('[name="username"]', username)
  await page.fill('[name="password"]', password)
  await page.click('button[type="submit"]')
  await page.waitForURL('**/dashboard')
}
```

---

## 4. Test File Organization

### 4.1 Naming conventions

```
tests/e2e/
  ├── helpers/
  │   └── mock-api.ts         ← fixed name, shared by all tests
  ├── auth.spec.ts             ← authentication/login related
  ├── {page-name}.spec.ts      ← one file per page
  └── navigation.spec.ts       ← global navigation/layout
```

**One page = one spec file**, organized by functional domain rather than by operation type.

### 4.2 test.describe grouping

```typescript
test.describe('Page/Feature name', () => {
  // beforeEach: shared setup
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    // navigate to the target page
    await page.click('nav a:has-text("Target Page")')
    await page.waitForURL('**/target-page')
  })

  // Test cases ordered: rendering → data → interaction → edge cases
  test('displays page title', ...)
  test('shows data from API', ...)
  test('button click triggers action', ...)
  test('shows error on failure', ...)
})
```

### 4.3 Test case naming

Use **declarative sentences** that describe expected behavior, not operation steps:

```typescript
// ✅ Good: describes the expected outcome
test('displays users list with correct columns', ...)
test('shows error on login failure', ...)
test('delete button removes item from list', ...)

// ❌ Bad: describes operation steps
test('clicks button and checks result', ...)
test('test 1', ...)
```

---

## 5. Test Writing Patterns

### 5.1 Standard test flow (the beforeEach pattern)

**The most common pattern** — 90% of tests follow this flow:

```typescript
test.describe('User Management', () => {
  test.beforeEach(async ({ page }) => {
    // Step 1: register global mocks
    await setupAuthenticatedMocks(page)
    // Step 2: simulate login
    await loginAsAdmin(page)
    // Step 3: navigate to the target page
    await page.click('nav a:has-text("Users")')
    await page.waitForURL('**/users')
  })

  test('displays user table', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Users')
    await expect(page.locator('th:has-text("Username")')).toBeVisible()
  })
})
```

**Flow diagram:**

```
beforeEach:
  setupAuthenticatedMocks(page)
       ↓
  loginAsAdmin(page)
       ↓
  navigate to the target page + waitForURL
       ↓
  ═══════════════════════════
  ↓  test case 1 runs     ↓
  ↓  test case 2 runs     ↓
  ↓  ...                  ↓
  ═══════════════════════════
```

### 5.2 Verifying page rendering

Verify that the page displays data correctly:

```typescript
test('displays users list with correct columns', async ({ page }) => {
  // Verify the title
  await expect(page.locator('h2')).toContainText('Users')
  // Verify the table headers
  await expect(page.locator('th:has-text("Username")')).toBeVisible()
  await expect(page.locator('th:has-text("Email")')).toBeVisible()
  // Verify data rows (from the mock data)
  const tableBody = page.locator('tbody')
  await expect(tableBody.getByRole('cell', { name: 'admin', exact: true })).toBeVisible()
})
```

### 5.3 Form interaction tests

Verify form filling, submission, and responses:

```typescript
test('creates a new application and shows secret', async ({ page }) => {
  // 1. Open the dialog
  await page.click('button:has-text("Create Application")')
  // 2. Verify the dialog appears
  await expect(page.locator('h3:has-text("Create Application")')).toBeVisible()
  // 3. Fill in the form
  await page.fill('input[placeholder="My App"]', 'Test Application')
  await page.selectOption('select', 'CONFIDENTIAL')
  // 4. Submit
  await page.locator('.fixed button[type="submit"]').click()
  // 5. Verify the result
  await expect(page.locator('h3:has-text("Client Secret")')).toBeVisible()
  await expect(page.locator('.font-mono.select-all')).toContainText('generated-secret-abc123xyz')
})
```

### 5.4 Modal/dialog tests

```typescript
test('opens and closes role assignment modal', async ({ page }) => {
  // Open
  await page.click('button:has-text("Assign Roles")')
  await expect(page.locator('h3:has-text("Assign Roles")')).toBeVisible()

  // Close
  await page.click('button:has-text("Cancel")')
  await expect(page.locator('h3:has-text("Assign Roles")')).not.toBeVisible()
})

// Handling native confirm dialogs
test('delete with confirmation', async ({ page }) => {
  page.on('dialog', (dialog) => dialog.accept())  // auto-accept the confirm
  await page.click('button:has-text("Delete")')
  await expect(page.locator('h2')).toContainText('Applications')
})
```

### 5.5 Pagination tests

```typescript
test('pagination sends correct page parameter', async ({ page }) => {
  // Build enough data to enable the "Next" button
  const manyItems = Array.from({ length: 50 }, (_, i) => ({
    id: i + 1, title: `Item ${i}`, status: 'active',
  }))

  let requestedPage = 1
  await page.route('**/api/items**', async (route) => {
    const url = new URL(route.request().url())
    requestedPage = parseInt(url.searchParams.get('page') || '1')
    await route.fulfill({
      status: 200,
      body: JSON.stringify({ items: requestedPage === 1 ? manyItems : MOCK_ITEMS }),
    })
  })

  // Navigate away and back to trigger a reload
  await page.click('nav a:has-text("Dashboard")')
  await page.click('nav a:has-text("Items")')
  await page.waitForURL('**/items')

  // Click Next
  const nextBtn = page.locator('button:has-text("Next")')
  await expect(nextBtn).not.toBeDisabled()
  await nextBtn.click()
  await expect(page.locator('text=Page 2')).toBeVisible()
  expect(requestedPage).toBe(2)
})
```

### 5.6 Navigation tests

```typescript
test('sidebar navigation works for all pages', async ({ page }) => {
  // Verify nav items are visible
  await expect(page.locator('nav a:has-text("Dashboard")')).toBeVisible()
  await expect(page.locator('nav a:has-text("Users")')).toBeVisible()

  // Click and verify URL + page title
  await page.click('nav a:has-text("Users")')
  await expect(page).toHaveURL(/\/users/)
  await expect(page.locator('h2')).toContainText('Users')

  // Navigate back and verify
  await page.click('nav a:has-text("Dashboard")')
  await expect(page).toHaveURL(/\/dashboard/)
})
```

---

## 6. Advanced Techniques

### 6.1 Overriding global mocks (testing error scenarios)

This is the most powerful feature of this approach: **override the global mocks within a single test, without modifying setupAuthenticatedMocks()**.

**How it works:** handlers registered later via `page.route()` are matched first. A later registration overrides an earlier one for the same URL pattern.

```typescript
test.describe('Dashboard', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)  // global mocks: health returns ok
    await loginAsAdmin(page)
  })

  test('displays healthy status', async ({ page }) => {
    await expect(page.locator('text=Healthy')).toBeVisible()  // uses the global mock
  })

  test('shows unhealthy status when backend is down', async ({ page }) => {
    // Key point: registered after the global mocks, overriding the global health response
    await page.route('**/health/ready', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'error', message: 'Database unreachable' }),
      })
    })

    await loginAsAdmin(page)  // log in again to trigger the health check
    await expect(page.locator('text=Unhealthy')).toBeVisible()
  })
})
```

**Common override scenarios:**

```typescript
// Scenario 1: API returns an error
await page.route('**/oauth2/login', async (route) => {
  await route.fulfill({ status: 401, body: JSON.stringify({ error: 'invalid_credentials' }) })
})

// Scenario 2: API returns empty data
await page.route('**/api/admin/clients', async (route) => {
  if (route.request().method() === 'GET') {
    await route.fulfill({ status: 200, body: JSON.stringify({ clients: [] }) })
  } else {
    await route.continue()
  }
})

// Scenario 3: non-admin user
await page.route('**/oauth2/userinfo', async (route) => {
  await route.fulfill({
    status: 200,
    body: JSON.stringify({ sub: '123', username: 'user', roles: ['user'] }),
  })
})

// Scenario 4: MFA required
await page.route('**/oauth2/login', async (route) => {
  await route.fulfill({
    status: 200,
    body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
  })
})
```

### 6.2 Capturing and verifying request bodies

Verify that the parameters the frontend sends are correct:

```typescript
test('assigns roles with correct request body', async ({ page }) => {
  let requestBody: any = null

  // Override the global mock and capture the request body at the same time
  await page.route('**/api/admin/users/*/roles', async (route) => {
    requestBody = JSON.parse(route.request().postData() || '{}')
    await route.fulfill({
      status: 200,
      body: JSON.stringify({ message: 'Roles updated' }),
    })
  })

  // Perform the action
  await page.locator('button:has-text("Assign Roles")').first().click()
  await page.fill('input[placeholder="admin, user"]', 'admin, editor')
  await page.click('button:has-text("Save Roles")')

  // Verify the request body
  expect(requestBody).toEqual({ roles: ['admin', 'editor'] })
})
```

### 6.3 Empty-state tests

Verify the UI presentation when the list is empty:

```typescript
test('shows empty state when no items exist', async ({ page }) => {
  // Override the mock to return an empty list
  await page.route('**/api/items', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({ status: 200, body: JSON.stringify({ items: [] }) })
    } else {
      await route.continue()
    }
  })

  // Navigate away and back to trigger a data reload
  await page.click('nav a:has-text("Dashboard")')
  await page.click('nav a:has-text("Items")')
  await page.waitForURL('**/items')

  // Verify the empty-state prompt
  await expect(page.locator('text=No items yet')).toBeVisible()
  await expect(page.locator('button:has-text("Create your first item")')).toBeVisible()
})
```

**Why "navigate away and back"?**

Because `beforeEach` has already navigated to the target page and the data is loaded, overriding the mock requires forcing the component to remount and re-fetch the data. Two approaches:

1. Navigate to another page and back (recommended; simulates real user behavior)
2. `await page.reload()` (simple, but some components may not re-request)

### 6.4 Verifying request query parameters

```typescript
test('filter sends correct parameters', async ({ page }) => {
  let capturedUrl = ''
  await page.route('**/api/items**', async (route) => {
    capturedUrl = route.request().url()
    await route.fulfill({ status: 200, body: JSON.stringify({ items: [] }) })
  })

  // Perform the filter operation
  await page.selectOption('select[name="status"]', 'active')
  await page.click('button:has-text("Filter")')

  // Verify the URL parameters
  const url = new URL(capturedUrl)
  expect(url.searchParams.get('status')).toBe('active')
})
```

---

## 7. CI/CD Integration

### 7.1 GitHub Actions example

```yaml
name: E2E Tests

on: [push, pull_request]

jobs:
  e2e:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with:
          node-version: 20

      - name: Install dependencies
        run: npm ci

      - name: Install Playwright browsers
        run: npx playwright install chromium --with-deps

      - name: Run E2E tests
        run: npm run test:e2e

      - name: Upload test report
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: playwright-report
          path: playwright-report/
```

### 7.2 CI configuration notes

| Option | CI value | Local value | Reason |
|------|-------|-------|------|
| `workers` | 1 | auto (multiple) | CI resources are limited; avoids contention |
| `retries` | 2 | 0 | CI networks are unstable; retries mitigate flakiness |
| `reuseExistingServer` | `false` | `true` | CI must start a fresh server |
| `forbidOnly` | `true` | `false` | Prevents `test.only` from being committed |

---

## 8. FAQ

### Q1: What if tests fail intermittently (flaky)?

1. Check whether you use `page.waitForTimeout()` — switch to `waitForSelector` / `waitForURL` / `expect().toBeVisible()`
2. Check the mocks for race conditions — make sure every API is intercepted and no request is left unmocked
3. Enable `trace: 'on-first-retry'` and analyze failures with `npx playwright show-trace`

### Q2: What if a request is not intercepted?

- Check that the URL pattern matches (the scope of the `**` wildcard)
- Open the browser DevTools and inspect the full URL of the actual request
- Add `console.log(route.request().url())` inside the handler to debug

### Q3: How do I test file uploads?

```typescript
// Listen for the filechooser event
const [fileChooser] = await Promise.all([
  page.waitForEvent('filechooser'),
  page.click('button:has-text("Upload")'),  // triggers the file selection
])
await fileChooser.setFiles({
  name: 'test.csv',
  mimeType: 'text/csv',
  buffer: Buffer.from('name,value\ntest,123'),
})
```

### Q4: How do I run tests without auto-starting the dev server?

Edit `playwright.config.ts`, remove the `webServer` configuration, start the dev server manually, then run the tests:

```bash
# Terminal 1
npm run dev

# Terminal 2
npm run test:e2e
```

### Q5: How do mock-mode and real-backend tests coexist?

```
tests/
├── e2e/
│   ├── helpers/
│   │   ├── mock-api.ts      ← mock mode
│   │   └── api-client.ts    ← real API calls
│   ├── auth.spec.ts          ← mock tests
│   └── ...
└── integration/
    └── full-flow.spec.ts     ← real-backend integration tests
```

Use separate `playwright.config.ts` files:

- `playwright.config.ts` — mock mode (daily development, every commit)
- `playwright.integration.config.ts` — real backend (pre-release, nightly)

---

## 9. Backend Integration Test Troubleshooting

### Problem: the test script fails entirely on its second run

**Symptoms**:
```
POST http://localhost:5174/oauth2/login 401 (Unauthorized)
```

Backend logs:
```
WARN  Account locked for user: admin until 1779441748
INFO  [METRIC] oauth2_login_failures_total reason=bad_credentials
```

**Cause**:
The OAuth2 system implements an account lockout mechanism. Once the number of failed logins reaches a threshold, the account is temporarily locked:

- 5 failures → 1-minute lockout
- 10 failures → 5-minute lockout
- 15 failures → 30-minute lockout
- 20+ failures → 1-hour lockout

**Solutions**:

#### Option 1: use a test script with automatic cleanup

The backend test scripts (for example `test-admin-endpoints.ps1`) already reset the account lockout state automatically when they finish.

For a local PostgreSQL database, you need to configure the database password:

```powershell
# Edit the test script, find the cleanup section, and set the password
$env:PGPASSWORD = "your_password"  # change to the actual password
```

#### Option 2: manually reset the account lockout

```powershell
# Use the reset script
$env:PGPASSWORD = "your_password"
.\scripts\backend\reset-account-lockout.ps1
$env:PGPASSWORD = $null

# Or use SQL directly
psql -U fulla_user -d fulla_db -h localhost -c "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username='admin';"
```

#### Option 3: wait for the lockout to expire

Depending on the number of failures, the account unlocks automatically after the corresponding time.

**Preventive measures**:

1. **Use a dedicated test account**: never use the production admin account in tests
2. **Make sure credentials are correct**: check the username and password in the test script
3. **Clean up automatically after testing**: append cleanup code at the end of the test script

For details, see [Account lockout mechanism](operate/account-lockout.md).

---

## Appendix A: From-scratch adoption checklist

Use this checklist when adopting E2E testing in a new project:

- [ ] `npm install -D @playwright/test`
- [ ] `npx playwright install chromium`
- [ ] Create `playwright.config.ts`
- [ ] Create `tests/e2e/helpers/mock-api.ts`
- [ ] Define mock data constants (matching the backend API response structure)
- [ ] Implement `setupAuthenticatedMocks(page)` — intercept all authentication + business APIs
- [ ] Implement `loginAsAdmin(page)` — a UI login helper
- [ ] Create the first test file (auth is a good starting point)
- [ ] Add `package.json` scripts
- [ ] Configure the CI pipeline

## Appendix B: Admin frontend E2E test statistics

| Metric | Value |
|------|------|
| Test files | 16 |
| Test cases | 174 |
| Execution time | ~1 minute |
| Backend dependency | None (fully mocked) |
| Browser | Chromium |
| Parallel execution | Yes |
| Mocked API endpoints | 15+ |

---

> This document summarizes practices from the fulla Admin frontend project. Project source: `frontends/admin/tests/e2e/`
