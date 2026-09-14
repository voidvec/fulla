import { Page } from '@playwright/test'

/**
 * Mock API responses for E2E tests.
 * Intercepts backend calls so tests run without a real OAuth2 server.
 */

export const ADMIN_USER = {
  sub: '550e8400-e29b-41d4-a716-446655440000',
  name: 'admin',
  username: 'admin',
  email: 'admin@example.com',
  email_verified: true,
  roles: ['admin', 'user'],
}

export const MOCK_CLIENTS = [
  {
    client_id: 'fulla-portal',
    name: 'Vue Frontend',
    client_type: 'PUBLIC',
    redirect_uris: 'http://localhost:8080/callback',
    allowed_grant_types: 'authorization_code',
  },
  {
    client_id: 'api-service',
    name: 'API Service',
    client_type: 'CONFIDENTIAL',
    redirect_uris: 'https://api.example.com/callback',
    allowed_grant_types: 'client_credentials',
  },
]

export const MOCK_USERS = [
  {
    id: 1,
    username: 'admin',
    email: 'admin@example.com',
    email_verified: true,
    mfa_enabled: true,
  },
  {
    id: 2,
    username: 'testuser',
    email: 'test@example.com',
    email_verified: false,
    mfa_enabled: false,
  },
]

export const MOCK_SCOPES = [
  { id: 1, name: 'openid', description: 'OpenID Connect', mapped_role: null, is_default: true, requires_admin_role: false },
  { id: 2, name: 'profile', description: 'User profile', mapped_role: null, is_default: true, requires_admin_role: false },
  { id: 3, name: 'admin', description: 'Admin access', mapped_role: 'admin', is_default: false, requires_admin_role: true },
]

export const MOCK_LOGS = [
  { id: 1, action: 'login_success', actor_type: 'user', actor_id: '550e8400-e29b-41d4-a716-446655440000', outcome: 'success', ip: '127.0.0.1', timestamp: '2026-05-21T10:00:00Z' },
  { id: 2, action: 'token_issued', actor_type: 'client', actor_id: 'fulla-portal', outcome: 'success', ip: '127.0.0.1', timestamp: '2026-05-21T10:00:01Z' },
  { id: 3, action: 'login_failure', actor_type: 'user', actor_id: '660e8400-e29b-41d4-a716-446655440001', outcome: 'failure', ip: '192.168.1.100', timestamp: '2026-05-21T09:55:00Z' },
]

export const MOCK_CLIENT_DETAIL = {
  status: 'success',
  client_id: 'fulla-portal',
  name: 'Vue Frontend',
  client_type: 'PUBLIC',
  redirect_uris: 'http://localhost:8080/callback',
  allowed_grant_types: 'authorization_code,refresh_token',
  created_at: '2026-05-20T10:00:00Z',
  scopes: ['openid', 'profile'],
}

export const MOCK_TOKENS = [
  { token_prefix: 'a1b2c3d4', client_id: 'fulla-portal', user_id: 'admin', scope: 'openid profile', created_at: '2026-05-21T10:00:00Z', expires_at: '2026-05-21T11:00:00Z' },
  { token_prefix: 'e5f6g7h8', client_id: 'api-service', user_id: '', scope: 'admin', created_at: '2026-05-21T09:30:00Z', expires_at: '2026-05-21T10:30:00Z' },
  { token_prefix: 'i9j0k1l2', client_id: 'fulla-portal', user_id: 'testuser', scope: 'openid', created_at: '2026-05-21T09:00:00Z', expires_at: '2026-05-21T10:00:00Z' },
]

export const MOCK_OIDC_KEYS = {
  status: 'success',
  kid: 'default-key-1',
  kty: 'RSA',
  alg: 'RS256',
  use: 'sig',
  jwks_uri: '/.well-known/jwks.json',
  discovery_uri: '/.well-known/openid-configuration',
  key_status: 'active',
  note: 'Key rotation is not yet implemented. Single signing key in use.',
}

export const MOCK_ROLES = [
  { id: 1, name: 'admin', description: 'System Administrator with full access', user_count: 1, created_at: '2026-05-01T00:00:00Z' },
  { id: 2, name: 'user', description: 'Standard user with basic access', user_count: 2, created_at: '2026-05-01T00:00:00Z' },
]

export const MOCK_USER_DETAIL = {
  status: 'success',
  id: 1,
  username: 'admin',
  email: 'admin@example.com',
  email_verified: true,
  mfa_enabled: true,
  failed_login_count: 0,
  locked: false,
  locked_until: 0,
  org_id: null,
  created_at: '2026-05-01T00:00:00Z',
  roles: ['admin', 'user'],
}

export const MOCK_DASHBOARD_STATS = {
  status: 'success',
  total_users: 5,
  total_clients: 3,
  active_tokens: 12,
  logs_today: 47,
  failures_today: 2,
}

/**
 * Set up all API mocks for an authenticated admin session.
 */
export async function setupAuthenticatedMocks(page: Page) {
  // Login endpoint
  await page.route('**/oauth2/login', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ code: 'mock-auth-code-12345' }),
    })
  })

  // Token exchange
  await page.route('**/oauth2/token', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        access_token: process.env.E2E_MOCK_AT ?? 'fixture',
        refresh_token: process.env.E2E_MOCK_RT ?? 'fixture',
        token_type: 'Bearer',
        expires_in: 3600,
      }),
    })
  })

  // UserInfo
  await page.route('**/oauth2/userinfo', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(ADMIN_USER),
    })
  })

  // Health
  await page.route('**/health/ready', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ status: 'ok', database: 'connected', redis: 'connected' }),
    })
  })

  // Logout (#55): the admin store's logout() POSTs the bearer token here —
  // this endpoint revokes it AND triggers backchannel logout notification.
  await page.route('**/oauth2/logout', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ message: 'Logged out successfully' }),
    })
  })

  // Token revocation (RFC 7009) — logout also POSTs the refresh token here
  // via fetch({keepalive:true}). The mock accepts any token; the auth.ts
  // logout tolerates failure regardless.
  await page.route('**/oauth2/revoke', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: '{}' })
  })

  // MFA login completion (gap-fix E2): mirrors the backend contract —
  // required mfa_token/code/client_id/redirect_uri, optional code_verifier;
  // responds with a TokenResponse superset (message/mfa_verified included).
  await page.route('**/oauth2/mfa/verify', async (route) => {
    const body = route.request().postData() || ''
    const required = ['mfa_token=', 'code=', 'client_id=', 'redirect_uri=']
    if (required.some((k) => !body.includes(k))) {
      await route.fulfill({
        status: 400,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'VALIDATION_MISSING_REQUIRED_FIELD', category: 'VALIDATION', message: 'mfa_token, code, client_id and redirect_uri are required', numeric_code: 1001, request_id: 'req-e2e-mfa' } }),
      })
    } else {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({
          access_token: process.env.E2E_MOCK_AT ?? 'fixture',
          refresh_token: process.env.E2E_MOCK_RT ?? 'fixture',
          token_type: 'Bearer',
          expires_in: 3600,
          message: 'MFA verification successful',
          mfa_verified: true,
        }),
      })
    }
  })

  // Device approval (gap-fix E2/plan D5): form-urlencoded user_code+user_id.
  await page.route('**/oauth2/device/approve', async (route) => {
    const body = route.request().postData() || ''
    if (!body.includes('user_code=') || !body.includes('user_id=')) {
      await route.fulfill({
        status: 400,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'VALIDATION_MISSING_REQUIRED_FIELD', category: 'VALIDATION', message: 'user_code and user_id are required', numeric_code: 1001, request_id: 'req-e2e-device' } }),
      })
    } else {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'approved', user_code: 'WDJB-MJHT' }),
      })
    }
  })

  // Admin clients
  await page.route('**/api/admin/clients', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ clients: MOCK_CLIENTS }),
      })
    } else if (route.request().method() === 'POST') {
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({
          client_id: 'new-client-' + Date.now(),
          client_secret: process.env.E2E_MOCK_CS ?? 'generated-secret-abc123xyz', // matches applications.spec expectation
          name: 'New App',
          client_type: 'CONFIDENTIAL',
        }),
      })
    } else {
      await route.continue()
    }
  })

  // Delete client / Get client detail / Update client
  await page.route('**/api/admin/clients/*', async (route) => {
    const url = route.request().url()
    // Skip if it's a sub-resource like /scopes or /reset-secret
    if (url.includes('/scopes') || url.includes('/reset-secret')) {
      await route.continue()
      return
    }
    if (route.request().method() === 'DELETE') {
      await route.fulfill({ status: 204 })
    } else if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify(MOCK_CLIENT_DETAIL),
      })
    } else if (route.request().method() === 'PUT') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Client updated successfully', client_id: 'fulla-portal' }),
      })
    } else {
      await route.continue()
    }
  })

  // Reset secret
  await page.route('**/api/admin/clients/*/reset-secret', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ client_secret: process.env.E2E_MOCK_RS ?? 'new-secret-after-reset-xyz789' }), // matches applications.spec expectation
    })
  })

  // Admin users (GET list with pagination/filter, POST create). Trailing `*`
  // (not `/`-crossing) so the pattern also matches query strings
  // (?page=1&per_page=50&q=...); the detail/roles/disable/enable routes below
  // are registered later and take precedence (LIFO) for their paths.
  await page.route('**/api/admin/users*', async (route) => {
    const method = route.request().method()
    if (method === 'GET')
    {
      const url = new URL(route.request().url())
      const q = url.searchParams.get('q') || ''
      const locked = url.searchParams.get('locked')
      let page = parseInt(url.searchParams.get('page') || '1', 10) || 1
      let perPage = parseInt(url.searchParams.get('per_page') || '50', 10) || 50
      if (perPage > 100) perPage = 100
      if (perPage < 1) perPage = 50
      if (page < 1) page = 1

      let filtered = MOCK_USERS
      if (q)
      {
        const ql = q.toLowerCase()
        filtered = filtered.filter(
          (u) => (u.username || '').toLowerCase().startsWith(ql) ||
                 (u.email || '').toLowerCase().startsWith(ql)
        )
      }
      if (locked === 'true')
      {
        filtered = filtered.filter((u) => (u as any).locked === true)
      }
      else if (locked === 'false')
      {
        filtered = filtered.filter((u) => !(u as any).locked)
      }

      const total = filtered.length
      const totalPages = total > 0 ? Math.ceil(total / perPage) : 0
      const start = (page - 1) * perPage
      const pageUsers = filtered.slice(start, start + perPage)

      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({
          status: 'success',
          page,
          per_page: perPage,
          total,
          total_pages: totalPages,
          users: pageUsers,
        }),
      })
    }
    else if (method === 'POST')
    {
      const body = JSON.parse(route.request().postData() || '{}')
      // Duplicate username check.
      if (MOCK_USERS.some((u) => u.username === body.username))
      {
        await route.fulfill({
          status: 400,
          contentType: 'application/json',
          body: JSON.stringify({
            error: { code: 'VALIDATION_USERNAME_TAKEN', message: 'Username already exists' },
          }),
        })
      }
      else
      {
        const newUser = {
          id: Date.now(),
          username: body.username,
          email: body.email || '',
          email_verified: false,
          mfa_enabled: false,
        }
        await route.fulfill({
          status: 201,
          contentType: 'application/json',
          body: JSON.stringify({ status: 'success', message: 'User created successfully', user: newUser }),
        })
      }
    }
    else
    {
      await route.continue()
    }
  })

  // User detail - sub-resources (must be registered before the wildcard)
  await page.route('**/api/admin/users/*/roles', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', roles: [{ id: 1, name: 'admin', description: 'Admin' }] }),
      })
    } else if (route.request().method() === 'PUT') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Roles updated' }),
      })
    } else {
      await route.continue()
    }
  })

  await page.route('**/api/admin/users/*/disable', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ status: 'success', message: 'User disabled successfully' }),
    })
  })

  await page.route('**/api/admin/users/*/enable', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ status: 'success', message: 'User enabled successfully' }),
    })
  })

  // User detail - GET/PUT/DELETE for user info. Stateful: PUT applies the
  // known fields onto the current detail state (mirroring the backend's
  // contract — org_id: null clears, wrong types are a 400 — issues #53/#59)
  // so a subsequent GET reflects the update, like the real API.
  const userDetail: any = { ...MOCK_USER_DETAIL }
  await page.route('**/api/admin/users/*', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify(userDetail),
      })
    } else if (route.request().method() === 'PUT') {
      const body = JSON.parse(route.request().postData() || '{}')
      const typeOk =
        ['username', 'email'].every((k) => body[k] === undefined || typeof body[k] === 'string') &&
        ['email_verified', 'mfa_enabled', 'locked'].every(
          (k) => body[k] === undefined || typeof body[k] === 'boolean'
        ) &&
        (body.org_id === undefined || body.org_id === null || Number.isInteger(body.org_id))
      if (!typeOk) {
        await route.fulfill({
          status: 400,
          contentType: 'application/json',
          body: JSON.stringify({ status: 'error', code: 'VALIDATION_INVALID_INPUT', message: 'One or more fields have an invalid type' }),
        })
        return
      }
      Object.assign(userDetail, body)
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'User updated successfully' }),
      })
    } else if (route.request().method() === 'DELETE') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'User deleted successfully', tokens_revoked: true }),
      })
    } else {
      await route.continue()
    }
  })

  // Roles
  await page.route('**/api/admin/roles', async (route) => {    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', roles: MOCK_ROLES, total: MOCK_ROLES.length }),
      })
    } else if (route.request().method() === 'POST') {
      const body = JSON.parse(route.request().postData() || '{}')
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Role created successfully', id: 99, name: body.name, description: body.description || '' }),
      })
    } else {
      await route.continue()
    }
  })

  await page.route('**/api/admin/roles/*', async (route) => {
    if (route.request().method() === 'PUT') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Role updated successfully' }),
      })
    } else if (route.request().method() === 'DELETE') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Role deleted successfully' }),
      })
    } else {
      await route.continue()
    }
  })

  // Dashboard stats
  await page.route('**/api/admin/dashboard/stats', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(MOCK_DASHBOARD_STATS),
    })
  })

  // Assign roles (legacy - kept for backward compat)
  await page.route('**/api/admin/users/*/roles', async (route) => {
    await route.continue()
  })

  // Scopes
  await page.route('**/api/admin/scopes', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ scopes: MOCK_SCOPES }),
      })
    } else if (route.request().method() === 'POST') {
      const body = JSON.parse(route.request().postData() || '{}')
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Scope created successfully', id: 99, name: body.name }),
      })
    } else {
      await route.continue()
    }
  })

  await page.route('**/api/admin/scopes/*', async (route) => {
    if (route.request().method() === 'PUT') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Scope updated successfully' }),
      })
    } else if (route.request().method() === 'DELETE') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Scope deleted successfully' }),
      })
    } else {
      await route.continue()
    }
  })

  // Audit logs
  await page.route('**/api/admin/logs**', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ logs: MOCK_LOGS }),
    })
  })

  // Client detail (GET /api/admin/clients/:id)
  await page.route('**/api/admin/clients/*/scopes', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', scopes: ['openid', 'profile'] }),
      })
    } else if (route.request().method() === 'PUT') {
      const body = JSON.parse(route.request().postData() || '{}')
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Scopes updated', scopes: body.scopes || [] }),
      })
    } else {
      await route.continue()
    }
  })

  // Tokens list
  await page.route('**/api/admin/tokens/revoke-by-client', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ status: 'success', message: 'All tokens for client revoked', count: 3 }),
    })
  })

  await page.route('**/api/admin/tokens/revoke-by-user', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ status: 'success', message: 'All tokens for user revoked', count: 2 }),
    })
  })

  await page.route('**/api/admin/tokens/**', async (route) => {
    if (route.request().method() === 'DELETE') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'success', message: 'Token revoked' }),
      })
    } else {
      await route.continue()
    }
  })

  await page.route('**/api/admin/tokens**', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ tokens: MOCK_TOKENS, total: 3, page: 1, per_page: 50 }),
      })
    } else {
      await route.continue()
    }
  })

  // OIDC keys
  await page.route('**/api/admin/oidc/keys', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(MOCK_OIDC_KEYS),
    })
  })
}

/**
 * Perform login through the UI.
 */
export async function loginAsAdmin(page: Page) {
  await page.goto('/admin/login')
  await page.fill('input[type="text"]', 'admin')
  await page.fill('input[type="password"]', 'admin')
  await page.click('button[type="submit"]')
  // Wait for navigation to dashboard
  await page.waitForURL('**/admin/')
}

/**
 * Override a previously registered route with a custom handler.
 */
export async function overrideRoute(page: Page, urlPattern: string, handler: (route: any) => Promise<void>) {
  await page.route(urlPattern, handler)
}

/**
 * Mock a specific API returning an error status with Error Envelope body.
 */
export async function mockApiError(page: Page, urlPattern: string, status: number, body: object) {
  await page.route(urlPattern, async (route) => {
    await route.fulfill({
      status,
      contentType: 'application/json',
      body: JSON.stringify(body),
    })
  })
}

/**
 * Mock a network failure for a given URL pattern.
 */
export async function mockNetworkError(page: Page, urlPattern: string) {
  await page.route(urlPattern, async (route) => {
    await route.abort('failed')
  })
}
