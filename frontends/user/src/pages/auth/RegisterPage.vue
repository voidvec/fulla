<script setup lang="ts">
import { ref, computed } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppAlert from '../../components/ui/AppAlert.vue'
import AppButton from '../../components/ui/AppButton.vue'
import AppInput from '../../components/ui/AppInput.vue'
import { passwordStrength } from '../../utils/passwordStrength'

const { t } = useI18n()
const username = ref('')
const email = ref('')
const password = ref('')
const confirmPassword = ref('')
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen;
// plain strings (chrome copy via t()) keep snapshot semantics.
const error = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = error.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})
const loading = ref(false)
const success = ref(false)

// Password strength meter (mockup 17): 4 segments, error -> warning ->
// success progression. Scoring lives in utils/passwordStrength.ts (unit
// tested; the scale reaches 4 so every segment can light).
const strength = computed(() => passwordStrength(password.value))

async function handleRegister() {
  error.value = null
  if (password.value !== confirmPassword.value) {
    error.value = t('common.passwordsDoNotMatch')
    return
  }
  if (password.value.length < 8) {
    error.value = t('common.passwordMinLength')
    return
  }
  loading.value = true
  try {
    await axios.post('/api/register', new URLSearchParams({
      username: username.value,
      password: password.value,
      email: email.value,
    }))
    // Stay on this page and tell the user to verify their email first — the
    // login leg rejects unverified accounts, so an auto-redirect used to land
    // them on a form that could only fail.
    success.value = true
  } catch (e: unknown) {
    error.value = normalizeError(e)
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div>
    <div class="mb-8">
      <h1 class="font-display text-2xl font-bold text-neutral-900 tracking-tight">
        {{ $t('auth.register.title') }}
      </h1>
      <p class="mt-2 text-sm text-neutral-500">
        {{ $t('auth.register.subtitle') }}
      </p>
    </div>

    <div
      v-if="success"
      class="text-center space-y-4 py-4"
    >
      <div class="w-16 h-16 bg-success-100 rounded-card flex items-center justify-center mx-auto">
        <svg
          class="w-8 h-8 text-success-600"
          viewBox="0 0 20 20"
          fill="currentColor"
          aria-hidden="true"
        >
          <path fill-rule="evenodd" d="M16.704 4.153a.75.75 0 01.143 1.052l-8 10.5a.75.75 0 01-1.127.075l-4.5-4.5a.75.75 0 011.06-1.06l3.894 3.893 7.48-9.817a.75.75 0 011.05-.143z" clip-rule="evenodd" />
        </svg>
      </div>
      <p class="text-neutral-700 font-medium">
        {{ $t('auth.register.success') }}
      </p>
      <p
        class="text-sm text-neutral-600 max-w-xs mx-auto"
        data-testid="verify-email-notice"
      >
        {{ $t('auth.register.verifyEmailNotice', { email: email }) }}
      </p>
      <router-link
        to="/login"
        class="inline-block text-sm text-brand-600 font-medium hover:text-brand-800"
        data-testid="go-to-login"
      >
        {{ $t('common.goToLogin') }}
      </router-link>
    </div>

    <AppAlert
      v-if="errorText"
      type="error"
      class="mb-4"
    >
      {{ errorText }}
    </AppAlert>

    <form
      v-if="!success"
      class="space-y-4"
      @submit.prevent="handleRegister"
    >
      <AppInput
        v-model="email"
        :label="$t('common.email')"
        type="email"
        required
        autocomplete="email"
        placeholder="you@example.com"
      />
      <AppInput
        v-model="username"
        :label="$t('common.username')"
        :hint="$t('auth.register.usernameHint')"
        autocomplete="username"
        placeholder="mia"
      />
      <div>
        <AppInput
          v-model="password"
          :label="$t('common.password')"
          type="password"
          required
          autocomplete="new-password"
          placeholder="••••••••"
        />
        <!-- Strength meter: 4 hairline segments (mockup .pw-meter) -->
        <div
          v-if="password"
          class="flex gap-1.5 mt-2"
          aria-hidden="true"
        >
          <span
            v-for="i in 4"
            :key="i"
            class="h-[3px] flex-1 rounded-full transition-colors duration-150"
            :class="i <= strength
              ? (strength <= 1 ? 'bg-error-500' : strength <= 2 ? 'bg-warning-500' : 'bg-success-500')
              : 'bg-neutral-200'"
          />
        </div>
        <p class="text-xs text-neutral-500 mt-1.5">
          {{ $t('auth.register.passwordHint') }}
        </p>
      </div>
      <AppInput
        v-model="confirmPassword"
        :label="$t('common.confirmPassword')"
        type="password"
        required
        autocomplete="new-password"
        placeholder="••••••••"
      />
      <AppButton
        type="submit"
        :loading="loading"
        block
      >
        {{ loading ? $t('auth.register.creating') : $t('auth.register.submit') }}
      </AppButton>
    </form>

    <p class="mt-6 text-center text-sm text-neutral-500">
      {{ $t('auth.register.haveAccount') }}
      <router-link
        to="/login"
        class="text-brand-600 font-medium hover:text-brand-800"
      >
        {{ $t('auth.register.signIn') }}
      </router-link>
    </p>
  </div>
</template>
