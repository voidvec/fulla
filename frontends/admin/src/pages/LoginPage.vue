<script setup lang="ts">
import { ref } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import AppInput from '../components/ui/AppInput.vue'
import AppAlert from '../components/ui/AppAlert.vue'
import AppLogo from '../components/shared/AppLogo.vue'
import LocaleSwitcher from '../components/ui/LocaleSwitcher.vue'
import { useAuthStore } from '../stores/auth'
import { usePermissionsStore } from '../stores/permissions'

const auth = useAuthStore()
const permissions = usePermissionsStore()
const router = useRouter()
const route = useRoute()
const { t } = useI18n()

// Issuer verification line (mockup 06): hidden when not configured.
const issuer = (import.meta.env.VITE_ISSUER as string | undefined) || ''

const username = ref('')
const password = ref('')
const loading = ref(false)

// MFA challenge state (gap-fix E2): /oauth2/login answers mfa_required with a
// short-lived mfa_token; the 6-digit TOTP code completes the login via
// auth.verifyMfa. On failure auth.loginError is set and the user stays here.
const showMfa = ref(false)
const mfaToken = ref('')
const mfaCode = ref('')
const mfaLoading = ref(false)

// #145: forced first-login password change (e.g. the bootstrap admin whose
// initial password was printed to the server log). The backend refuses to
// issue tokens while the account is flagged; the password is replaced on the
// login session and the administrator signs in again.
const showPasswordChange = ref(false)
const oldPassword = ref('')
const newPassword = ref('')
const confirmPassword = ref('')
const passwordChangeBusy = ref(false)
const passwordChangeDone = ref(false)

// PR #157 review MAJOR 4: the server's must-change redirect sends flagged
// admin-console users here with the query flag set (302 from
// /oauth2/authorize); show the change form without a login round-trip.
if (route.query.must_change_password === '1') {
  showPasswordChange.value = true
}

async function handleLogin() {
  loading.value = true
  try {
    const result = await auth.login(username.value, password.value)
    if (result?.mfaRequired) {
      mfaToken.value = result.mfaToken!
      showMfa.value = true
    } else if (result?.passwordChangeRequired) {
      oldPassword.value = password.value
      password.value = ''
      showPasswordChange.value = true
    } else if (!result?.error) {
      // AF-01: a delegated account (e.g. users-only) lands on its first
      // readable domain instead of the admin-role dashboard.
      router.push(permissions.landingPath)
    }
  } finally {
    loading.value = false
  }
}

async function handleMfa() {
  mfaLoading.value = true
  try {
    const result = await auth.verifyMfa(mfaToken.value, mfaCode.value)
    if (!result?.error) {
      router.push(permissions.landingPath)
    }
  } finally {
    mfaLoading.value = false
  }
}

async function handlePasswordChange() {
  if (newPassword.value !== confirmPassword.value) {
    auth.loginError = t('login.passwordChange.mismatch')
    return
  }
  passwordChangeBusy.value = true
  try {
    await auth.changePasswordForced(oldPassword.value, newPassword.value)
    passwordChangeDone.value = true
    auth.loginError = null
  } catch {
    // loginError is set by the store; shown in the banner above.
  } finally {
    passwordChangeBusy.value = false
  }
}

function backToLogin() {
  showMfa.value = false
  mfaCode.value = ''
  mfaToken.value = ''
  showPasswordChange.value = false
  passwordChangeDone.value = false
  auth.loginError = null
}
</script>

<template>
  <div class="min-h-screen flex flex-col items-center bg-page blueprint-grid p-4">
    <!-- Machined top blade -->
    <div class="fixed inset-x-0 top-0 h-[3px] bg-brand-600 z-10" />

    <!-- Logo + admin chip -->
    <div class="flex items-center gap-2.5 mb-7 mt-12">
      <AppLogo size="lg" />
      <span class="font-mono text-[11.5px] leading-none px-2 py-[5px] rounded-ctl bg-brand-50 border border-brand-200 text-brand-700 font-semibold">admin</span>
    </div>

    <!-- Card -->
    <div class="w-full max-w-[440px] bg-surface rounded-auth border border-neutral-200 shadow-md p-9 pb-[30px]">
      <h1 class="font-display text-[23px] leading-tight font-semibold text-neutral-900 tracking-tight">
        {{ $t('login.title') }}
      </h1>
      <p class="mt-1.5 mb-6 text-sm text-neutral-500">
        {{ $t('login.subtitle') }}
      </p>
      <AppAlert
        v-if="auth.loginErrorText"
        type="error"
        class="mb-6"
        dismissible
        @dismiss="auth.loginError = null"
      >
        {{ auth.loginErrorText }}
      </AppAlert>

      <!-- #145: forced first-login password change -->
      <form
        v-if="showPasswordChange"
        class="space-y-5"
        @submit.prevent="handlePasswordChange"
      >
        <div>
          <h2 class="text-base font-semibold text-neutral-900">
            {{ $t('login.passwordChange.title') }}
          </h2>
          <p class="mt-1 text-sm text-neutral-500">
            {{ $t('login.passwordChange.subtitle') }}
          </p>
        </div>

        <div
          v-if="passwordChangeDone"
          class="rounded-ctl border border-success-200 bg-success-50 p-4 text-sm text-success-800"
          data-testid="forced-password-change-done"
        >
          {{ $t('login.passwordChange.done') }}
        </div>
        <template v-else>
          <div class="space-y-1.5">
            <label
              for="old-password-field"
              class="block text-sm font-medium text-neutral-700"
            >
              {{ $t('login.passwordChange.oldLabel') }}
            </label>
            <input
              id="old-password-field"
              v-model="oldPassword"
              type="password"
              :placeholder="$t('login.passwordChange.oldPlaceholder')"
              required
              autocomplete="current-password"
              class="block w-full px-3.5 py-2.5 text-sm rounded-ctl border border-neutral-300 bg-surface
                     placeholder:text-neutral-400 transition-colors duration-150
                     focus:outline-none focus-visible:ring-[3px] focus-visible:ring-ring focus:border-brand-700"
            >
          </div>
          <div class="space-y-1.5">
            <label
              for="new-password-field"
              class="block text-sm font-medium text-neutral-700"
            >
              {{ $t('login.passwordChange.newLabel') }}
            </label>
            <input
              id="new-password-field"
              v-model="newPassword"
              type="password"
              :placeholder="$t('login.passwordChange.newPlaceholder')"
              required
              autocomplete="new-password"
              class="block w-full px-3.5 py-2.5 text-sm rounded-ctl border border-neutral-300 bg-surface
                     placeholder:text-neutral-400 transition-colors duration-150
                     focus:outline-none focus-visible:ring-[3px] focus-visible:ring-ring focus:border-brand-700"
            >
          </div>
          <div class="space-y-1.5">
            <label
              for="confirm-password-field"
              class="block text-sm font-medium text-neutral-700"
            >
              {{ $t('login.passwordChange.confirmLabel') }}
            </label>
            <input
              id="confirm-password-field"
              v-model="confirmPassword"
              type="password"
              :placeholder="$t('login.passwordChange.confirmPlaceholder')"
              required
              autocomplete="new-password"
              class="block w-full px-3.5 py-2.5 text-sm rounded-ctl border border-neutral-300 bg-surface
                     placeholder:text-neutral-400 transition-colors duration-150
                     focus:outline-none focus-visible:ring-[3px] focus-visible:ring-ring focus:border-brand-700"
            >
          </div>

          <button
            type="submit"
            :disabled="passwordChangeBusy || !oldPassword || !newPassword || !confirmPassword"
            class="w-full inline-flex items-center justify-center gap-2 px-4 py-2.5 text-sm font-medium
                   bg-brand-600 text-white rounded-ctl hover:bg-brand-700 shadow-sm
                   disabled:opacity-50 disabled:cursor-not-allowed
                   transition-all duration-150 active:scale-[0.98]
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
            {{ passwordChangeBusy ? $t('login.passwordChange.submitting') : $t('login.passwordChange.submit') }}
          </button>
        </template>

        <button
          type="button"
          class="w-full text-center text-xs text-neutral-500 hover:text-neutral-700 transition-colors
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring rounded-ctl"
          @click="backToLogin"
        >
          {{ $t('login.passwordChange.back') }}
        </button>
      </form>

      <form
        v-else-if="!showMfa"
        class="space-y-5"
        @submit.prevent="handleLogin"
      >
        <AppInput
          v-model="username"
          :label="$t('login.username')"
          placeholder="admin"
          required
          autocomplete="username"
        />

        <div class="space-y-1.5">
          <div class="flex items-center justify-between">
            <label
              for="password-field"
              class="block text-sm font-medium text-neutral-700"
            >
              {{ $t('login.password') }}
            </label>
          </div>
          <input
            id="password-field"
            v-model="password"
            type="password"
            :placeholder="$t('login.passwordPlaceholder')"
            required
            autocomplete="current-password"
            class="block w-full px-3.5 py-2.5 text-sm rounded-ctl border border-neutral-300 bg-surface
                     placeholder:text-neutral-400 transition-colors duration-150
                     focus:outline-none focus-visible:ring-[3px] focus-visible:ring-ring focus:border-brand-700"
          >
        </div>

        <button
          type="submit"
          :disabled="loading"
          class="w-full inline-flex items-center justify-center gap-2 px-4 py-2.5 text-sm font-medium
                   bg-brand-600 text-white rounded-ctl hover:bg-brand-700 shadow-sm
                   disabled:opacity-50 disabled:cursor-not-allowed
                   transition-all duration-150 active:scale-[0.98]
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        >
          <svg
            v-if="loading"
            class="animate-spin w-4 h-4"
            viewBox="0 0 24 24"
            fill="none"
            aria-hidden="true"
          >
            <circle
              class="opacity-25"
              cx="12"
              cy="12"
              r="10"
              stroke="currentColor"
              stroke-width="4"
            />
            <path
              class="opacity-75"
              fill="currentColor"
              d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"
            />
          </svg>
          {{ loading ? $t('login.signingIn') : $t('login.submit') }}
        </button>
      </form>

      <!-- MFA challenge: complete the login with the 6-digit TOTP code -->
      <form
        v-else
        class="space-y-5"
        @submit.prevent="handleMfa"
      >
        <div>
          <h2 class="text-base font-semibold text-neutral-900">
            {{ $t('login.mfa.title') }}
          </h2>
          <p class="mt-1 text-sm text-neutral-500">
            {{ $t('login.mfa.subtitle') }}
          </p>
        </div>

        <div class="space-y-1.5">
          <label
            for="mfa-code-field"
            class="block text-sm font-medium text-neutral-700"
          >
            {{ $t('login.mfa.codeLabel') }}
          </label>
          <input
            id="mfa-code-field"
            v-model="mfaCode"
            type="text"
            placeholder="000000"
            required
            inputmode="numeric"
            autocomplete="one-time-code"
            maxlength="6"
            class="block w-full px-3.5 py-2.5 text-sm tracking-[0.4em] text-center font-mono tabular-nums rounded-ctl border border-neutral-300 bg-surface
                     placeholder:text-neutral-400 placeholder:tracking-normal placeholder:font-sans transition-colors duration-150
                     focus:outline-none focus-visible:ring-[3px] focus-visible:ring-ring focus:border-brand-700"
          >
        </div>

        <button
          type="submit"
          :disabled="mfaLoading || mfaCode.length !== 6"
          class="w-full inline-flex items-center justify-center gap-2 px-4 py-2.5 text-sm font-medium
                   bg-brand-600 text-white rounded-ctl hover:bg-brand-700 shadow-sm
                   disabled:opacity-50 disabled:cursor-not-allowed
                   transition-all duration-150 active:scale-[0.98]
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        >
          <svg
            v-if="mfaLoading"
            class="animate-spin w-4 h-4"
            viewBox="0 0 24 24"
            fill="none"
            aria-hidden="true"
          >
            <circle
              class="opacity-25"
              cx="12"
              cy="12"
              r="10"
              stroke="currentColor"
              stroke-width="4"
            />
            <path
              class="opacity-75"
              fill="currentColor"
              d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"
            />
          </svg>
          {{ mfaLoading ? $t('login.mfa.verifying') : $t('login.mfa.verify') }}
        </button>

        <button
          type="button"
          class="w-full text-center text-xs text-neutral-500 hover:text-neutral-700 transition-colors
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring rounded-ctl"
          @click="backToLogin"
        >
          {{ $t('login.mfa.back') }}
        </button>
      </form>
    </div>

    <!-- Issuer verification line (hidden when unconfigured) -->
    <p
      v-if="issuer"
      class="mt-6 font-mono text-xs text-neutral-500 tabular-nums"
    >
      <span class="font-semibold text-neutral-600">issuer</span> &middot; {{ issuer }}
    </p>

    <p class="mt-2.5 text-center text-xs text-neutral-400">
      {{ $t('login.footer') }}
    </p>

    <!-- Locale switcher (card footer area) -->
    <div class="mt-4 flex items-center justify-center">
      <LocaleSwitcher />
    </div>
  </div>
</template>
