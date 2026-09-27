export interface User {
  sub: string
  name: string
  email?: string
  email_verified?: boolean
  roles?: string[]
}

// #158: `error` carries the NormalizedError (not the resolved string) so the
// login banner re-translates on locale switch; plain strings keep snapshot
// semantics (chrome copy via t()).
export interface LoginResult {
  success?: boolean
  mfaRequired?: boolean
  mfaToken?: string
  passwordChangeRequired?: boolean
  error?: import('../services/errorAdapter').NormalizedError | string
}

export interface TokenResponse {
  access_token: string
  // Optional per the OAuth2/OIDC specs (and openapi.yaml): M2M flows may not
  // mint one. Writing an undefined value would persist the string "undefined".
  refresh_token?: string
  token_type: string
  expires_in: number
  id_token?: string
}

export interface AuthorizedApp {
  client_id: string
  name?: string
}

export interface UserProfile {
  username: string
  email: string
  email_verified: boolean
  mfa_enabled: boolean
  created_at?: string
}

export interface SocialLink {
  provider: string
  subject: string
  linked_at?: string
}

// v1.5.0 provider tiers: one entry of the GET /api/auth/providers discovery
// response. authorize_url is fully built server-side (redirect target from
// the per-provider redirect_uri override or frontend.url) — the login page
// redirects to it as-is.
export interface ExternalProviderInfo {
  provider: string
  authorize_url: string
}
