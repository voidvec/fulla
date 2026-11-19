<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'

// P2-1d: upstream identity-provider registry console. The backend CRUD
// (/api/admin/upstream-providers, v1.7.0 P2-1) with no UI until now.
// client_secret is write-only: never echoed (client_secret_set bool);
// empty on update keeps the current one. The three legacy social keys
// (google/wechat/github) can be edited/toggled here (P2-1c: the row is the
// runtime source of truth) but cannot be CREATED -- they are born from the
// startup config seed.
const { t } = useI18n()

const providers = ref<any[]>([])
const loading = ref(true)
const showCreateModal = ref(false)
const showEditModal = ref(false)
const selected = ref<any>(null)
const saving = ref(false)
const successMessage = ref('')
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

const RESERVED_KEYS = ['google', 'wechat', 'github']

const emptyForm = () => ({
  provider_key: '',
  display_name: '',
  issuer: '',
  client_id: '',
  client_secret: '',
  scopes: 'openid profile email',
  claims_mapping: '',
  endpoint_overrides: '',
  enabled: true,
})
const newProvider = ref(emptyForm())
const editProvider = ref(emptyForm())

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

// Optional JSON columns travel as strings; validate locally so a typo gets
// an inline message instead of the server's 400 envelope.
function parseOptionalJson(value: string): string | null {
  const trimmed = value.trim()
  if (!trimmed) return ''
  try {
    const parsed = JSON.parse(trimmed)
    if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) return null
    return JSON.stringify(parsed)
  } catch {
    return null
  }
}

async function fetchProviders() {
  loading.value = true
  try {
    const resp = await axios.get('/api/admin/upstream-providers')
    providers.value = resp.data.providers || []
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function createProvider() {
  const form = newProvider.value
  if (!form.provider_key.trim() || !form.display_name.trim() || !form.issuer.trim() || !form.client_id.trim() || !form.client_secret) return
  // Reserved-key rejection is the SERVER's call (single source of truth:
  // VALIDATION_FORMAT_ERROR catalog text) -- surfacing its 400 here beats a
  // silent client-side guard.
  const claims = parseOptionalJson(form.claims_mapping)
  const overrides = parseOptionalJson(form.endpoint_overrides)
  if (claims === null || overrides === null) {
    showError(t('admin.upstreamProviders.invalidJson'))
    return
  }
  saving.value = true
  try {
    await axios.post('/api/admin/upstream-providers', {
      provider_key: form.provider_key.trim(),
      display_name: form.display_name.trim(),
      issuer: form.issuer.trim(),
      client_id: form.client_id,
      client_secret: form.client_secret,
      scopes: form.scopes,
      claims_mapping: claims,
      endpoint_overrides: overrides,
      enabled: form.enabled,
    }, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.upstreamProviders.created', { name: form.provider_key }))
    showCreateModal.value = false
    newProvider.value = emptyForm()
    await fetchProviders()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

function openEditModal(provider: any) {
  selected.value = provider
  editProvider.value = {
    provider_key: provider.provider_key,
    display_name: provider.display_name || '',
    issuer: provider.issuer || '',
    client_id: provider.client_id || '',
    client_secret: '',
    scopes: provider.scopes || 'openid profile email',
    claims_mapping: provider.claims_mapping || '',
    endpoint_overrides: provider.endpoint_overrides || '',
    enabled: !!provider.enabled,
  }
  showEditModal.value = true
}

async function updateProvider() {
  if (!selected.value) return
  const form = editProvider.value
  if (!form.display_name.trim() || !form.issuer.trim() || !form.client_id.trim()) return
  const claims = parseOptionalJson(form.claims_mapping)
  const overrides = parseOptionalJson(form.endpoint_overrides)
  if (claims === null || overrides === null) {
    showError(t('admin.upstreamProviders.invalidJson'))
    return
  }
  saving.value = true
  try {
    // Empty client_secret = keep the current one (write-only semantics).
    await axios.put(`/api/admin/upstream-providers/${selected.value.provider_key}`, {
      display_name: form.display_name.trim(),
      issuer: form.issuer.trim(),
      client_id: form.client_id,
      client_secret: form.client_secret,
      scopes: form.scopes,
      claims_mapping: claims,
      endpoint_overrides: overrides,
      enabled: form.enabled,
    }, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.upstreamProviders.updated', { name: selected.value.provider_key }))
    showEditModal.value = false
    await fetchProviders()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

async function toggleEnabled(provider: any) {
  const next = !provider.enabled
  try {
    await axios.put(`/api/admin/upstream-providers/${provider.provider_key}`, {
      display_name: provider.display_name,
      issuer: provider.issuer,
      client_id: provider.client_id,
      client_secret: '',
      scopes: provider.scopes,
      claims_mapping: provider.claims_mapping || '',
      endpoint_overrides: provider.endpoint_overrides || '',
      enabled: next,
    }, { headers: { 'Content-Type': 'application/json' } })
    provider.enabled = next
    showSuccess(next
      ? t('admin.upstreamProviders.enabledMsg', { name: provider.provider_key })
      : t('admin.upstreamProviders.disabledMsg', { name: provider.provider_key }))
  } catch (e: unknown) {
    showError(normalizeError(e))
  }
}

function deleteProvider(provider: any) {
  askConfirm(
    t('admin.upstreamProviders.deleteConfirm', { name: provider.provider_key }),
    async () => {
      try {
        await axios.delete(`/api/admin/upstream-providers/${provider.provider_key}`)
        showSuccess(t('admin.upstreamProviders.deleted', { name: provider.provider_key }))
        await fetchProviders()
      } catch (e: unknown) {
        showError(normalizeError(e))
      }
    },
  )
}

function isLegacy(provider: any): boolean {
  return RESERVED_KEYS.includes(provider.provider_key)
}

onMounted(() => {
  fetchProviders()
})
</script>

<template>
  <div>
    <div class="flex justify-between items-center mb-6">
      <h2 class="text-2xl font-bold text-neutral-900">
        {{ $t('admin.upstreamProviders.title') }}
      </h2>
      <button
        class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700"
        @click="showCreateModal = true"
      >
        {{ $t('admin.upstreamProviders.create') }}
      </button>
    </div>

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

    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ $t('common.loading') }}
    </div>

    <div
      v-else
      class="bg-surface shadow rounded-lg overflow-hidden"
    >
      <table class="min-w-full divide-y divide-neutral-200">
        <thead class="bg-neutral-50">
          <tr>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.upstreamProviders.key') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.upstreamProviders.displayName') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.upstreamProviders.type') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.upstreamProviders.issuer') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.upstreamProviders.enabled') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.actions') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="provider in providers"
            :key="provider.provider_key"
            class="hover:bg-neutral-50"
          >
            <td class="px-6 py-3">
              <span class="text-sm font-medium text-neutral-900 font-mono">{{ provider.provider_key }}</span>
            </td>
            <td class="px-6 py-3 text-sm text-neutral-700">
              {{ provider.display_name }}
            </td>
            <td class="px-6 py-3">
              <span
                v-if="isLegacy(provider)"
                class="px-1.5 py-0.5 text-xs bg-neutral-200 text-neutral-700 rounded font-mono"
                :title="$t('admin.upstreamProviders.legacyHint')"
              >social</span>
              <span
                v-else
                class="px-1.5 py-0.5 text-xs bg-brand-100 text-brand-700 rounded font-mono"
              >oidc</span>
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500 font-mono truncate max-w-[220px]">
              {{ provider.issuer }}
            </td>
            <td class="px-6 py-3">
              <button
                :aria-label="$t('admin.upstreamProviders.toggleAria', { name: provider.provider_key })"
                class="relative inline-flex h-6 w-11 items-center rounded-full transition-colors"
                :class="provider.enabled ? 'bg-brand-600' : 'bg-neutral-300'"
                @click="toggleEnabled(provider)"
              >
                <span
                  class="inline-block h-4 w-4 transform rounded-full bg-white transition-transform"
                  :class="provider.enabled ? 'translate-x-6' : 'translate-x-1'"
                />
              </button>
            </td>
            <td class="px-6 py-3 text-sm space-x-2">
              <button
                class="text-brand-600 hover:text-brand-800"
                data-testid="edit-provider"
                @click="openEditModal(provider)"
              >
                {{ $t('common.edit') }}
              </button>
              <button
                class="text-error-600 hover:text-error-800"
                data-testid="delete-provider"
                @click="deleteProvider(provider)"
              >
                {{ $t('common.delete') }}
              </button>
            </td>
          </tr>
          <tr v-if="providers.length === 0">
            <td
              colspan="6"
              class="px-6 py-12 text-center text-sm text-neutral-500"
            >
              {{ $t('admin.upstreamProviders.empty') }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <!-- Create modal -->
    <div
      v-if="showCreateModal"
      class="fixed inset-0 z-50 flex items-center justify-center"
    >
      <div
        class="absolute inset-0 bg-black/40"
        @click="showCreateModal = false"
      />
      <div class="relative bg-surface rounded-lg shadow-xl max-w-lg w-full mx-4 max-h-[90vh] overflow-y-auto">
        <div class="px-6 py-4 border-b border-neutral-200">
          <h3 class="text-lg font-semibold text-neutral-900">
            {{ $t('admin.upstreamProviders.createTitle') }}
          </h3>
        </div>
        <form
          class="px-6 py-4 space-y-4"
          @submit.prevent="createProvider"
        >
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.upstreamProviders.key') }}</label>
            <input
              v-model="newProvider.provider_key"
              data-testid="new-provider-key"
              required
              pattern="[a-z0-9][a-z0-9-]{1,62}[a-z0-9]"
              class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
              placeholder="entra-prod"
            >
            <p class="mt-1 text-xs text-neutral-400">
              {{ t('admin.upstreamProviders.keyHint') }}
            </p>
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.upstreamProviders.displayName') }}</label>
            <input
              v-model="newProvider.display_name"
              data-testid="new-provider-name"
              required
              class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.upstreamProviders.issuer') }}</label>
            <input
              v-model="newProvider.issuer"
              data-testid="new-provider-issuer"
              required
              class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
              placeholder="https://login.microsoftonline.com/{tenant}/v2.0"
            >
          </div>
          <div class="grid grid-cols-2 gap-3">
            <div>
              <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.clientId') }}</label>
              <input
                v-model="newProvider.client_id"
                required
                class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
              >
            </div>
            <div>
              <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.secretField') }}</label>
              <input
                v-model="newProvider.client_secret"
                type="password"
                required
                autocomplete="new-password"
                class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
              >
            </div>
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.scopes') }}</label>
            <input
              v-model="newProvider.scopes"
              class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
            >
          </div>
          <details>
            <summary class="text-sm text-neutral-500 cursor-pointer">
              {{ t('admin.upstreamProviders.advanced') }}
            </summary>
            <div class="mt-3 space-y-3">
              <div>
                <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.claimsMapping') }}</label>
                <textarea
                  v-model="newProvider.claims_mapping"
                  rows="2"
                  class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-xs font-mono"
                  placeholder='{"sub":"sub","email":"email","username":"preferred_username"}'
                />
              </div>
              <div>
                <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.endpointOverrides') }}</label>
                <textarea
                  v-model="newProvider.endpoint_overrides"
                  rows="2"
                  class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-xs font-mono"
                  placeholder='{"authorize":"https://…","token":"https://…","userinfo":"https://…"}'
                />
              </div>
            </div>
          </details>
          <div class="flex justify-end space-x-3 pt-2">
            <button
              type="button"
              class="px-4 py-2 text-sm rounded-md border border-neutral-300 text-neutral-700"
              @click="showCreateModal = false"
            >
              {{ t('common.cancel') }}
            </button>
            <button
              type="submit"
              :disabled="saving"
              data-testid="create-provider-submit"
              class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            >
              {{ t('common.create') }}
            </button>
          </div>
        </form>
      </div>
    </div>

    <!-- Edit modal -->
    <div
      v-if="showEditModal"
      class="fixed inset-0 z-50 flex items-center justify-center"
    >
      <div
        class="absolute inset-0 bg-black/40"
        @click="showEditModal = false"
      />
      <div class="relative bg-surface rounded-lg shadow-xl max-w-lg w-full mx-4 max-h-[90vh] overflow-y-auto">
        <div class="px-6 py-4 border-b border-neutral-200 flex items-center justify-between">
          <h3 class="text-lg font-semibold text-neutral-900">
            {{ t('admin.upstreamProviders.editTitle', { name: editProvider.provider_key }) }}
          </h3>
          <span
            v-if="isLegacy(editProvider)"
            class="px-1.5 py-0.5 text-xs bg-neutral-200 text-neutral-700 rounded font-mono"
          >social</span>
          <span
            v-else
            class="px-1.5 py-0.5 text-xs bg-brand-100 text-brand-700 rounded font-mono"
          >oidc</span>
        </div>
        <form
          class="px-6 py-4 space-y-4"
          @submit.prevent="updateProvider"
        >
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.displayName') }}</label>
            <input
              v-model="editProvider.display_name"
              data-testid="edit-provider-name"
              required
              class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.issuer') }}</label>
            <input
              v-model="editProvider.issuer"
              required
              class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
            >
          </div>
          <div class="grid grid-cols-2 gap-3">
            <div>
              <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.clientId') }}</label>
              <input
                v-model="editProvider.client_id"
                required
                class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
              >
            </div>
            <div>
              <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.secretField') }}</label>
              <input
                v-model="editProvider.client_secret"
                type="password"
                autocomplete="new-password"
                :placeholder="$t('admin.upstreamProviders.keepSecret')"
                class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
              >
            </div>
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.scopes') }}</label>
            <input
              v-model="editProvider.scopes"
              class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-sm font-mono"
            >
          </div>
          <details>
            <summary class="text-sm text-neutral-500 cursor-pointer">
              {{ t('admin.upstreamProviders.advanced') }}
            </summary>
            <div class="mt-3 space-y-3">
              <div>
                <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.claimsMapping') }}</label>
                <textarea
                  v-model="editProvider.claims_mapping"
                  rows="2"
                  class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-xs font-mono"
                />
              </div>
              <div>
                <label class="block text-sm font-medium text-neutral-700">{{ t('admin.upstreamProviders.endpointOverrides') }}</label>
                <textarea
                  v-model="editProvider.endpoint_overrides"
                  rows="2"
                  class="mt-1 block w-full rounded-md border border-neutral-300 px-3 py-2 text-xs font-mono"
                />
              </div>
            </div>
          </details>
          <label class="flex items-center space-x-2 text-sm text-neutral-700">
            <input
              v-model="editProvider.enabled"
              type="checkbox"
              class="rounded"
            >
            {{ t('admin.upstreamProviders.enabled') }}
          </label>
          <div class="flex justify-end space-x-3 pt-2">
            <button
              type="button"
              class="px-4 py-2 text-sm rounded-md border border-neutral-300 text-neutral-700"
              @click="showEditModal = false"
            >
              {{ t('common.cancel') }}
            </button>
            <button
              type="submit"
              :disabled="saving"
              data-testid="edit-provider-submit"
              class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            >
              {{ t('common.save') }}
            </button>
          </div>
        </form>
      </div>
    </div>

    <AppConfirmDialog
      :open="confirmOpen"
      :message="confirmMessage"
      :confirm-label="$t('common.delete')"
      @confirm="runConfirm"
      @cancel="confirmOpen = false"
    />
  </div>
</template>
