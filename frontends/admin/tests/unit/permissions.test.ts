/**
 * Unit tests for the admin console's permission awareness (AF-01).
 *
 * The store learns what the signed-in account may see by probing the admin
 * surface, so these tests wire a mocked backend and pin the decision for
 * each account shape: the admin fast path, a read-only delegate, a
 * read-write delegate, and an account with no admin-surface access.
 */
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import {
  CONSOLE_DOMAINS,
  filterNavSections,
  usePermissionsStore,
} from '../../src/stores/permissions'

// vi.mock is hoisted above every import, so the mock handles have to be
// created in a hoisted block as well.
const http = vi.hoisted(() => ({ get: vi.fn(), post: vi.fn() }))
vi.mock('axios', () => ({ default: http }))

function httpError(status: number) {
  return { response: { status } }
}

/**
 * Wire the mocked backend. Exactly the `readable` domains answer 200 on
 * their list endpoint. Among those, the `writable` domains let the
 * validation-first write probe reach the handler (400); every other
 * request is refused with 403.
 */
function mockBackend(readable: Set<string>, writable: Set<string> = new Set()) {
  http.get.mockImplementation(async (url: string) => {
    const domain = CONSOLE_DOMAINS.find((d) => d.readProbe === url)
    if (domain && readable.has(domain.key)) return { status: 200, data: {} }
    throw httpError(403)
  })
  http.post.mockImplementation(async (url: string) => {
    const domain = CONSOLE_DOMAINS.find((d) => d.writeProbe?.url === url)
    if (domain && writable.has(domain.key)) throw httpError(400)
    throw httpError(403)
  })
}

beforeEach(() => {
  setActivePinia(createPinia())
  http.get.mockReset()
  http.post.mockReset()
})

describe('console domain registry', () => {
  it('has unique keys and landing paths', () => {
    const keys = CONSOLE_DOMAINS.map((d) => d.key)
    const paths = CONSOLE_DOMAINS.map((d) => d.path)
    expect(new Set(keys).size).toBe(keys.length)
    expect(new Set(paths).size).toBe(paths.length)
  })

  it('probes every readable domain with a GET under the admin surface', () => {
    for (const domain of CONSOLE_DOMAINS) {
      if (domain.readProbe === null) continue
      expect(domain.readProbe.startsWith('/api/admin/')).toBe(true)
      expect(domain.readProbe).not.toContain(' ')
    }
  })
})

describe('usePermissionsStore — admin fast path', () => {
  it('makes every domain visible and writable without probing', async () => {
    const store = usePermissionsStore()
    store.adoptAdminRole()

    expect(store.isAdminRole).toBe(true)
    expect(store.landingPath).toBe('/')
    for (const domain of CONSOLE_DOMAINS) {
      expect(store.canRead(domain.key)).toBe(true)
      expect(store.canWrite(domain.key)).toBe(true)
    }
    expect(http.get).not.toHaveBeenCalled()
    expect(http.post).not.toHaveBeenCalled()
  })
})

describe('usePermissionsStore — delegated accounts', () => {
  it('a users:read-only delegate sees only the users domain, read-only', async () => {
    mockBackend(new Set(['users']))
    const store = usePermissionsStore()

    await expect(store.probeDomains()).resolves.toBe(true)

    expect(store.isAdminRole).toBe(false)
    expect(store.canRead('users')).toBe(true)
    expect(store.canRead('dashboard')).toBe(false)
    expect(store.canRead('tokens')).toBe(false)
    // devices has no probe target: never readable without the admin role.
    expect(store.canRead('devices')).toBe(false)
    expect(store.canWrite('users')).toBe(false)
    expect(store.landingPath).toBe('/users')
  })

  it('a delegate holding users:read and users:write may write users', async () => {
    mockBackend(new Set(['users']), new Set(['users']))
    const store = usePermissionsStore()

    await store.probeDomains()

    expect(store.canRead('users')).toBe(true)
    expect(store.canWrite('users')).toBe(true)
    // Write access never leaks into a domain that has no write probe.
    expect(store.canWrite('dashboard')).toBe(false)
  })

  it('lands on the first readable domain in registry order', async () => {
    mockBackend(new Set(['tokens', 'logs']))
    const store = usePermissionsStore()

    await store.probeDomains()

    expect(store.landingPath).toBe('/tokens')
  })

  it('refuses the session when no domain is readable', async () => {
    mockBackend(new Set())
    const store = usePermissionsStore()

    await expect(store.probeDomains()).resolves.toBe(false)
    expect(store.landingPath).toBe('/')
    for (const domain of CONSOLE_DOMAINS) {
      expect(store.canRead(domain.key)).toBe(false)
    }
  })

  it('treats a probe that never got a response as not readable', async () => {
    http.get.mockImplementation(async () => {
      throw new Error('Network Error')
    })
    const store = usePermissionsStore()

    await expect(store.probeDomains()).resolves.toBe(false)
  })

  it('sends the write probe only for a readable domain, as an empty JSON object', async () => {
    mockBackend(new Set(['users']))
    const store = usePermissionsStore()
    await store.probeDomains()

    expect(http.post).toHaveBeenCalledTimes(1)
    const [url, body] = http.post.mock.calls[0]
    expect(url).toBe('/api/admin/users')
    expect(body).toEqual({})
  })

  it('does not send a write probe for a domain it cannot read', async () => {
    mockBackend(new Set())
    const store = usePermissionsStore()
    await store.probeDomains()

    expect(http.post).not.toHaveBeenCalled()
  })
})

describe('usePermissionsStore — reset', () => {
  it('drops a previous access map so it cannot leak into the next login', async () => {
    mockBackend(new Set(['users']), new Set(['users']))
    const store = usePermissionsStore()
    await store.probeDomains()
    expect(store.canRead('users')).toBe(true)

    store.reset()

    expect(store.loaded).toBe(false)
    expect(store.canRead('users')).toBe(false)
    expect(store.canWrite('users')).toBe(false)
  })

  it('clears the admin fast path too', () => {
    const store = usePermissionsStore()
    store.adoptAdminRole()
    store.reset()

    expect(store.isAdminRole).toBe(false)
    expect(store.canRead('users')).toBe(false)
  })
})

describe('filterNavSections', () => {
  const sections = [
    { label: 'Overview', items: [{ name: 'Dashboard', domain: 'dashboard', path: '/' }] },
    {
      label: 'Management',
      items: [
        { name: 'Users', domain: 'users', path: '/users' },
        { name: 'Tokens', domain: 'tokens', path: '/tokens' },
      ],
    },
    { label: 'Monitoring', items: [{ name: 'Logs', domain: 'logs', path: '/logs' }] },
  ]

  it('keeps every item for the admin role', () => {
    const visible = filterNavSections(sections, () => true)
    expect(visible).toHaveLength(3)
    expect(visible[1].items.map((i) => i.domain)).toEqual(['users', 'tokens'])
  })

  it('drops unreadable items and sections left without any', () => {
    const visible = filterNavSections(sections, (domain) => domain === 'users')
    expect(visible.map((s) => s.label)).toEqual(['Management'])
    expect(visible[0].items.map((i) => i.domain)).toEqual(['users'])
  })

  it('does not mutate the input sections', () => {
    filterNavSections(sections, (domain) => domain === 'users')
    expect(sections[1].items).toHaveLength(2)
  })
})
