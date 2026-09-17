<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import http from '../../services/http'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppAlert from '../../components/ui/AppAlert.vue'
import AppBadge from '../../components/ui/AppBadge.vue'
import AppCard from '../../components/ui/AppCard.vue'
import AppEmptyState from '../../components/ui/AppEmptyState.vue'
import DData from '../../components/ui/DData.vue'

const { t } = useI18n()
const apps = ref<any[]>([])
const loading = ref(true)
const error = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = error.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})
const success = ref('')
const showCreate = ref(false)
const creating = ref(false)
// Shown EXACTLY once after create/rotate (backend contract).
const oneTimeSecret = ref('')
const oneTimeSecretFor = ref('')
const newName = ref('')
const newType = ref<'PUBLIC' | 'CONFIDENTIAL'>('PUBLIC')
const newRedirectUris = ref('')
const newScopes = ref('openid profile')

async function fetchApps() {
  loading.value = true
  try {
    const resp = await http.get('/api/me/applications')
    apps.value = resp.data?.applications || []
  } catch {
    error.value = t('account.applications.loadFailed')
  } finally {
    loading.value = false
  }
}

async function createApp() {
  creating.value = true
  error.value = null
  try {
    const uris = newRedirectUris.value.split(/[\s,]+/).filter(Boolean)
    const scopes = newScopes.value.split(/[\s,]+/).filter(Boolean)
    const body: any = {
      name: newName.value,
      client_type: newType.value,
    }
    if (uris.length) body.redirect_uris = uris
    if (scopes.length) body.scopes = scopes
    const resp = await http.post('/api/me/applications', body)
    oneTimeSecret.value = resp.data?.client_secret || ''
    oneTimeSecretFor.value = resp.data?.client_id || ''
    showCreate.value = false
    newName.value = ''
    success.value = t('account.applications.created')
    setTimeout(() => { success.value = '' }, 3000)
    await fetchApps()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  } finally {
    creating.value = false
  }
}

async function rotateSecret(app: any) {
  if (!confirm(t('account.applications.rotateConfirm', { app: app.name || app.client_id }))) return
  try {
    const resp = await http.post(`/api/me/applications/${app.client_id}/rotate-secret`, {})
    oneTimeSecret.value = resp.data?.client_secret || ''
    oneTimeSecretFor.value = app.client_id
    success.value = t('account.applications.rotated', { app: app.name || app.client_id })
    setTimeout(() => { success.value = '' }, 3000)
  } catch (e: unknown) {
    error.value = normalizeError(e)
  }
}

async function deleteApp(app: any) {
  if (!confirm(t('account.applications.deleteConfirm', { app: app.name || app.client_id }))) return
  try {
    await http.delete(`/api/me/applications/${app.client_id}`)
    success.value = t('account.applications.deleted', { app: app.name || app.client_id })
    setTimeout(() => { success.value = '' }, 3000)
    await fetchApps()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  }
}

onMounted(fetchApps)
</script>

<template>
  <div>
    <div class="flex items-center justify-between mb-6">
      <h1 class="text-2xl font-bold text-neutral-900">
        {{ $t('account.applications.title') }}
      </h1>
      <button
        class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700 transition-colors
               focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        @click="showCreate = !showCreate"
      >
        {{ $t('account.applications.create') }}
      </button>
    </div>
    <p class="text-neutral-500 mb-6">
      {{ $t('account.applications.intro') }}
    </p>

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

    <AppCard
      v-if="oneTimeSecret"
      class="mb-6 border-2 border-warning-300"
    >
      <p class="font-medium text-neutral-900 mb-1">
        {{ $t('account.applications.secretTitle', { id: oneTimeSecretFor }) }}
      </p>
      <p class="text-sm text-neutral-500 mb-2">
        {{ $t('account.applications.secretOnce') }}
      </p>
      <DData :value="oneTimeSecret" />
    </AppCard>

    <AppCard
      v-if="showCreate"
      class="mb-6"
    >
      <form
        class="space-y-4"
        @submit.prevent="createApp"
      >
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.name') }}</label>
          <input
            v-model="newName"
            required
            maxlength="100"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.clientType') }}</label>
          <select
            v-model="newType"
            class="mt-1 rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
            <option value="PUBLIC">PUBLIC</option>
            <option value="CONFIDENTIAL">CONFIDENTIAL</option>
          </select>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.redirectUris') }}</label>
          <input
            v-model="newRedirectUris"
            :placeholder="$t('account.applications.redirectUrisPlaceholder')"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.scopes') }}</label>
          <input
            v-model="newScopes"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <button
          type="submit"
          :disabled="creating || !newName"
          class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                 disabled:opacity-50 transition-colors
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        >
          {{ $t('account.applications.submit') }}
        </button>
      </form>
    </AppCard>

    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ $t('common.loading') }}
    </div>

    <AppCard
      v-else-if="apps.length === 0"
      padding="none"
    >
      <AppEmptyState
        :title="$t('account.applications.emptyTitle')"
        :description="$t('account.applications.emptyDesc')"
      />
    </AppCard>

    <div
      v-else
      class="space-y-3"
    >
      <AppCard
        v-for="app in apps"
        :key="app.client_id"
        padding="sm"
      >
        <div class="flex items-center justify-between">
          <div>
            <p class="font-medium text-neutral-900">
              {{ app.name || app.client_id }}
            </p>
            <div class="flex items-center gap-1.5 mt-0.5">
              <span class="text-sm text-neutral-500">{{ $t('account.applications.clientId') }}</span>
              <DData :value="app.client_id" />
            </div>
            <div class="flex items-center gap-1.5 mt-1.5">
              <AppBadge
                :variant="app.client_type === 'PUBLIC' ? 'info' : 'neutral'"
                size="sm"
              >{{ app.client_type }}</AppBadge>
              <AppBadge
                :variant="app.status === 'active' ? 'success' : 'warning'"
                size="sm"
              >{{ app.status }}</AppBadge>
            </div>
            <div
              v-if="(app.redirect_uris || []).length"
              class="text-xs text-neutral-400 mt-1"
            >
              {{ app.redirect_uris.join(', ') }}
            </div>
          </div>
          <div class="flex items-center gap-2">
            <button
              v-if="app.client_type === 'CONFIDENTIAL'"
              class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
              @click="rotateSecret(app)"
            >
              {{ $t('account.applications.rotate') }}
            </button>
            <button
              class="px-3 py-1.5 text-sm text-error-600 border border-error-200 rounded-ctl hover:bg-error-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
              @click="deleteApp(app)"
            >
              {{ $t('account.applications.delete') }}
            </button>
          </div>
        </div>
      </AppCard>
    </div>
  </div>
</template>
