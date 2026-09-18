<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppEmptyState from '../../components/ui/AppEmptyState.vue'

const { t } = useI18n()

const clients = ref<any[]>([])
const loading = ref(true)
const showCreateModal = ref(false)
const showSecretModal = ref(false)
const newClientSecret = ref('')
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen;
// plain strings (parameterized chrome copy via t()) keep snapshot semantics.
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})
const createForm = ref({
  name: '',
  client_type: 'CONFIDENTIAL',
  redirect_uris: '',
  grant_types: ['authorization_code'] as string[],
  backchannel_logout_uri: '',
})
const creating = ref(false)

// Grant-type checkboxes: labels/descriptions follow the active locale.
const AVAILABLE_GRANT_TYPES = computed(() => [
  { value: 'authorization_code', label: t('admin.applications.grantTypeOptions.authorizationCode.label'), description: t('admin.applications.grantTypeOptions.authorizationCode.description') },
  { value: 'refresh_token', label: t('admin.applications.grantTypeOptions.refreshToken.label'), description: t('admin.applications.grantTypeOptions.refreshToken.description') },
  { value: 'client_credentials', label: t('admin.applications.grantTypeOptions.clientCredentials.label'), description: t('admin.applications.grantTypeOptions.clientCredentials.description') },
  { value: 'urn:ietf:params:oauth:grant-type:device_code', label: t('admin.applications.grantTypeOptions.deviceCode.label'), description: t('admin.applications.grantTypeOptions.deviceCode.description') },
])

// Inline error banner (replaces native alert for backend errors, Req 10.6).
function showError(msg: NormalizedError | string) {
  errorMessage.value = msg
  setTimeout(() => { errorMessage.value = null }, 5000)
}

async function fetchClients() {
  loading.value = true
  try {
    const resp = await axios.get('/api/admin/clients')
    clients.value = resp.data.clients || []
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function createClient() {
  if (createForm.value.grant_types.length === 0) {
    showError(t('admin.applications.selectGrantType'))
    return
  }
  creating.value = true
  try {
    const body = {
      name: createForm.value.name,
      client_type: createForm.value.client_type,
      redirect_uris: createForm.value.redirect_uris,
      allowed_grant_types: createForm.value.grant_types.join(','),
      backchannel_logout_uri: createForm.value.backchannel_logout_uri,
    }
    const resp = await axios.post('/api/admin/clients', body, {
      headers: { 'Content-Type': 'application/json' },
    })
    newClientSecret.value = resp.data.client_secret || ''
    showCreateModal.value = false
    showSecretModal.value = true
    createForm.value = { name: '', client_type: 'CONFIDENTIAL', redirect_uris: '', grant_types: ['authorization_code'], backchannel_logout_uri: '' }
    await fetchClients()
  } catch (e: unknown) {
    // Req 10.3/10.6: normalize via Frontend_Error_Module, no native alert.
    showError(normalizeError(e))
  } finally {
    creating.value = false
  }
}

async function deleteClient(clientId: string) {
  if (!confirm(t('admin.applications.deleteConfirm', { name: clientId }))) return
  try {
    await axios.delete(`/api/admin/clients/${clientId}`)
    await fetchClients()
  } catch (e: unknown) {
    showError(normalizeError(e))
  }
}

async function resetSecret(clientId: string) {
  if (!confirm(t('admin.applications.resetSecretConfirm', { name: clientId }))) return
  try {
    const resp = await axios.post(`/api/admin/clients/${clientId}/reset-secret`)
    newClientSecret.value = resp.data.client_secret || ''
    showSecretModal.value = true
  } catch (e: unknown) {
    showError(normalizeError(e))
  }
}

// v1.4.0 open platform governance: suspend/resume self-registered apps.
// The status lives on the owners row; admin-seeded clients have none.
async function setSuspended(client: any, suspended: boolean) {
  const action = suspended ? 'suspend' : 'resume'
  if (!confirm(t(`admin.applications.${action}Confirm`, { name: client.name || client.client_id }))) return
  try {
    await axios.post(`/api/admin/clients/${client.client_id}/${action}`)
    await fetchClients()
  } catch (e: unknown) {
    showError(normalizeError(e))
  }
}

function ownerLabel(client: any): string {
  if (!client.owner) return ''
  if (client.owner.org_name) return client.owner.org_name
  return client.owner.creator_name || `#${client.owner.creator_user_id}`
}

onMounted(fetchClients)
</script>

<template>
  <div>
    <div class="flex justify-between items-center mb-6">
      <h2 class="text-2xl font-bold text-neutral-900">
        {{ $t('admin.applications.title') }}
      </h2>
      <button
        class="px-4 py-2 bg-brand-600 text-white rounded-md hover:bg-brand-700 text-sm font-medium"
        @click="showCreateModal = true"
      >
        {{ $t('admin.applications.create') }}
      </button>
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

    <AppEmptyState
      v-else-if="clients.length === 0"
      :title="$t('admin.applications.emptyTitle')"
      :description="$t('admin.applications.emptyDescription')"
      :action-label="$t('admin.applications.emptyAction')"
      @action="showCreateModal = true"
    />

    <div
      v-else
      class="bg-surface shadow rounded-lg overflow-hidden"
    >
      <table class="min-w-full divide-y divide-neutral-200">
        <thead class="bg-neutral-50">
          <tr>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.name') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.applications.clientId') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.type') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.applications.owner') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.actions') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="client in clients"
            :key="client.client_id"
            class="hover:bg-neutral-50"
          >
            <td class="px-6 py-3 text-sm font-medium text-neutral-900">
              <router-link
                :to="{ name: 'application-detail', params: { id: client.client_id } }"
                class="text-brand-600 hover:text-brand-800 hover:underline"
              >
                {{ client.name || client.client_id }}
              </router-link>
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500 font-mono text-xs">
              {{ client.client_id }}
            </td>
            <td class="px-6 py-3">
              <span
                class="px-2 py-1 text-xs rounded-full"
                :class="client.client_type === 'PUBLIC' ? 'bg-brand-100 text-brand-800' : 'bg-info-100 text-info-700'"
              >
                {{ client.client_type }}
              </span>
            </td>
            <td class="px-6 py-3 text-sm">
              <template v-if="client.owner">
                <span
                  class="px-1.5 py-0.5 text-[10px] font-semibold rounded bg-success-50 text-success-700 border border-success-200 mr-1.5"
                  :title="$t('admin.applications.selfRegistered')"
                >{{ $t('admin.applications.selfRegisteredShort') }}</span>
                <span
                  class="text-neutral-700"
                  :class="client.owner.status === 'suspended' ? 'line-through text-neutral-400' : ''"
                >{{ ownerLabel(client) }}</span>
              </template>
              <span
                v-else
                class="text-xs text-neutral-400"
              >{{ $t('admin.applications.adminManaged') }}</span>
            </td>
            <td class="px-6 py-3 text-sm space-x-2">
              <button
                v-if="client.client_type === 'CONFIDENTIAL'"
                class="px-2 py-1 rounded text-brand-600 hover:bg-brand-50 hover:text-brand-800 font-medium transition-colors"
                @click="resetSecret(client.client_id)"
              >
                {{ $t('admin.applications.resetSecret') }}
              </button>
              <button
                v-if="client.owner && client.owner.status !== 'suspended'"
                class="px-2 py-1 rounded text-warning-600 hover:bg-warning-50 font-medium transition-colors"
                @click="setSuspended(client, true)"
              >
                {{ $t('admin.applications.suspend') }}
              </button>
              <button
                v-if="client.owner && client.owner.status === 'suspended'"
                class="px-2 py-1 rounded text-success-600 hover:bg-success-50 font-medium transition-colors"
                @click="setSuspended(client, false)"
              >
                {{ $t('admin.applications.resume') }}
              </button>
              <button
                class="px-2 py-1 rounded text-error-600 hover:bg-error-50 hover:text-error-700 font-medium transition-colors"
                @click="deleteClient(client.client_id)"
              >
                {{ $t('common.delete') }}
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <!-- Create Modal -->
    <div
      v-if="showCreateModal"
      class="fixed inset-0 bg-black/50 flex items-center justify-center z-50"
    >
      <div class="bg-surface rounded-lg shadow-xl p-6 w-full max-w-md">
        <h3 class="text-lg font-semibold mb-4">
          {{ $t('admin.applications.createTitle') }}
        </h3>
        <form
          class="space-y-4"
          @submit.prevent="createClient"
        >
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.applications.nameLabel') }}</label>
            <input
              v-model="createForm.name"
              required
              class="mt-1 block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
              :placeholder="$t('admin.applications.namePlaceholder')"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ $t('common.type') }}</label>
            <select
              v-model="createForm.client_type"
              class="mt-1 block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
            >
              <option value="CONFIDENTIAL">
                {{ $t('admin.applications.confidential') }}
              </option>
              <option value="PUBLIC">
                {{ $t('admin.applications.public') }}
              </option>
            </select>
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.applications.redirectUrisComma') }}</label>
            <input
              v-model="createForm.redirect_uris"
              class="mt-1 block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
              placeholder="https://myapp.com/callback"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.applications.backchannelLabel') }}</label>
            <input
              v-model="createForm.backchannel_logout_uri"
              class="mt-1 block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
              placeholder="https://rp.example.com/backchannel-logout"
            >
            <p class="mt-1 text-xs text-neutral-500">
              {{ $t('admin.applications.backchannelHint') }}
            </p>
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-2">{{ $t('admin.applications.grantTypes') }}</label>
            <div class="space-y-2">
              <label
                v-for="gt in AVAILABLE_GRANT_TYPES"
                :key="gt.value"
                class="flex items-start gap-2 cursor-pointer"
              >
                <input
                  v-model="createForm.grant_types"
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
          <div class="flex justify-end space-x-3 pt-2">
            <button
              type="button"
              class="px-4 py-2 border border-neutral-300 rounded-md text-sm"
              @click="showCreateModal = false"
            >
              {{ $t('common.cancel') }}
            </button>
            <button
              type="submit"
              :disabled="creating"
              class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            >
              {{ creating ? $t('common.creating') : $t('common.create') }}
            </button>
          </div>
        </form>
      </div>
    </div>

    <!-- Secret Display Modal -->
    <div
      v-if="showSecretModal"
      class="fixed inset-0 bg-black/50 flex items-center justify-center z-50"
    >
      <div class="bg-surface rounded-lg shadow-xl p-6 w-full max-w-md">
        <h3 class="text-lg font-semibold mb-2">
          {{ $t('admin.applications.secretTitle') }}
        </h3>
        <p class="text-sm text-error-600 mb-4">
          {{ $t('admin.applications.secretWarning') }}
        </p>
        <div class="bg-neutral-100 p-3 rounded-md font-mono text-sm break-all select-all">
          {{ newClientSecret }}
        </div>
        <div class="flex justify-end mt-4">
          <button
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm"
            @click="showSecretModal = false; newClientSecret = ''"
          >
            {{ $t('common.done') }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
