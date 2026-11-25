/**
 * Flattened authorize-request context on the SPA's routes.
 *
 * `/oauth2/authorize` sends a user who has to (re)authenticate to
 * `/login?client_id=...&redirect_uri=...&state=...&response_type=...`: the
 * OAuth parameters flattened onto the query (there is no `redirect` param).
 * Two places must agree on what "this URL carries an authorize request"
 * means — the login page, which rebuilds the authorize URL after a successful
 * login, and the router's guest guard, which has to let such a URL render.
 * They share this module so the two decisions cannot drift apart.
 */

/** Keys forwarded back to /oauth2/authorize (whitelist; values verbatim). */
export const AUTHORIZE_CARRY_KEYS = [
  'client_id', 'redirect_uri', 'scope', 'state', 'response_type',
  'code_challenge', 'code_challenge_method', 'nonce', 'org_id',
] as const

/** The single string value of a query key ('' when absent or repeated). */
function single(query: Record<string, unknown>, key: string): string {
  const value = query[key]
  return typeof value === 'string' ? value : ''
}

/**
 * True when the query carries an authorize request a login can resume. The
 * two keys the authorize URL cannot be rebuilt without are the client and the
 * response type; every other key is optional pass-through.
 */
export function isAuthorizeResumeRequest(query: Record<string, unknown>): boolean {
  return single(query, 'client_id') !== '' && single(query, 'response_type') !== ''
}

/**
 * Rebuild `/oauth2/authorize?...` from the flattened parameters. Only the
 * whitelisted keys are forwarded and their values are passed through verbatim
 * — the server re-validates redirect_uri against the client registration.
 * Returns '' when the query is not a resumable authorize request.
 */
export function buildAuthorizeResumeUrl(query: Record<string, unknown>): string {
  if (!isAuthorizeResumeRequest(query)) return ''
  const params = new URLSearchParams()
  for (const key of AUTHORIZE_CARRY_KEYS) {
    const value = single(query, key)
    if (value !== '') params.set(key, value)
  }
  return `/oauth2/authorize?${params.toString()}`
}
