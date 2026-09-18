<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useAuthStore } from '../../stores/auth'
import DData from '../../components/ui/DData.vue'

const { t } = useI18n()
const route = useRoute()
const auth = useAuthStore()
const loading = ref(false)

const clientId = route.query.client_id as string || ''
const scope = route.query.scope as string || ''
const redirectUri = route.query.redirect_uri as string || ''
const state = route.query.state as string || ''
// Gap-fix E7: the server's consent page URL explicitly carries the session
// user's id (AuthorizationEndpointController). Trust that value first — the
// store's userinfo may not have loaded yet (no auth meta on this route before
// the guard was added), and `sub` used to be silently submitted as ''.
const serverUserId = route.query.user_id as string || ''
const userId = serverUserId || auth.user?.sub || ''
// P0 #1: PKCE/nonce must round-trip through the consent page, otherwise the
// authorization code is issued without a stored code_challenge and the token
// endpoint's PKCE verification is silently skipped.
const codeChallenge = route.query.code_challenge as string || ''
// F1: server-minted CSRF nonce from the authorize->consent redirect; the
// consent POST must echo it (one-shot, TTL-bounded server-side).
const consentCsrf = route.query.consent_csrf as string || ''
const codeChallengeMethod = route.query.code_challenge_method as string || ''
const nonce = route.query.nonce as string || ''
// v1.4.0 open platform: who offers this app (org name for org apps, the
// creator's display name for personal apps) — carried on the authorize ->
// consent redirect so users can tell self-registered apps from official ones.
const ownerName = route.query.owner_name as string || ''

const scopes = scope.split(' ').filter(Boolean)

// Neither the server-provided nor the store user id is known — submitting
// would 500 server-side ("failed to get user mapping"). Block the action
// instead of firing a request that cannot succeed.
const missingUserId = !userId

// #43: resource-scope vocabulary. The legacy bare 'read'/'write' labels are
// dropped; the OIDC standard scopes keep their human-readable descriptions and
// the resource-prefixed admin scopes are listed for completeness (they
// normally appear only for admin-console clients, not end users). The map is
// a computed so descriptions follow locale switches.
const scopeDescriptions = computed<Record<string, string>>(() => ({
  openid: t('oauth.scopes.openid'),
  profile: t('oauth.scopes.profile'),
  email: t('oauth.scopes.email'),
  admin: t('oauth.scopes.admin'),
  'users:read': t('oauth.scopes.users:read'),
  'users:write': t('oauth.scopes.users:write'),
  'clients:read': t('oauth.scopes.clients:read'),
  'clients:write': t('oauth.scopes.clients:write'),
  'tokens:read': t('oauth.scopes.tokens:read'),
  'tokens:write': t('oauth.scopes.tokens:write'),
  'roles:read': t('oauth.scopes.roles:read'),
  'roles:write': t('oauth.scopes.roles:write'),
  'audit:read': t('oauth.scopes.audit:read'),
}))

/**
 * Submit consent via a native form POST (not XHR).
 *
 * The backend returns a real HTTP 302 redirect to the client's redirect_uri
 * with ?code=... (F-020). An XHR/fetch would auto-follow this redirect and
 * land on an opaque cross-origin response, preventing the browser from
 * navigating. A native form POST lets the browser handle the 302 directly,
 * correctly redirecting the top-level window to the client application.
 * This is the standard browser pattern for OAuth2 consent submission.
 */
function handleConsent(action: 'approve' | 'deny') {
  loading.value = true
  const form = document.createElement('form')
  form.method = 'POST'
  form.action = '/oauth2/consent'
  form.enctype = 'application/x-www-form-urlencoded'

  const fields: Record<string, string> = {
    client_id: clientId,
    user_id: userId,
    scope,
    redirect_uri: redirectUri,
    state,
    action,
  }
  if (consentCsrf) {
    fields.consent_csrf = consentCsrf
  }
  if (codeChallenge) {
    fields.code_challenge = codeChallenge
    fields.code_challenge_method = codeChallengeMethod
  }
  if (nonce) fields.nonce = nonce

  for (const [key, value] of Object.entries(fields)) {
    const input = document.createElement('input')
    input.type = 'hidden'
    input.name = key
    input.value = value
    form.appendChild(input)
  }
  document.body.appendChild(form)
  form.submit()
}
</script>

<template>
  <div>
    <h1 class="font-display text-[25px] font-bold text-neutral-900 tracking-tight leading-tight">
      {{ $t('oauth.consent.title') }}
    </h1>
    <p class="mt-1.5 text-sm text-neutral-500">
      {{ $t('oauth.consent.subtitle') }}
    </p>

    <!-- Client identity block -->
    <div class="flex items-center gap-3.5 mt-6 px-4 py-3.5 bg-page border border-neutral-200 rounded-card">
      <div class="w-[38px] h-[38px] rounded-[9px] bg-brand-100 text-brand-700 flex items-center justify-center font-bold text-[15px] shrink-0">
        {{ clientId.slice(0, 2).toUpperCase() || 'CL' }}
      </div>
      <div class="min-w-0">
        <div class="font-bold text-[15.5px] leading-tight text-neutral-900 truncate">
          {{ clientId || $t('oauth.consent.unknownClient') }}
        </div>
        <DData
          :value="clientId.length > 10 ? clientId.slice(0, 10) + '…' : clientId"
          label="client_id"
          class="mt-1"
        />
        <!-- Owner attribution (v1.4.0): shown only for self-registered apps -->
        <p
          v-if="ownerName"
          class="mt-0.5 text-xs text-neutral-500"
          data-testid="consent-owner-name"
        >
          {{ $t('oauth.consent.providedBy', { owner: ownerName }) }}
        </p>
      </div>
    </div>

    <!-- Requested permissions: human voice left, machine voice right -->
    <div class="mt-7">
      <p class="text-[10.5px] font-semibold tracking-[0.09em] uppercase text-neutral-500 mb-1">
        {{ $t('oauth.consent.permissionsHeading') }}
      </p>
      <div
        v-for="s in scopes"
        :key="s"
        class="flex items-center gap-3 py-3 border-b border-neutral-100"
      >
        <span class="w-[21px] h-[21px] rounded-full bg-success-50 border border-success-200 text-success-600 flex items-center justify-center shrink-0">
          <svg
            class="w-[11px] h-[11px]"
            viewBox="0 0 12 12"
            fill="none"
            aria-hidden="true"
          >
            <path
              d="M2.5 6.2 5 8.7l4.5-5.4"
              stroke="currentColor"
              stroke-width="1.8"
              stroke-linecap="round"
              stroke-linejoin="round"
            />
          </svg>
        </span>
        <span class="flex-1 text-sm text-neutral-600">{{ scopeDescriptions[s] || s }}</span>
        <DData :value="s" />
      </div>
    </div>

    <!-- Blocking state: no usable user id (gap-fix E7 companion) -->
    <p
      v-if="missingUserId"
      class="mt-4 text-center text-sm text-error-600"
      data-testid="consent-missing-user"
    >
      {{ $t('oauth.consent.missingUser') }}
    </p>

    <!-- Actions: Deny (quiet) / Authorize (primary stamp) -->
    <div class="flex gap-2.5 mt-7">
      <button
        :disabled="loading || missingUserId"
        class="flex-1 py-3 border border-neutral-300 text-neutral-700 text-sm font-medium rounded-ctl hover:bg-neutral-50
               focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring
               disabled:opacity-50 disabled:cursor-not-allowed transition-colors"
        @click="handleConsent('deny')"
      >
        {{ $t('oauth.consent.deny') }}
      </button>
      <button
        :disabled="loading || missingUserId"
        class="consent-authorize flex-1 py-3 bg-brand-600 text-white text-sm font-medium rounded-ctl hover:bg-brand-700 shadow-sm
               focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring
               disabled:opacity-50 disabled:cursor-not-allowed transition-all duration-150 active:scale-[0.98]"
        :class="loading ? 'stamp-pulse' : ''"
        @click="handleConsent('approve')"
      >
        {{ loading ? $t('oauth.consent.authorizing') : $t('oauth.consent.authorize') }}
      </button>
    </div>

    <!-- Session microcopy (mono) -->
    <p class="mt-7 pt-4 border-t border-neutral-100 font-mono text-[11.5px] text-neutral-500 text-center">
      {{ $t('oauth.consent.signedInAs', { name: auth.user?.name || auth.user?.sub || '—' }) }}
    </p>
  </div>
</template>

<style scoped>
/* Stamp micro-interaction: one 400ms brand-ring pulse at the moment of
   authorization — the class mounts when loading flips true on approve.
   Static under prefers-reduced-motion. */
.stamp-pulse {
  animation: stamp-pulse 400ms var(--ease-spring, ease-out);
}

@keyframes stamp-pulse {
  0%   { box-shadow: 0 0 0 0 oklch(46% 0.150 252 / 0.45); }
  100% { box-shadow: 0 0 0 14px oklch(46% 0.150 252 / 0); }
}

@media (prefers-reduced-motion: reduce) {
  .stamp-pulse {
    animation: none;
  }
}
</style>
