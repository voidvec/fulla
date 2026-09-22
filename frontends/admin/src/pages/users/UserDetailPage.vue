<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppAlert from '../../components/ui/AppAlert.vue'
import AppBadge from '../../components/ui/AppBadge.vue'
import DData from '../../components/ui/DData.vue'

const { t } = useI18n()
const route = useRoute()
const userId = computed(() => route.params.id as string)

// Tab strip (labels follow the active locale).
const TABS = computed(() => [
  { key: 'info' as const, label: t('admin.users.tabs.info') },
  { key: 'security' as const, label: t('admin.users.tabs.security') },
  { key: 'roles' as const, label: t('admin.users.tabs.roles') },
])

const loading = ref(true)
const saving = ref(false)
const activeTab = ref<'info' | 'security' | 'roles'>('info')
const successMessage = ref('')
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen;
// plain strings (parameterized chrome copy via t()) keep snapshot semantics.
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

const user = ref<any>({})
const allRoles = ref<any[]>([])
const editUsername = ref('')
const editEmail = ref('')
const editEmailVerified = ref(false)
const editMfaEnabled = ref(false)
const editLocked = ref(false)
const selectedRoles = ref<string[]>([])

function showSuccess(msg: string) {
  successMessage.value = msg
  errorMessage.value = null
  setTimeout(() => { successMessage.value = '' }, 3000)
}
function showError(msg: NormalizedError | string) {
  errorMessage.value = msg
  successMessage.value = ''
  setTimeout(() => { errorMessage.value = null }, 5000)
}

async function fetchUser() {
  loading.value = true
  try {
    const resp = await axios.get(`/api/admin/users/${userId.value}`)
    user.value = resp.data
    editUsername.value = resp.data.username || ''
    editEmail.value = resp.data.email || ''
    editEmailVerified.value = resp.data.email_verified || false
    editMfaEnabled.value = resp.data.mfa_enabled || false
    editLocked.value = resp.data.locked || false
    selectedRoles.value = (resp.data.roles || []).filter((r: any) => typeof r === 'string')
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function fetchAllRoles() {
  try {
    const resp = await axios.get('/api/admin/roles')
    allRoles.value = resp.data.roles || []
  } catch {}
}

async function saveInfo() {
  saving.value = true
  try {
    const body: any = {}
    if (editUsername.value !== (user.value.username || '')) body.username = editUsername.value
    if (editEmail.value !== (user.value.email || '')) body.email = editEmail.value
    if (editEmailVerified.value !== user.value.email_verified) body.email_verified = editEmailVerified.value
    if (editMfaEnabled.value !== user.value.mfa_enabled) body.mfa_enabled = editMfaEnabled.value
    if (editLocked.value !== (user.value.locked || false)) body.locked = editLocked.value
    if (Object.keys(body).length === 0) { showSuccess(t('admin.users.noChanges')); saving.value = false; return }
    await axios.put(`/api/admin/users/${userId.value}`, body, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.users.userUpdated'))
    await fetchUser()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

async function saveRoles() {
  saving.value = true
  try {
    await axios.put(`/api/admin/users/${userId.value}/roles`, { roles: selectedRoles.value }, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.users.rolesUpdated'))
    // Refresh user data in background without clearing success message
    fetchUser().catch(() => {})
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

async function disableUser() {
  if (!confirm(t('admin.users.disableConfirm', { name: user.value.username }))) return
  try {
    await axios.put(`/api/admin/users/${userId.value}/disable`)
    showSuccess(t('admin.users.userDisabled'))
    await fetchUser()
  } catch (e: unknown) {
    showError(normalizeError(e))
  }
}

async function enableUser() {
  try {
    await axios.post(`/api/admin/users/${userId.value}/enable`)
    showSuccess(t('admin.users.userEnabled'))
    await fetchUser()
  } catch (e: unknown) {
    showError(normalizeError(e))
  }
}

const isLocked = computed(() => {
  const now = Math.floor(Date.now() / 1000)
  return (user.value.locked_until || 0) > now
})

onMounted(() => {
  fetchUser()
  fetchAllRoles()
})
</script>

<template>
  <div>
    <div class="flex items-center gap-3 mb-6">
      <router-link
        :to="{ name: 'users' }"
        class="text-neutral-500 hover:text-neutral-700 text-sm"
      >
        {{ $t('admin.users.backToList') }}
      </router-link>
    </div>

    <AppAlert
      v-if="successMessage"
      type="success"
      class="mb-4"
    >
      {{ successMessage }}
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

    <div v-else>
      <div class="flex justify-between items-start mb-6">
        <div>
          <h2 class="text-2xl font-bold text-neutral-900">
            {{ user.username }}
          </h2>
          <div class="mt-1.5">
            <DData
              :value="user.id"
              label="ID"
              truncate
            />
          </div>
        </div>
        <div class="flex gap-2">
          <button
            v-if="isLocked"
            class="px-3 py-2 bg-success-600 text-white rounded-ctl text-sm hover:bg-success-700
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            @click="enableUser"
          >
            {{ $t('admin.users.enableAccount') }}
          </button>
          <button
            v-else
            class="px-3 py-2 bg-error-600 text-white rounded-ctl text-sm hover:bg-error-700
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            @click="disableUser"
          >
            {{ $t('admin.users.disableAccount') }}
          </button>
        </div>
      </div>

      <!-- Three-pill status bar -->
      <div class="flex gap-2 mb-6">
        <AppBadge :variant="isLocked ? 'error' : 'success'">
          {{ isLocked ? $t('admin.users.locked') : $t('admin.users.active') }}
        </AppBadge>
        <AppBadge :variant="user.email_verified ? 'success' : 'warning'">
          {{ user.email_verified ? $t('admin.users.emailVerifiedBadge') : $t('admin.users.emailPendingBadge') }}
        </AppBadge>
        <AppBadge :variant="user.mfa_enabled ? 'success' : 'default'">
          {{ user.mfa_enabled ? $t('admin.users.mfaBadgeEnabled') : $t('admin.users.mfaBadgeOff') }}
        </AppBadge>
      </div>

      <!-- Tabs -->
      <div class="border-b border-neutral-200 mb-6">
        <nav class="-mb-px flex space-x-8">
          <button
            v-for="tab in TABS"
            :key="tab.key"
            :class="['py-3 px-1 border-b-2 text-sm font-medium transition-colors',
                     activeTab === tab.key ? 'border-brand-500 text-brand-600' : 'border-transparent text-neutral-500 hover:text-neutral-700']"
            @click="activeTab = tab.key"
          >
            {{ tab.label }}
          </button>
        </nav>
      </div>

      <!-- Info Tab -->
      <div
        v-if="activeTab === 'info'"
        class="bg-surface shadow rounded-lg p-6 space-y-5"
      >
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.username') }}</label>
          <input
            v-model="editUsername"
            class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm font-mono"
            placeholder="username"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.email') }}</label>
          <input
            v-model="editEmail"
            type="email"
            class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
            placeholder="user@example.com"
          >
        </div>
        <div class="flex items-center gap-3">
          <input
            id="emailVerified"
            v-model="editEmailVerified"
            type="checkbox"
            class="h-4 w-4 rounded border-neutral-300 text-brand-600"
          >
          <label
            for="emailVerified"
            class="text-sm font-medium text-neutral-700"
          >{{ $t('admin.users.emailVerifiedBadge') }}</label>
        </div>
        <div class="flex items-center gap-3">
          <input
            id="mfaEnabled"
            v-model="editMfaEnabled"
            type="checkbox"
            class="h-4 w-4 rounded border-neutral-300 text-brand-600"
          >
          <label
            for="mfaEnabled"
            class="text-sm font-medium text-neutral-700"
          >{{ $t('admin.users.mfaBadgeEnabled') }}</label>
        </div>
        <div class="flex items-center gap-3">
          <input
            id="locked"
            v-model="editLocked"
            type="checkbox"
            class="h-4 w-4 rounded border-neutral-300 text-error-600"
          >
          <label
            for="locked"
            class="text-sm font-medium text-neutral-700"
          >{{ $t('admin.users.accountLocked') }}</label>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.users.createdAt') }}</label>
          <p class="text-sm text-neutral-600">
            {{ user.created_at || '—' }}
          </p>
        </div>
        <div class="pt-4 border-t border-neutral-200">
          <button
            :disabled="saving"
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            @click="saveInfo"
          >
            {{ saving ? $t('common.saving') : $t('common.saveChanges') }}
          </button>
        </div>
      </div>

      <!-- Security Tab -->
      <div
        v-if="activeTab === 'security'"
        class="bg-surface shadow rounded-lg p-6 space-y-4"
      >
        <div class="grid grid-cols-2 gap-4">
          <div class="p-4 bg-neutral-50 rounded-lg">
            <p class="text-xs text-neutral-500 uppercase font-medium">
              {{ $t('admin.users.failedLoginCount') }}
            </p>
            <p
              class="text-2xl font-bold mt-1"
              :class="(user.failed_login_count || 0) > 0 ? 'text-error-600' : 'text-neutral-900'"
            >
              {{ user.failed_login_count || 0 }}
            </p>
          </div>
          <div class="p-4 bg-neutral-50 rounded-lg">
            <p class="text-xs text-neutral-500 uppercase font-medium">
              {{ $t('admin.users.accountStatus') }}
            </p>
            <p
              class="text-lg font-semibold mt-1"
              :class="isLocked ? 'text-error-600' : 'text-success-600'"
            >
              {{ isLocked ? $t('admin.users.locked') : $t('admin.users.active') }}
            </p>
          </div>
        </div>
        <div
          v-if="isLocked"
          class="p-4 bg-error-50 border border-error-200 rounded-lg"
        >
          <p class="text-sm text-error-700">
            {{ $t('admin.users.lockedUntil', { time: new Date((user.locked_until || 0) * 1000).toLocaleString() }) }}
          </p>
          <button
            class="mt-2 px-3 py-1.5 bg-error-600 text-white rounded text-sm hover:bg-error-700"
            @click="enableUser"
          >
            {{ $t('admin.users.unlockAccount') }}
          </button>
        </div>
        <div class="p-4 bg-neutral-50 rounded-lg">
          <p class="text-xs text-neutral-500 uppercase font-medium mb-1">
            {{ $t('admin.users.mfaStatus') }}
          </p>
          <p
            class="text-sm font-medium"
            :class="user.mfa_enabled ? 'text-brand-600' : 'text-neutral-500'"
          >
            {{ user.mfa_enabled ? $t('admin.users.mfaFullEnabled') : $t('admin.users.mfaNotConfigured') }}
          </p>
        </div>
      </div>

      <!-- Roles Tab -->
      <div
        v-if="activeTab === 'roles'"
        class="bg-surface shadow rounded-lg p-6"
      >
        <p class="text-sm text-neutral-600 mb-4">
          {{ $t('admin.users.rolesTabIntro') }}
        </p>
        <div
          v-if="allRoles.length === 0"
          class="text-neutral-500 text-sm"
        >
          {{ $t('admin.users.noRolesAvailable') }}
        </div>
        <div
          v-else
          class="space-y-2"
        >
          <label
            v-for="role in allRoles"
            :key="role.id"
            class="flex items-start gap-3 cursor-pointer p-3 rounded-lg hover:bg-neutral-50 border border-transparent hover:border-neutral-200"
          >
            <input
              v-model="selectedRoles"
              type="checkbox"
              :value="role.name"
              class="mt-0.5 h-4 w-4 rounded border-neutral-300 text-brand-600"
            >
            <div>
              <span class="text-sm font-medium text-neutral-700">{{ role.name }}</span>
              <p
                v-if="role.description"
                class="text-xs text-neutral-500"
              >{{ role.description }}</p>
              <p class="text-xs text-neutral-400">{{ $t('admin.users.userCount', { count: role.user_count }) }}</p>
            </div>
          </label>
        </div>
        <div class="mt-6 pt-4 border-t border-neutral-200">
          <button
            :disabled="saving"
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            @click="saveRoles"
          >
            {{ saving ? $t('common.saving') : $t('admin.users.saveRoles') }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
