import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import axios from 'axios'

/**
 * AF-01 (P1-8 delegated administration): permission awareness for the admin
 * console.
 *
 * The backend enforces access on every request through three gates: the
 * token's scope, the role→permission rules (rbac_rules), and the literal
 * admin role for the catch-all. None of that is readable by a restricted
 * account — userinfo carries role names, and the role→permission mapping
 * lives under GET /api/admin/roles, which only admins may call. So instead
 * of keeping a frontend copy of the server's rule table (which would drift),
 * the console asks the server directly, right after the session is
 * established:
 *
 *   - Read probe per console domain: a GET against that domain's list
 *     endpoint. 200 ⇒ the menu entry renders; anything else ⇒ hidden.
 *   - Write probe (users domain only): POST /api/admin/users with an empty
 *     JSON object. The scope gate answers 403 before the request reaches
 *     the handler unless the token carries users:write; with it, the handler
 *     rejects the body at validation (400 "username is required") before any
 *     row is written or audited. 400 ⇒ write-capable, anything else ⇒
 *     read-only.
 *
 * What the probes can see is bounded by the token's scope. A delegated
 * account is only issued the resource scopes it holds AND that the console
 * client's allowlist permits (see CONSOLE_LOGIN_SCOPE in stores/auth.ts);
 * with no admin-class scope on the token every probe answers 403.
 *
 * The 'admin' role takes the fast path: every domain is visible and
 * writable, no probes are sent — the pre-AF-01 console behavior.
 *
 * The devices domain has no list endpoint to probe (its page POSTs
 * /oauth2/device/approve, an admin-role rule in the shipped config), so it
 * renders for the admin role only. Write gating is limited to the users
 * domain; other domains hide when unreadable but leave their write buttons
 * to the backend's 403.
 */

export interface ConsoleDomain {
  /** Store key + AdminLayout nav `domain` tag. */
  key: string
  /** Landing route for the domain. */
  path: string
  /** Side-effect-free GET against the domain's list endpoint; null = no
   * probe target (admin role only). */
  readProbe: string | null
  /** Validation-first request that answers 400 only when the write
   * permission is held. */
  writeProbe: { url: string; body: unknown } | null
}

export const CONSOLE_DOMAINS: ConsoleDomain[] = [
  { key: 'dashboard', path: '/', readProbe: '/api/admin/dashboard/stats', writeProbe: null },
  { key: 'users', path: '/users', readProbe: '/api/admin/users?page=1&per_page=1', writeProbe: { url: '/api/admin/users', body: {} } },
  { key: 'applications', path: '/applications', readProbe: '/api/admin/clients', writeProbe: null },
  { key: 'organizations', path: '/organizations', readProbe: '/api/admin/organizations', writeProbe: null },
  { key: 'roles', path: '/roles', readProbe: '/api/admin/roles', writeProbe: null },
  { key: 'scopes', path: '/scopes', readProbe: '/api/admin/scopes', writeProbe: null },
  { key: 'claim-mappings', path: '/claim-mappings', readProbe: '/api/admin/claim-mappings', writeProbe: null },
  { key: 'upstream-providers', path: '/upstream-providers', readProbe: '/api/admin/upstream-providers', writeProbe: null },
  { key: 'tokens', path: '/tokens', readProbe: '/api/admin/tokens?page=1&per_page=1', writeProbe: null },
  { key: 'devices', path: '/devices', readProbe: null, writeProbe: null },
  { key: 'logs', path: '/logs', readProbe: '/api/admin/logs?page=1&per_page=1', writeProbe: null },
  { key: 'email-templates', path: '/email-templates', readProbe: '/api/admin/email-templates', writeProbe: null },
  { key: 'settings', path: '/settings', readProbe: '/api/admin/oidc/keys', writeProbe: null },
]

/** 0 = the request never got a response (network failure, timeout). */
function statusOf(e: unknown): number {
  return (e as { response?: { status?: number } })?.response?.status ?? 0
}

/** Pure menu filtering over an access predicate — exported for unit tests.
 * A section with no visible item disappears entirely. */
export function filterNavSections<T extends { items: { domain: string }[] }>(
  sections: T[],
  canRead: (domain: string) => boolean,
): T[] {
  return sections
    .map((section) => ({ ...section, items: section.items.filter((item) => canRead(item.domain)) }))
    .filter((section) => section.items.length > 0)
}

export const usePermissionsStore = defineStore('permissions', () => {
  const isAdminRole = ref(false)
  const loaded = ref(false)
  const readAccess = ref<Record<string, boolean>>({})
  const writeAccess = ref<Record<string, boolean>>({})

  function canRead(domain: string): boolean {
    if (isAdminRole.value) return true
    return readAccess.value[domain] === true
  }

  function canWrite(domain: string): boolean {
    if (isAdminRole.value) return true
    return writeAccess.value[domain] === true
  }

  /** First readable domain's route in registry order — the post-login
   * landing page (a users-only delegate lands on /users, not on a dashboard
   * it cannot read). '/' for the admin role. */
  const landingPath = computed<string>(() => {
    if (isAdminRole.value) return '/'
    const first = CONSOLE_DOMAINS.find((d) => readAccess.value[d.key] === true)
    return first?.path ?? '/'
  })

  async function probeRead(url: string): Promise<boolean> {
    try {
      return (await axios.get(url)).status === 200
    } catch {
      return false
    }
  }

  async function probeWrite(probe: { url: string; body: unknown }): Promise<boolean> {
    try {
      await axios.post(probe.url, probe.body, { headers: { 'Content-Type': 'application/json' } })
      return true
    } catch (e: unknown) {
      return statusOf(e) === 400
    }
  }

  /** Admin fast path: no probes, everything visible and writable. */
  function adoptAdminRole() {
    isAdminRole.value = true
    loaded.value = true
  }

  /**
   * Delegated (non-admin) path: probe every domain. Returns true when at
   * least one domain is readable — the caller may enter the console; false
   * means no admin-surface access, and the auth store refuses the session.
   * Only runs for a non-admin session, once per login or page load.
   */
  async function probeDomains(): Promise<boolean> {
    isAdminRole.value = false
    const nextRead: Record<string, boolean> = {}
    const nextWrite: Record<string, boolean> = {}
    await Promise.all(
      CONSOLE_DOMAINS.map(async (domain) => {
        if (!domain.readProbe) {
          nextRead[domain.key] = false
          return
        }
        const readable = await probeRead(domain.readProbe)
        nextRead[domain.key] = readable
        if (readable && domain.writeProbe) {
          nextWrite[domain.key] = await probeWrite(domain.writeProbe)
        }
      }),
    )
    readAccess.value = nextRead
    writeAccess.value = nextWrite
    loaded.value = true
    return CONSOLE_DOMAINS.some((d) => nextRead[d.key])
  }

  /** Clear on logout — an access map must never survive into the next
   * login on this tab (delegate out, admin in). */
  function reset() {
    isAdminRole.value = false
    loaded.value = false
    readAccess.value = {}
    writeAccess.value = {}
  }

  return {
    isAdminRole,
    loaded,
    landingPath,
    canRead,
    canWrite,
    adoptAdminRole,
    probeDomains,
    reset,
  }
})
