<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useAuthStore } from '../../stores/auth'
import { beginSocialLogin, SocialFlowError } from '../../services/socialLogin'
import type { SocialProvider } from '../../services/socialFlowContext'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import { authService, PORTAL_REDIRECT_URI } from '../../services/authService'
import { buildAuthorizeResumeUrl, isAuthorizeResumeRequest } from '../../services/authorizeResume'
import {
  beginPasskeyAuthentication,
  finishPasskeyAuthentication,
  toPublicKeyRequestOptions,
  assertionToFinishPayload,
} from '../../services/webauthnService'
import type { ExternalProviderInfo } from '../../types'
import AppInput from '../../components/ui/AppInput.vue'
import AppButton from '../../components/ui/AppButton.vue'
import AppAlert from '../../components/ui/AppAlert.vue'

const auth = useAuthStore()
const router = useRouter()
const route = useRoute()
const { t } = useI18n()

// When login fails because the portal's redirect_uri is
// not registered for the client, show the concrete value we sent — the bare
// catalog message tells the integrator WHAT failed, this tells them WHAT to
// whitelist.
const redirectUriHint = computed(() => {
  const e = auth.error
  return e && typeof e === 'object' && e.code === 'VALIDATION_CLIENT_REDIRECT_URI_NOT_REGISTERED'
    ? PORTAL_REDIRECT_URI
    : ''
})

// P0-6 (#260): the application this login is being performed for (the
// authorize flow flattens client_id onto the query when it redirects here).
const clientId = (route.query.client_id as string) || ''

const username = ref('')
const password = ref('')
const mfaCode = ref('')
const mfaToken = ref('')
const showMfa = ref(false)

// #145: forced first-login password change (account flagged
// must_change_password — the backend refuses to issue tokens until the
// password is replaced on the login session).
const showPasswordChange = ref(false)
const oldPassword = ref('')
const newPassword = ref('')
const confirmPassword = ref('')
const passwordChangeError = ref('')
const passwordChangeBusy = ref(false)
const passwordChangeDone = ref(false)

// v1.4.0 provider tiers: the social buttons render from the server's runtime
// discovery (GET /api/auth/providers) instead of build-time VITE_* env vars —
// the same gate (tier switch + credentials) that enforces the login endpoints
// decides what this page offers, so buttons and server behavior can never
// disagree. Discovery's deprecated URL is empty; clicks issue a transaction.
// Fetch failure
// hides the whole section: a degraded password-only login beats buttons that
// cannot complete a login.
const externalProviders = ref<ExternalProviderInfo[]>([])

// P0-6 (#260): display name of the app that initiated this authorization
// (client_id only resumes the flow — the user should see WHO is asking).
const clientName = ref('')
const githubEnabled = computed(() => externalProviders.value.some((p) => p.provider === 'github'))
const googleEnabled = computed(() => externalProviders.value.some((p) => p.provider === 'google'))
const wechatEnabled = computed(() => externalProviders.value.some((p) => p.provider === 'wechat'))
const socialBusy = ref(false)
const socialError = ref('')
async function startSocialLogin(provider: SocialProvider) {
  if (socialBusy.value) return
  socialBusy.value = true
  socialError.value = ''
  try {
    window.location.assign(await beginSocialLogin(provider))
  } catch (failure) {
    socialError.value = failure instanceof SocialFlowError
      ? t(failure.reason === 'cookies' ? 'oauth.cookiesRequired' : 'oauth.invalidFlow')
      : normalizeError(failure).message
  } finally {
    socialBusy.value = false
  }
}

onMounted(async () => {
  try {
    externalProviders.value = await authService.listExternalProviders()
  } catch {
    // Discovery unreachable (e.g. backend down in a mocked dev run): keep
    // the section hidden instead of rendering dead buttons.
  }
  // Branding lookup degrades to the raw client_id on any failure.
  if (clientId) clientName.value = await authService.getAuthorizeContext(clientId)
})

// #145: the authorize gate sends flagged users here with the query flag set
// (302 from /oauth2/authorize) — show the change form without a login round-trip.
if (route.query.must_change_password === '1') {
  showPasswordChange.value = true
}

// U-1: the security page redirects here with pw_changed=1 after a successful
// self-service password change (the server revoked every session; a fresh
// login is required).
const passwordChangedNotice = route.query.pw_changed === '1'

// Second WeKnora report: reaching /login with an authorize request while the
// portal still holds a refresh token means the IdP's SSO session has ended
// (or the relying party asked for a fresh authentication: prompt=login, a
// short max_age). The two are separate lifetimes (session-management.md §1),
// so the user signs in once more; say why, instead of showing a silent login
// form to someone who is, by the portal's own state, already signed in.
const showReauthNotice = computed(
  () => auth.isAuthenticated && isAuthorizeResumeRequest(route.query),
)

// U-3 (browser-e2e 2026-09-08): /oauth2/authorize redirects anonymous users
// to /login with the OAuth parameters FLATTENED onto the query (client_id,
// redirect_uri, state, ... — there is no `redirect` param). After a successful
// login the authorization flow must RESUME: rebuild the authorize URL from
// those parameters and navigate with a full page load. Before this, such
// users landed on the dashboard and the relying party never received its
// authorization code.
// v1.4.0 M1b: 'org_id' is carried too — the backend already flattens the org
// context hint onto the /login URL (both authorize login-redirect branches),
// but dropping it here rebuilt the authorize request without the org context,
// silently degrading the browser chain to a no-org issuance while every
// CI-covered (API-driven) flow stayed green. The login submit is an XHR (no
// form POST navigation), so the whitelist IS the forwarding mechanism — the
// query survives on the page until the resume rebuild.
// The whitelist and the "is this an authorize request" decision live in
// services/authorizeResume: the router's guest guard relies on the same
// decision to let this page render.
function resumeAuthorizeFlow(): boolean {
  const url = buildAuthorizeResumeUrl(route.query)
  if (!url) return false
  window.location.href = url
  return true
}

function navigateAfterLogin() {
  const redirect = route.query.redirect
  if (typeof redirect === 'string' && redirect) {
    router.push(redirect)
    return
  }
  if (resumeAuthorizeFlow()) return
  router.push('/')
}

async function handleLogin() {
  const result = await auth.login(username.value, password.value)
  if (result.mfaRequired) {
    mfaToken.value = result.mfaToken!
    showMfa.value = true
  } else if (result.passwordChangeRequired) {
    oldPassword.value = password.value
    password.value = ''
    showPasswordChange.value = true
  } else if (result.success) {
    navigateAfterLogin()
  }
}

// UF-05: the login was refused because the account's email address is not
// verified (auth.require_email_verification). /oauth2/login answers exactly
// that one case with 403 AUTHZ_ACCESS_DENIED, and the frontend only keeps the
// code (not the server's message), so the code is the discriminator. The
// catalog copy for it is a bare "Access denied"; this offers the way out —
// a new verification link, requested without a session.
const emailNotVerified = computed(
  () => !!auth.error && typeof auth.error === 'object' && auth.error.code === 'AUTHZ_ACCESS_DENIED',
)
const verifyEmail = ref('')
const resending = ref(false)
const verificationSent = ref(false)
// #158: a catalog-backed failure is kept as NormalizedError so the text
// follows a locale switch.
const resendError = ref<NormalizedError | string | null>(null)
const resendErrorText = computed(() => {
  const e = resendError.value
  return !e ? '' : typeof e === 'string' ? e : getErrorMessage(e.code)
})

// The sign-in field takes a username as well, so carry the typed value over
// only when it is an address; start the panel clean of any earlier outcome.
watch(emailNotVerified, (denied) => {
  if (!denied) return
  verificationSent.value = false
  resendError.value = null
  if (username.value.includes('@')) verifyEmail.value = username.value
})

async function handleResendVerification() {
  if (resending.value) return
  resending.value = true
  resendError.value = null
  try {
    await authService.resendVerificationByEmail(verifyEmail.value.trim())
    verificationSent.value = true
  } catch (e: unknown) {
    verificationSent.value = false
    resendError.value = normalizeError(e)
  } finally {
    resending.value = false
  }
}

// UF-01: passkey login entry. The finish endpoint establishes the browser
// session (cookie) but no tokens, so a plain portal login completes through
// the SPA's own authorize round-trip; an in-flight authorize request resumes
// through the same rebuild as the password path. Every server failure is the
// generic 401 AUTH_INVALID_CREDENTIALS (anti-enumeration — the catalog copy
// covers it); DOMExceptions carry no axios response and get local copy
// (SecurityPage M-3 pattern).
const webauthnSupported = typeof window !== 'undefined' && !!window.PublicKeyCredential
const passkeyBusy = ref(false)
const passkeyError = ref('')

async function handlePasskeyLogin() {
  if (passkeyBusy.value) return
  passkeyBusy.value = true
  passkeyError.value = ''
  try {
    const options = toPublicKeyRequestOptions(await beginPasskeyAuthentication())
    const credential = await navigator.credentials.get({ publicKey: options }) as PublicKeyCredential | null
    if (!credential) throw new DOMException('The operation was aborted', 'NotAllowedError')
    await finishPasskeyAuthentication(assertionToFinishPayload(credential))
    if (resumeAuthorizeFlow()) return
    window.location.href = await authService.buildOwnAuthorizeUrl()
  } catch (e: unknown) {
    const name = (e as { name?: string })?.name
    if (name === 'NotAllowedError') {
      passkeyError.value = t('auth.login.passkey.timedOut')
    } else if (!name || !(e as { response?: unknown }).response) {
      // WebAuthn DOMExceptions and local throws carry no axios response;
      // normalizeError would misreport them as network failures.
      passkeyError.value = t('auth.login.passkey.failed')
    } else {
      passkeyError.value = normalizeError(e).message
    }
  } finally {
    passkeyBusy.value = false
  }
}

async function handleMfa() {
  const result = await auth.verifyMfa(mfaToken.value, mfaCode.value)
  if (result.success) {
    // P0-3 (#257): backup codes exhausted — flag it so the security page (the
    // login flow itself redirects away) shows a forced re-bind banner.
    if (result.backupCodesRemaining === 0) {
      localStorage.setItem('fulla_mfa_backup_exhausted', '1')
    }
    navigateAfterLogin()
  }
}

async function handlePasswordChange() {
  passwordChangeError.value = ''
  if (newPassword.value !== confirmPassword.value) {
    passwordChangeError.value = t('auth.login.passwordChange.mismatch')
    return
  }
  passwordChangeBusy.value = true
  try {
    await authService.changePasswordForced(oldPassword.value, newPassword.value)
    passwordChangeDone.value = true
  } catch (e) {
    passwordChangeError.value = e instanceof Error ? e.message : t('auth.login.passwordChange.failed')
  } finally {
    passwordChangeBusy.value = false
  }
}
</script>

<template>
  <div>
    <!-- Header -->
    <div class="mb-8">
      <h1 class="text-2xl font-bold text-neutral-900 tracking-tight">
        {{ $t('auth.login.title') }}
      </h1>
      <p class="mt-2 text-sm text-neutral-500">
        {{ $t('auth.login.or') }}
        <router-link
          to="/register"
          class="text-brand-700 font-medium hover:text-brand-700 transition-colors"
        >
          {{ $t('auth.login.createAccountLink') }}
        </router-link>
      </p>
      <!-- P0-6 (#260): which application is this login for -->
      <p
        v-if="clientId"
        class="mt-3 text-sm font-medium text-brand-700"
        data-testid="login-client-name"
      >
        {{ $t('auth.login.continueTo', { app: clientName || clientId }) }}
      </p>
    </div>

    <AppAlert
      v-if="passwordChangedNotice"
      type="success"
      class="mb-6"
      data-testid="password-changed-notice"
    >
      {{ $t('auth.login.passwordChangedNotice') }}
    </AppAlert>

    <!-- Second WeKnora report: the portal session is alive but the IdP's SSO
         session is not (or the RP asked for a fresh authentication) — explain
         why a signed-in user is asked to sign in again. -->
    <AppAlert
      v-if="showReauthNotice"
      type="info"
      class="mb-6"
      data-testid="login-reauth-notice"
    >
      {{ $t('auth.login.reauthNotice') }}
    </AppAlert>

    <AppAlert
      v-if="auth.errorText"
      type="error"
      class="mb-6"
      dismissible
    >
      {{ auth.errorText }}
      <!-- The concrete redirect_uri this SPA sent — show it
           under the catalog message so the operator can whitelist exactly it. -->
      <p
        v-if="redirectUriHint"
        class="mt-1.5 font-mono text-[11.5px] opacity-80 break-all"
        data-testid="login-redirect-uri-hint"
      >
        {{ $t('auth.login.redirectUriHint') }}: {{ redirectUriHint }}
      </p>
    </AppAlert>

    <!-- UF-05: sign-in refused because the email address is not verified —
         request a new verification link (no session needed). The
         confirmation is deliberately the same whatever address was entered:
         the server does not reveal whether an account exists for it. -->
    <div
      v-if="emailNotVerified"
      class="mb-6 rounded-lg border border-warning-200 bg-warning-50 p-4"
      data-testid="verify-email-panel"
    >
      <p class="font-medium text-neutral-900">
        {{ $t('auth.login.verifyEmail.title') }}
      </p>
      <p class="mt-1 text-sm text-neutral-600">
        {{ $t('auth.login.verifyEmail.body') }}
      </p>
      <div
        v-if="verificationSent"
        class="mt-3 rounded-lg bg-success-50 border border-success-200 p-3 text-sm text-success-800"
        data-testid="verify-email-sent"
      >
        {{ $t('auth.login.verifyEmail.sent') }}
      </div>
      <form
        v-else
        class="mt-3 space-y-3"
        data-testid="verify-email-form"
        @submit.prevent="handleResendVerification"
      >
        <AppInput
          v-model="verifyEmail"
          :label="$t('auth.login.verifyEmail.emailLabel')"
          type="email"
          autocomplete="email"
          required
        />
        <AppAlert
          v-if="resendErrorText"
          type="error"
          data-testid="verify-email-error"
        >
          {{ resendErrorText }}
        </AppAlert>
        <AppButton
          type="submit"
          :loading="resending"
          :disabled="!verifyEmail.trim()"
          block
        >
          {{ $t('auth.login.verifyEmail.submit') }}
        </AppButton>
      </form>
    </div>

    <!-- #145: Forced Password Change -->
    <form
      v-if="showPasswordChange"
      class="space-y-5"
      @submit.prevent="handlePasswordChange"
    >
      <div class="text-center py-4">
        <h2 class="text-lg font-semibold text-neutral-900">
          {{ $t('auth.login.passwordChange.title') }}
        </h2>
        <p class="text-sm text-neutral-500 mt-1">
          {{ $t('auth.login.passwordChange.subtitle') }}
        </p>
      </div>

      <div
        v-if="passwordChangeDone"
        class="rounded-lg bg-success-50 border border-success-200 p-4 text-sm text-success-800"
        data-testid="forced-password-change-done"
      >
        {{ $t('auth.login.passwordChange.done') }}
      </div>
      <template v-else>
        <AppAlert
          v-if="passwordChangeError"
          type="error"
          class="mb-2"
        >
          {{ passwordChangeError }}
        </AppAlert>
        <AppInput
          v-model="oldPassword"
          :label="$t('auth.login.passwordChange.oldLabel')"
          type="password"
          :placeholder="$t('auth.login.passwordChange.oldPlaceholder')"
          required
          autocomplete="current-password"
        />
        <AppInput
          v-model="newPassword"
          :label="$t('auth.login.passwordChange.newLabel')"
          type="password"
          :placeholder="$t('auth.login.passwordChange.newPlaceholder')"
          required
          autocomplete="new-password"
        />
        <AppInput
          v-model="confirmPassword"
          :label="$t('auth.login.passwordChange.confirmLabel')"
          type="password"
          :placeholder="$t('auth.login.passwordChange.confirmPlaceholder')"
          required
          autocomplete="new-password"
        />
        <AppButton
          type="submit"
          :loading="passwordChangeBusy"
          :disabled="!oldPassword || !newPassword || !confirmPassword"
          block
        >
          {{ $t('auth.login.passwordChange.submit') }}
        </AppButton>
      </template>

      <button
        type="button"
        class="w-full text-sm text-neutral-500 hover:text-neutral-700 transition-colors"
        @click="showPasswordChange = false; passwordChangeDone = false"
      >
        Back to sign in
      </button>
    </form>

    <!-- MFA Challenge -->
    <form
      v-else-if="showMfa"
      class="space-y-6"
      @submit.prevent="handleMfa"
    >
      <div class="text-center py-4">
        <div class="w-14 h-14 rounded-2xl bg-brand-50 flex items-center justify-center mx-auto mb-4">
          <svg
            class="w-7 h-7 text-brand-700"
            viewBox="0 0 20 20"
            fill="currentColor"
          >
            <path
              fill-rule="evenodd"
              d="M10 2a4 4 0 00-4 4v4H4a2 2 0 00-2 2v4a2 2 0 002 2h12a2 2 0 002-2v-4a2 2 0 00-2-2h-2V6a4 4 0 00-4-4zm1.5 6.5a1 1 0 11-2 0 1 1 0 012 0zM4 14h12v4H4v-4z"
            />
          </svg>
        </div>
        <h2 class="text-lg font-semibold text-neutral-900">
          {{ $t('auth.login.mfa.title') }}
        </h2>
        <p class="text-sm text-neutral-500 mt-1">
          {{ $t('auth.login.mfa.subtitle') }}
        </p>
      </div>
      <div>
        <input
          v-model="mfaCode"
          type="text"
          inputmode="numeric"
          maxlength="6"
          autocomplete="one-time-code"
          :aria-label="$t('auth.login.mfa.codeLabel')"
          class="block w-full px-4 py-4 text-center text-2xl tracking-[0.42em] font-mono tabular-nums border border-neutral-300 rounded-ctl bg-surface
                 focus:outline-none focus-visible:ring-[3px] focus-visible:ring-ring focus:border-brand-700"
          placeholder="000000"
        >
      </div>
      <AppButton
        type="submit"
        :loading="auth.loading"
        :disabled="mfaCode.length !== 6"
        block
      >
        {{ $t('auth.login.mfa.verify') }}
      </AppButton>
      <button
        type="button"
        class="w-full text-sm text-neutral-500 hover:text-neutral-700 transition-colors"
        @click="showMfa = false; mfaCode = ''"
      >
        {{ $t('auth.login.mfa.back') }}
      </button>
    </form>

    <!-- Login Form -->
    <form
      v-else
      class="space-y-5"
      @submit.prevent="handleLogin"
    >
      <AppInput
        v-model="username"
        :label="$t('auth.login.emailOrUsername')"
        placeholder="you@example.com"
        required
        autocomplete="username"
      />

      <AppInput
        v-model="password"
        :label="$t('common.password')"
        type="password"
        :placeholder="$t('auth.login.passwordPlaceholder')"
        required
        autocomplete="current-password"
      />

      <div class="flex justify-end">
        <router-link
          to="/forgot-password"
          class="text-sm text-brand-700 hover:text-brand-800 font-medium transition-colors"
        >
          {{ $t('auth.login.forgotPassword') }}
        </router-link>
      </div>

      <AppButton
        type="submit"
        :loading="auth.loading"
        block
      >
        {{ $t('auth.login.submit') }}
      </AppButton>

      <!-- UF-01: passkey login entry — offered only when the browser
           supports WebAuthn; a degraded password-only login beats a button
           that cannot complete (same rule as the social section). -->
      <button
        v-if="webauthnSupported"
        type="button"
        :disabled="passkeyBusy"
        data-testid="login-passkey-button"
        class="w-full flex items-center justify-center gap-2 px-4 py-2.5 border border-neutral-300
               rounded-lg text-sm font-medium text-neutral-700 hover:bg-neutral-50 transition-colors
               disabled:opacity-50"
        @click="handlePasskeyLogin"
      >
        <svg
          class="w-4 h-4"
          viewBox="0 0 20 20"
          fill="currentColor"
          aria-hidden="true"
        >
          <path
            fill-rule="evenodd"
            d="M10 1a4.5 4.5 0 00-4.5 4.5V8H5a2 2 0 00-2 2v6a2 2 0 002 2h10a2 2 0 002-2v-6a2 2 0 00-2-2h-.5V5.5A4.5 4.5 0 0010 1zm3 7V5.5a3 3 0 10-6 0V8h6zm-3 4a1 1 0 100-2 1 1 0 000 2z"
            clip-rule="evenodd"
          />
        </svg>
        {{ passkeyBusy ? $t('auth.login.passkey.busy') : $t('auth.login.passkey.button') }}
      </button>

      <AppAlert
        v-if="passkeyError"
        type="error"
        class="mt-4"
        data-testid="login-passkey-error"
      >
        {{ passkeyError }}
      </AppAlert>

      <AppAlert
        v-if="socialError"
        type="error"
        class="mt-4"
      >
        {{ socialError }}
      </AppAlert>

      <!-- Social Login Divider -->
      <div
        v-if="githubEnabled || googleEnabled || wechatEnabled"
        class="relative my-6"
      >
        <div class="absolute inset-0 flex items-center">
          <div class="w-full border-t border-neutral-200" />
        </div>
        <div class="relative flex justify-center text-sm">
          <span class="px-3 bg-surface text-neutral-400">{{ $t('auth.login.orContinueWith') }}</span>
        </div>
      </div>

      <!-- GitHub Login -->
      <button
        v-if="githubEnabled"
        type="button"
        :disabled="socialBusy"
        class="w-full flex items-center justify-center gap-3 px-4 py-2.5 border border-neutral-300
               rounded-lg text-sm font-medium text-neutral-700 hover:bg-neutral-50 transition-colors"
        @click="startSocialLogin('github')"
      >
        <svg
          class="w-5 h-5"
          viewBox="0 0 24 24"
          fill="currentColor"
          aria-hidden="true"
        >
          <path d="M12 0C5.37 0 0 5.37 0 12c0 5.31 3.435 9.795 8.205 11.385.6.105.825-.255.825-.57 0-.285-.015-1.23-.015-2.235-3.015.555-3.795-.735-4.035-1.41-.135-.345-.72-1.41-1.23-1.695-.42-.225-1.02-.78-.015-.795.945-.015 1.62.87 1.845 1.23 1.08 1.815 2.805 1.305 3.495.99.105-.78.42-1.305.765-1.605-2.67-.3-5.46-1.335-5.46-5.925 0-1.305.465-2.385 1.23-3.225-.12-.3-.54-1.53.12-3.18 0 0 1.005-.315 3.3 1.23.96-.27 1.98-.405 3-.405s2.04.135 3 .405c2.295-1.56 3.3-1.23 3.3-1.23.66 1.65.24 2.88.12 3.18.765.84 1.23 1.905 1.23 3.225 0 4.605-2.805 5.625-5.475 5.925.435.375.81 1.095.81 2.22 0 1.605-.015 2.895-.015 3.3 0 .315.225.69.825.57A12.02 12.02 0 0024 12c0-6.63-5.37-12-12-12z" />
        </svg>
        {{ $t('auth.login.signInWithGitHub') }}
      </button>

      <!-- Google Login (#70) -->
      <button
        v-if="googleEnabled"
        type="button"
        :disabled="socialBusy"
        class="w-full flex items-center justify-center gap-3 px-4 py-2.5 border border-neutral-300
               rounded-lg text-sm font-medium text-neutral-700 hover:bg-neutral-50 transition-colors"
        @click="startSocialLogin('google')"
      >
        <svg
          class="w-5 h-5"
          viewBox="0 0 24 24"
          aria-hidden="true"
        >
          <path
            fill="#4285F4"
            d="M23.49 12.27c0-.79-.07-1.54-.19-2.27H12v4.51h6.47a5.57 5.57 0 0 1-2.4 3.58v3h3.86c2.26-2.09 3.56-5.17 3.56-8.82z"
          />
          <path
            fill="#34A853"
            d="M12 24c3.24 0 5.95-1.08 7.93-2.91l-3.86-3c-1.08.72-2.45 1.16-4.07 1.16-3.13 0-5.78-2.11-6.73-4.96H1.29v3.09A11.99 11.99 0 0 0 12 24z"
          />
          <path
            fill="#FBBC05"
            d="M5.27 14.29A7.2 7.2 0 0 1 4.89 12c0-.8.14-1.57.38-2.29V6.62H1.29a12 12 0 0 0 0 10.76l3.98-3.09z"
          />
          <path
            fill="#EA4335"
            d="M12 4.75c1.77 0 3.35.61 4.6 1.8l3.42-3.42A11.97 11.97 0 0 0 12 0 11.99 11.99 0 0 0 1.29 6.62l3.98 3.09C6.22 6.86 8.87 4.75 12 4.75z"
          />
        </svg>
        {{ $t('auth.login.signInWithGoogle') }}
      </button>

      <!-- WeChat (#70): QR-scan login requires a mobile browser agent; the
           desktop SPA can only surface the entry point when the deployment
           offers WeChat (domestic tier). -->
      <p
        v-if="wechatEnabled"
        class="mt-4 text-xs text-neutral-400 text-center"
      >
        {{ $t('auth.login.wechatNote') }}
      </p>
    </form>
  </div>
</template>
