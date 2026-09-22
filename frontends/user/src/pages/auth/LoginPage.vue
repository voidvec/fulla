<script setup lang="ts">
import { ref } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useAuthStore } from '../../stores/auth'
import { authService } from '../../services/authService'
import AppInput from '../../components/ui/AppInput.vue'
import AppButton from '../../components/ui/AppButton.vue'
import AppAlert from '../../components/ui/AppAlert.vue'

const auth = useAuthStore()
const router = useRouter()
const route = useRoute()
const { t } = useI18n()

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

const GITHUB_CLIENT_ID = import.meta.env.VITE_GITHUB_CLIENT_ID || ''
const githubAuthUrl = `https://github.com/login/oauth/authorize?client_id=${GITHUB_CLIENT_ID}&scope=user:email&redirect_uri=${encodeURIComponent(window.location.origin + '/callback/github')}`

// #70: Google login entry; rendered only when the deployment configured a
// client id (unconfigured providers stay hidden rather than broken).
const GOOGLE_CLIENT_ID = import.meta.env.VITE_GOOGLE_CLIENT_ID || ''
const googleAuthUrl = `https://accounts.google.com/o/oauth2/v2/auth?client_id=${GOOGLE_CLIENT_ID}&response_type=code&scope=openid%20email%20profile&redirect_uri=${encodeURIComponent(window.location.origin + '/callback/google')}`
const WECHAT_ENABLED = Boolean(import.meta.env.VITE_WECHAT_APPID)

// #145: the authorize gate sends flagged users here with the query flag set
// (302 from /oauth2/authorize) — show the change form without a login round-trip.
if (route.query.must_change_password === '1') {
  showPasswordChange.value = true
}

// U-1: the security page redirects here with pw_changed=1 after a successful
// self-service password change (the server revoked every session; a fresh
// login is required).
const passwordChangedNotice = route.query.pw_changed === '1'

// U-3 (browser-e2e 2026-09-08): /oauth2/authorize redirects anonymous users
// to /login with the OAuth parameters FLATTENED onto the query (client_id,
// redirect_uri, state, ... — there is no `redirect` param). After a successful
// login the authorization flow must RESUME: rebuild the authorize URL from
// those parameters (whitelisted keys only, values passed through verbatim —
// the server re-validates redirect_uri against the client registration) and
// navigate with a full page load. Before this, such users landed on the
// dashboard and the relying party never received its authorization code.
// v1.5.0 M1b: 'org_id' joins the whitelist — the backend already flattens
// the org context hint onto the /login URL (both authorize login-redirect
// branches), but dropping it here rebuilt the authorize request without the
// org context, silently degrading the browser chain to a no-org issuance
// while every CI-covered (API-driven) flow stayed green. The login submit
// is an XHR (no form POST navigation), so the whitelist IS the forwarding
// mechanism — the query survives on the page until the resume rebuild.
const AUTHORIZE_CARRY_KEYS = [
  'client_id', 'redirect_uri', 'scope', 'state', 'response_type',
  'code_challenge', 'code_challenge_method', 'nonce', 'org_id',
] as const

function resumeAuthorizeFlow(): boolean {
  const q = route.query
  const clientId = typeof q.client_id === 'string' ? q.client_id : ''
  const responseType = typeof q.response_type === 'string' ? q.response_type : ''
  if (!clientId || !responseType) return false
  const params = new URLSearchParams()
  for (const key of AUTHORIZE_CARRY_KEYS) {
    const v = q[key]
    if (typeof v === 'string' && v !== '') params.set(key, v)
  }
  window.location.href = `/oauth2/authorize?${params.toString()}`
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

async function handleMfa() {
  const result = await auth.verifyMfa(mfaToken.value, mfaCode.value)
  if (result.success) {
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
    </div>

    <AppAlert
      v-if="passwordChangedNotice"
      type="success"
      class="mb-6"
      data-testid="password-changed-notice"
    >
      {{ $t('auth.login.passwordChangedNotice') }}
    </AppAlert>

    <AppAlert
      v-if="auth.errorText"
      type="error"
      class="mb-6"
      dismissible
    >
      {{ auth.errorText }}
    </AppAlert>

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

      <!-- Social Login Divider -->
      <div
        v-if="GITHUB_CLIENT_ID || GOOGLE_CLIENT_ID"
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
      <a
        v-if="GITHUB_CLIENT_ID"
        :href="githubAuthUrl"
        class="w-full flex items-center justify-center gap-3 px-4 py-2.5 border border-neutral-300
               rounded-lg text-sm font-medium text-neutral-700 hover:bg-neutral-50 transition-colors"
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
      </a>

      <!-- Google Login (#70) -->
      <a
        v-if="GOOGLE_CLIENT_ID"
        :href="googleAuthUrl"
        class="w-full flex items-center justify-center gap-3 px-4 py-2.5 border border-neutral-300
               rounded-lg text-sm font-medium text-neutral-700 hover:bg-neutral-50 transition-colors"
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
      </a>

      <!-- WeChat (#70): QR-scan login requires a mobile browser agent; the
           desktop SPA can only surface the entry point when configured. -->
      <p
        v-if="WECHAT_ENABLED"
        class="mt-4 text-xs text-neutral-400 text-center"
      >
        {{ $t('auth.login.wechatNote') }}
      </p>
    </form>
  </div>
</template>
