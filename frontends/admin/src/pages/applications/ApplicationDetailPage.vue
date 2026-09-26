<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import DData from '../../components/ui/DData.vue'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'

const { t } = useI18n()
const route = useRoute()
const clientId = computed(() => route.params.id as string)

const loading = ref(true)
const saving = ref(false)
const savingScopes = ref(false)
const activeTab = ref<'info' | 'auth' | 'scopes' | 'credentials'>('info')
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

// #181: destructive actions route through the shared confirm dialog instead
// of native confirm() (which needed page.on('dialog') shims in the e2e suite).
const confirmOpen = ref(false)
const confirmMessage = ref('')
const confirmAction = ref<(() => Promise<void>) | null>(null)
function askConfirm(message: string, action: () => Promise<void>) {
  confirmMessage.value = message
  confirmAction.value = action
  confirmOpen.value = true
}
async function runConfirm() {
  confirmOpen.value = false
  const action = confirmAction.value
  confirmAction.value = null
  if (action) await action()
}

// Client data
const client = ref<any>({})
const editName = ref('')
const editRedirectUris = ref('')
const editGrantTypes = ref<string[]>([])
const editBackchannelLogoutUri = ref('')

// Scopes data
const allScopes = ref<any[]>([])
const clientScopes = ref<string[]>([])

// Secret modal
const showSecretModal = ref(false)
const newClientSecret = ref('')

// Grant-type checkboxes: labels/descriptions follow the active locale.
const AVAILABLE_GRANT_TYPES = computed(() => [
  { value: 'authorization_code', label: t('admin.applications.grantTypeOptions.authorizationCode.label'), description: t('admin.applications.grantTypeOptions.authorizationCode.description') },
  { value: 'refresh_token', label: t('admin.applications.grantTypeOptions.refreshToken.label'), description: t('admin.applications.grantTypeOptions.refreshToken.description') },
  { value: 'client_credentials', label: t('admin.applications.grantTypeOptions.clientCredentials.label'), description: t('admin.applications.grantTypeOptions.clientCredentials.description') },
  { value: 'urn:ietf:params:oauth:grant-type:device_code', label: t('admin.applications.grantTypeOptions.deviceCode.label'), description: t('admin.applications.grantTypeOptions.deviceCode.description') },
])

// Tab strip (labels follow the active locale).
const TABS = computed(() => [
  { key: 'info' as const, label: t('admin.applications.tabs.info') },
  { key: 'auth' as const, label: t('admin.applications.tabs.auth') },
  { key: 'scopes' as const, label: t('admin.applications.tabs.scopes') },
  { key: 'credentials' as const, label: t('admin.applications.tabs.credentials') },
])

function showSuccess(msg: string) {
  successMessage.value = msg
  errorMessage.value = ''
  setTimeout(() => { successMessage.value = '' }, 3000)
}

function showError(msg: NormalizedError | string) {
  errorMessage.value = msg
  successMessage.value = ''
  setTimeout(() => { errorMessage.value = null }, 5000)
}

async function fetchClient() {
  loading.value = true
  try {
    const resp = await axios.get(`/api/admin/clients/${clientId.value}`)
    client.value = resp.data
    editName.value = resp.data.name || ''
    editRedirectUris.value = (resp.data.redirect_uris || '').replace(/,/g, '\n')
    editGrantTypes.value = resp.data.allowed_grant_types
      ? resp.data.allowed_grant_types.split(',').filter((s: string) => s)
      : []
    editBackchannelLogoutUri.value = resp.data.backchannel_logout_uri || ''
    clientScopes.value = resp.data.scopes || []
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function fetchAllScopes() {
  try {
    const resp = await axios.get('/api/admin/scopes')
    allScopes.value = resp.data.scopes || []
  } catch (e) {
    console.error('Failed to fetch scopes:', e)
  }
}

async function saveChanges() {
  saving.value = true
  try {
    const body: any = {}
    if (editName.value !== (client.value.name || '')) {
      // RFC 7591 §2.2: client_name is required; the backend now rejects
      // empty names with 400. Validate client-side for better UX.
      if (!editName.value.trim()) {
        showError(t('admin.applications.nameRequired'))
        saving.value = false
        return
      }
      body.name = editName.value
    }
    const uris = editRedirectUris.value.split('\n').map(s => s.trim()).filter(s => s).join(',')
    if (uris !== (client.value.redirect_uris || '')) {
      body.redirect_uris = uris
    }
    const grants = editGrantTypes.value.join(',')
    if (grants !== (client.value.allowed_grant_types || '')) {
      body.allowed_grant_types = grants
    }
    if (editBackchannelLogoutUri.value !== (client.value.backchannel_logout_uri || '')) {
      body.backchannel_logout_uri = editBackchannelLogoutUri.value
    }

    if (Object.keys(body).length === 0) {
      showSuccess(t('admin.applications.noChangesToSave'))
      saving.value = false
      return
    }

    await axios.put(`/api/admin/clients/${clientId.value}`, body, {
      headers: { 'Content-Type': 'application/json' },
    })
    showSuccess(t('admin.applications.changesSaved'))
    await fetchClient()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

async function saveScopes() {
  savingScopes.value = true
  try {
    const resp = await axios.put(`/api/admin/clients/${clientId.value}/scopes`, {
      scopes: clientScopes.value,
    }, {
      headers: { 'Content-Type': 'application/json' },
    })
    clientScopes.value = resp.data.scopes || clientScopes.value
    showSuccess(t('admin.applications.scopesUpdated'))
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    savingScopes.value = false
  }
}

function resetSecret() {
  askConfirm(t('admin.applications.resetSecretConfirm', { name: clientId.value }), async () => {
    try {
      const resp = await axios.post(`/api/admin/clients/${clientId.value}/reset-secret`)
      newClientSecret.value = resp.data.client_secret || ''
      showSecretModal.value = true
    } catch (e: unknown) {
      showError(normalizeError(e))
    }
  })
}

function copyToClipboard(text: string) {
  navigator.clipboard.writeText(text)
  showSuccess(t('admin.applications.copiedToClipboard'))
}

onMounted(() => {
  fetchClient()
  fetchAllScopes()
})
</script>

<template>
  <div>
    <!-- Header -->
    <div class="flex justify-between items-center mb-6">
      <div class="flex items-center gap-3">
        <router-link
          :to="{ name: 'applications' }"
          class="text-neutral-500 hover:text-neutral-700 text-sm"
        >
          {{ $t('admin.applications.backToList') }}
        </router-link>
      </div>
      <button
        v-if="activeTab === 'info' || activeTab === 'auth'"
        :disabled="saving"
        class="px-4 py-2 bg-brand-600 text-white rounded-md hover:bg-brand-700 text-sm font-medium disabled:opacity-50"
        @click="saveChanges"
      >
        {{ saving ? $t('common.saving') : $t('common.saveChanges') }}
      </button>
    </div>

    <!-- Toast Messages -->
    <div
      v-if="successMessage"
      class="mb-4 p-3 bg-success-50 border border-success-200 text-success-700 rounded-md text-sm"
    >
      {{ successMessage }}
    </div>
    <div
      v-if="errorText"
      class="mb-4 p-3 bg-error-50 border border-error-200 text-error-700 rounded-md text-sm"
    >
      {{ errorText }}
    </div>

    <!-- Loading -->
    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ $t('common.loading') }}
    </div>

    <!-- Content -->
    <div v-else>
      <!-- App Title -->
      <h2 class="text-2xl font-bold text-neutral-900 mb-6">
        {{ client.name || client.client_id }}
      </h2>

      <!-- Tabs -->
      <div class="border-b border-neutral-200 mb-6">
        <nav class="-mb-px flex space-x-8">
          <button
            v-for="tab in TABS"
            :key="tab.key"
            :class="[
              'py-3 px-1 border-b-2 text-sm font-medium transition-colors',
              activeTab === tab.key
                ? 'border-brand-500 text-brand-600'
                : 'border-transparent text-neutral-500 hover:text-neutral-700 hover:border-neutral-300'
            ]"
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
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.applications.clientId') }}</label>
          <div class="flex items-center gap-2">
            <DData
              :value="client.client_id"
              truncate
              class="flex-1"
              data-testid="client-id-chip"
            />
            <button
              class="px-3 py-2 text-sm text-brand-600 hover:bg-brand-50 rounded-md transition-colors"
              @click="copyToClipboard(client.client_id)"
            >
              {{ $t('common.copy') }}
            </button>
          </div>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.applications.nameLabel') }}</label>
          <input
            v-model="editName"
            class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm focus:ring-brand-500 focus:border-brand-500"
            :placeholder="$t('admin.applications.applicationNamePlaceholder')"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.type') }}</label>
          <span
            class="px-3 py-1 text-sm rounded-full"
            :class="client.client_type === 'PUBLIC' ? 'bg-brand-100 text-brand-800' : 'bg-info-100 text-info-700'"
          >
            {{ client.client_type }}
          </span>
        </div>
      </div>

      <!-- Auth Config Tab -->
      <div
        v-if="activeTab === 'auth'"
        class="bg-surface shadow rounded-lg p-6 space-y-5"
      >
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.applications.authMethodLabel') }}</label>
          <div>
            <DData :value="client.token_endpoint_auth_method || $t('admin.applications.notSet')" />
          </div>
          <p class="text-xs text-neutral-500 mt-1">
            {{
              $t('admin.applications.authMethodHint', {
                none: 'none',
                basic: 'client_secret_basic',
                post: 'client_secret_post',
              })
            }}
          </p>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.applications.redirectUrisLines') }}</label>
          <textarea
            v-model="editRedirectUris"
            rows="4"
            class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm font-mono focus:ring-brand-500 focus:border-brand-500"
            placeholder="https://myapp.com/callback"
          />
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-2">{{ $t('admin.applications.allowedGrantTypes') }}</label>
          <div class="space-y-2">
            <label
              v-for="gt in AVAILABLE_GRANT_TYPES"
              :key="gt.value"
              class="flex items-start gap-2 cursor-pointer"
            >
              <input
                v-model="editGrantTypes"
                type="checkbox"
                :value="gt.value"
                class="mt-0.5 h-4 w-4 rounded border-neutral-300 text-brand-600 focus:ring-brand-500"
              >
              <div>
                <span class="text-sm font-medium text-neutral-700">{{ gt.label }}</span>
                <p class="text-xs text-neutral-500">{{ gt.description }}</p>
              </div>
            </label>
          </div>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.applications.backchannelLabelEdit') }}</label>
          <input
            v-model="editBackchannelLogoutUri"
            class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm font-mono focus:ring-brand-500 focus:border-brand-500"
            placeholder="https://rp.example.com/backchannel-logout"
          >
          <p class="text-xs text-neutral-500 mt-1">
            {{ $t('admin.applications.backchannelHintEdit') }}
          </p>
        </div>
      </div>

      <!-- Scopes Tab -->
      <div
        v-if="activeTab === 'scopes'"
        class="bg-surface shadow rounded-lg p-6"
      >
        <div
          v-if="editGrantTypes.includes('client_credentials')"
          class="mb-4 p-3 bg-brand-50 border border-brand-200 text-brand-700 rounded-md text-sm"
        >
          {{ $t('admin.applications.clientCredentialsScopeNote') }}
        </div>
        <div
          v-if="allScopes.length === 0"
          class="text-neutral-500 text-sm"
        >
          {{ $t('admin.applications.noScopesAvailable') }}
        </div>
        <div
          v-else
          class="space-y-3"
        >
          <label
            v-for="scope in allScopes"
            :key="scope.name"
            class="flex items-start gap-3 cursor-pointer p-2 rounded hover:bg-neutral-50"
          >
            <input
              v-model="clientScopes"
              type="checkbox"
              :value="scope.name"
              class="mt-0.5 h-4 w-4 rounded border-neutral-300 text-brand-600 focus:ring-brand-500"
            >
            <div>
              <span class="text-sm font-medium text-neutral-700">{{ scope.name }}</span>
              <p
                v-if="scope.description"
                class="text-xs text-neutral-500"
              >{{ scope.description }}</p>
              <p
                v-if="scope.requires_admin_role"
                class="text-xs text-warning-600"
              >{{ $t('common.requiresAdminRole') }}</p>
            </div>
          </label>
        </div>
        <div class="mt-6 pt-4 border-t border-neutral-200">
          <button
            :disabled="savingScopes"
            class="px-4 py-2 bg-brand-600 text-white rounded-md hover:bg-brand-700 text-sm font-medium disabled:opacity-50"
            @click="saveScopes"
          >
            {{ savingScopes ? $t('common.saving') : $t('admin.applications.saveScopes') }}
          </button>
        </div>
      </div>

      <!-- Credentials Tab -->
      <div
        v-if="activeTab === 'credentials'"
        class="bg-surface shadow rounded-lg p-6"
      >
        <div v-if="client.client_type === 'CONFIDENTIAL'">
          <h3 class="text-sm font-medium text-neutral-700 mb-2">
            {{ $t('admin.applications.secretTitle') }}
          </h3>
          <p class="text-sm text-neutral-500 mb-4">
            {{ $t('admin.applications.secretStoredHint') }}
          </p>
          <button
            class="px-4 py-2 bg-error-600 text-white rounded-md hover:bg-error-700 text-sm font-medium"
            @click="resetSecret"
          >
            {{ $t('admin.applications.resetClientSecret') }}
          </button>
        </div>
        <div v-else>
          <p class="text-sm text-neutral-500">
            {{ $t('admin.applications.noClientSecret') }}
          </p>
        </div>
      </div>
    </div>

    <!-- Secret Display Modal -->
    <div
      v-if="showSecretModal"
      class="fixed inset-0 bg-black/50 flex items-center justify-center z-50"
    >
      <div class="bg-surface rounded-lg shadow-xl p-6 w-full max-w-md">
        <h3 class="text-lg font-semibold mb-2">
          {{ $t('admin.applications.newSecretTitle') }}
        </h3>
        <p class="text-sm text-error-600 mb-4">
          {{ $t('admin.applications.secretWarning') }}
        </p>
        <div class="bg-neutral-100 p-3 rounded-md font-mono text-sm break-all select-all">
          {{ newClientSecret }}
        </div>
        <div class="flex justify-end mt-4 gap-2">
          <button
            class="px-4 py-2 border border-neutral-300 rounded-md text-sm hover:bg-neutral-50"
            @click="copyToClipboard(newClientSecret)"
          >
            {{ $t('common.copy') }}
          </button>
          <button
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700"
            @click="showSecretModal = false; newClientSecret = ''"
          >
            {{ $t('common.done') }}
          </button>
        </div>
      </div>
    </div>

    <AppConfirmDialog
      :open="confirmOpen"
      :message="confirmMessage"
      danger
      @confirm="runConfirm"
      @cancel="confirmOpen = false"
    />
  </div>
</template>
