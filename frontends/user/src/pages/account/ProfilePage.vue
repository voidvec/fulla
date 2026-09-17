<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAuthStore } from '../../stores/auth'
import http from '../../services/http'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppAlert from '../../components/ui/AppAlert.vue'
import AppBadge from '../../components/ui/AppBadge.vue'
import AppCard from '../../components/ui/AppCard.vue'
import DData from '../../components/ui/DData.vue'

const { t } = useI18n()
const auth = useAuthStore()
const profile = ref<any>(null)
const loading = ref(true)
const success = ref('')
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen;
// plain strings (chrome copy via t()) keep snapshot semantics.
const error = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = error.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

async function fetchProfile() {
  loading.value = true
  try {
    const resp = await http.get('/api/me')
    profile.value = resp.data
  } catch {
    error.value = t('account.profile.loadFailed')
  } finally {
    loading.value = false
  }
}

async function resendVerification() {
  try {
    await http.post('/api/verify-email/resend')
    success.value = t('account.profile.verificationSent')
    setTimeout(() => { success.value = '' }, 3000)
  } catch (e: unknown) {
    error.value = normalizeError(e)
  }
}

// v1.4.0 profile minimal set: editable display_name + avatar_url.
const editDisplayName = ref('')
const editAvatarUrl = ref('')
const savingProfile = ref(false)

function startProfileEdit() {
  editDisplayName.value = profile.value?.display_name || ''
  editAvatarUrl.value = profile.value?.avatar_url || ''
}

async function saveProfile() {
  savingProfile.value = true
  error.value = null
  try {
    await http.patch('/api/me/profile', {
      display_name: editDisplayName.value.trim(),
      avatar_url: editAvatarUrl.value.trim(),
    })
    success.value = t('account.profile.saved')
    setTimeout(() => { success.value = '' }, 3000)
    await fetchProfile()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  } finally {
    savingProfile.value = false
  }
}

onMounted(() => {
  startProfileEdit()
  fetchProfile()
})
</script>

<template>
  <div>
    <h1 class="text-2xl font-bold text-neutral-900 mb-6">
      {{ $t('nav.profile') }}
    </h1>

    <AppAlert
      v-if="success"
      type="success"
      class="mb-4"
    >
      {{ success }}
    </AppAlert>
    <AppAlert
      v-if="errorText"
      type="error"
      class="mb-4"
    >
      {{ errorText }}
    </AppAlert>

    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ $t('common.loading') }}
    </div>

    <AppCard
      v-else
      class="space-y-6"
    >
      <div class="grid grid-cols-1 md:grid-cols-2 gap-6">
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('common.username') }}</label>
          <p class="mt-1 text-lg font-medium text-neutral-900">
            {{ profile?.username || auth.user?.name }}
          </p>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.profile.accountIdSub') }}</label>
          <div class="mt-1">
            <DData
              :value="auth.user?.sub || '—'"
              truncate
            />
          </div>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('common.email') }}</label>
          <div class="flex items-center gap-2 mt-1">
            <p class="text-neutral-900">
              {{ profile?.email || 'N/A' }}
            </p>
            <AppBadge
              v-if="profile?.email_verified"
              variant="success"
              size="sm"
            >{{ $t('account.profile.verified') }}</AppBadge>
            <AppBadge
              v-else
              variant="warning"
              size="sm"
            >{{ $t('account.profile.unverified') }}</AppBadge>
          </div>
          <button
            v-if="profile?.email && !profile?.email_verified"
            class="mt-2 text-sm text-brand-600 hover:text-brand-800"
            @click="resendVerification"
          >
            {{ $t('account.profile.resendVerification') }}
          </button>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('common.roles') }}</label>
          <div class="flex flex-wrap gap-1.5 mt-1">
            <AppBadge
              v-for="role in (auth.user?.roles || [])"
              :key="role"
              variant="info"
              size="sm"
            >{{ role }}</AppBadge>
          </div>
        </div>
      </div>

      <div class="border-t border-neutral-100 pt-6">
        <h2 class="text-sm font-semibold text-neutral-900 mb-3">
          {{ $t('account.profile.editTitle') }}
        </h2>
        <form
          class="grid grid-cols-1 md:grid-cols-2 gap-4 items-end"
          @submit.prevent="saveProfile"
        >
          <div>
            <label class="block text-sm font-medium text-neutral-500">{{ $t('account.profile.displayName') }}</label>
            <input
              v-model="editDisplayName"
              maxlength="100"
              class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-500">{{ $t('account.profile.avatarUrl') }}</label>
            <input
              v-model="editAvatarUrl"
              placeholder="https://"
              class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            >
          </div>
          <div>
            <button
              type="submit"
              :disabled="savingProfile"
              class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                     disabled:opacity-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            >
              {{ $t('account.profile.save') }}
            </button>
          </div>
        </form>
      </div>
    </AppCard>
  </div>
</template>

