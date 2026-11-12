<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'

// P1-3 (#280) claim-mapping admin console (audit wave B): the /api/admin/
// claim-mappings CRUD existed since v1.6.0 with no UI — this page is the
// declared follow-up from #293.
const { t } = useI18n()

const mappings = ref<any[]>([])
const scopeNames = ref<string[]>([])
const loading = ref(true)
const showCreateModal = ref(false)
const showEditModal = ref(false)
const selectedMapping = ref<any>(null)
const saving = ref(false)
const successMessage = ref('')
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen.
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

// The source whitelist is exactly two shapes (P1-3 red line): custom
// attributes by key, plus the single built-in display-name source.
const BUILTIN_SOURCES = ['user.display_name']
const INCLUDE_IN_OPTIONS = ['both', 'id_token', 'userinfo']

const newMapping = ref({ claim_name: '', scope_name: '', source_path: '', include_in: 'both' })
const editMapping = ref({ claim_name: '', scope_name: '', source_path: '', include_in: 'both', enabled: false })

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

// #181: destructive actions route through the shared confirm dialog.
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

async function fetchMappings() {
  loading.value = true
  try {
    const resp = await axios.get('/api/admin/claim-mappings')
    mappings.value = resp.data.mappings || []
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function fetchScopeNames() {
  try {
    const resp = await axios.get('/api/admin/scopes')
    scopeNames.value = (resp.data.scopes || []).map((s: any) => s.name)
  } catch {
    // Non-fatal: the scope dropdown falls back to free-text input.
  }
}

async function createMapping() {
  if (!newMapping.value.claim_name.trim() || !newMapping.value.scope_name || !newMapping.value.source_path.trim()) return
  saving.value = true
  try {
    await axios.post('/api/admin/claim-mappings', newMapping.value, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.claimMappings.created', { name: newMapping.value.claim_name }))
    showCreateModal.value = false
    newMapping.value = { claim_name: '', scope_name: '', source_path: '', include_in: 'both' }
    await fetchMappings()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

function openEditModal(mapping: any) {
  selectedMapping.value = mapping
  editMapping.value = {
    claim_name: mapping.claim_name || '',
    scope_name: mapping.scope_name || '',
    source_path: mapping.source_path || '',
    include_in: mapping.include_in || 'both',
    enabled: !!mapping.enabled,
  }
  showEditModal.value = true
}

// PUT is a FULL replace (scope_name/claim_name/source_path are required in
// every request; enabled rides along).
async function updateMapping() {
  if (!selectedMapping.value) return
  saving.value = true
  try {
    await axios.put(`/api/admin/claim-mappings/${selectedMapping.value.id}`, editMapping.value, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.claimMappings.updated'))
    showEditModal.value = false
    await fetchMappings()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

// Row-level enabled toggle: same full-replace PUT with the row's current
// identity fields and the flipped flag.
async function toggleEnabled(mapping: any) {
  const next = !mapping.enabled
  try {
    await axios.put(`/api/admin/claim-mappings/${mapping.id}`, {
      claim_name: mapping.claim_name,
      scope_name: mapping.scope_name,
      source_path: mapping.source_path,
      include_in: mapping.include_in || 'both',
      enabled: next,
    }, { headers: { 'Content-Type': 'application/json' } })
    mapping.enabled = next
    showSuccess(next ? t('admin.claimMappings.enabledMsg', { name: mapping.claim_name }) : t('admin.claimMappings.disabledMsg', { name: mapping.claim_name }))
  } catch (e: unknown) {
    showError(normalizeError(e))
  }
}

function deleteMapping(mapping: any) {
  askConfirm(t('admin.claimMappings.deleteConfirm', { name: mapping.claim_name }), async () => {
    try {
      await axios.delete(`/api/admin/claim-mappings/${mapping.id}`)
      showSuccess(t('admin.claimMappings.deleted', { name: mapping.claim_name }))
      await fetchMappings()
    } catch (e: unknown) {
      showError(normalizeError(e))
    }
  })
}

onMounted(() => {
  fetchMappings()
  fetchScopeNames()
})
</script>

<template>
  <div>
    <div class="flex justify-between items-center mb-6">
      <h2 class="text-2xl font-bold text-neutral-900">
        {{ $t('admin.claimMappings.title') }}
      </h2>
      <button
        class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700"
        @click="showCreateModal = true"
      >
        {{ $t('admin.claimMappings.create') }}
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
              {{ $t('admin.claimMappings.claim') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.claimMappings.scope') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.claimMappings.source') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.claimMappings.includeIn') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.claimMappings.enabled') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.actions') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="mapping in mappings"
            :key="mapping.id"
            class="hover:bg-neutral-50"
          >
            <td class="px-6 py-3">
              <span class="text-sm font-medium text-neutral-900 font-mono">{{ mapping.claim_name }}</span>
            </td>
            <td class="px-6 py-3">
              <span class="px-1.5 py-0.5 text-xs bg-brand-100 text-brand-700 rounded font-mono">{{ mapping.scope_name }}</span>
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500 font-mono">
              {{ mapping.source_path }}
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500">
              {{ mapping.include_in || 'both' }}
            </td>
            <td class="px-6 py-3">
              <input
                type="checkbox"
                :checked="mapping.enabled"
                :aria-label="t('admin.claimMappings.enabled')"
                @change="toggleEnabled(mapping)"
              >
            </td>
            <td class="px-6 py-3 text-sm space-x-3">
              <button
                class="text-brand-600 hover:text-brand-900 transition-colors"
                @click="openEditModal(mapping)"
              >
                {{ $t('common.edit') }}
              </button>
              <button
                class="text-error-600 hover:text-error-700 transition-colors"
                @click="deleteMapping(mapping)"
              >
                {{ $t('common.delete') }}
              </button>
            </td>
          </tr>
          <tr v-if="mappings.length === 0">
            <td
              colspan="6"
              class="px-6 py-12 text-center text-neutral-500"
            >
              {{ $t('admin.claimMappings.empty') }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <!-- Create modal -->
    <div
      v-if="showCreateModal"
      class="fixed inset-0 bg-black/50 flex items-center justify-center z-50"
      @click.self="showCreateModal = false"
    >
      <div class="bg-surface rounded-lg p-6 w-full max-w-md">
        <h3 class="text-lg font-semibold mb-4">
          {{ $t('admin.claimMappings.createTitle') }}
        </h3>
        <div class="space-y-4">
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.claimMappings.claim') }}</label>
            <input
              v-model="newMapping.claim_name"
              type="text"
              :placeholder="t('admin.claimMappings.claimPlaceholder')"
              class="w-full border border-neutral-300 rounded-md px-3 py-2 text-sm focus:ring-brand-500 focus:border-brand-500 font-mono"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.claimMappings.scope') }}</label>
            <input
              v-model="newMapping.scope_name"
              type="text"
              list="claim-scope-options"
              :placeholder="t('admin.claimMappings.scopePlaceholder')"
              class="w-full border border-neutral-300 rounded-md px-3 py-2 text-sm focus:ring-brand-500 focus:border-brand-500 font-mono"
            >
            <datalist id="claim-scope-options">
              <option
                v-for="name in scopeNames"
                :key="name"
                :value="name"
              />
            </datalist>
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.claimMappings.source') }}</label>
            <input
              v-model="newMapping.source_path"
              type="text"
              list="claim-source-options"
              :placeholder="t('admin.claimMappings.sourcePlaceholder')"
              class="w-full border border-neutral-300 rounded-md px-3 py-2 text-sm focus:ring-brand-500 focus:border-brand-500 font-mono"
            >
            <datalist id="claim-source-options">
              <option
                v-for="src in BUILTIN_SOURCES"
                :key="src"
                :value="src"
              />
              <option value="custom_attributes.department" />
            </datalist>
            <p class="mt-1 text-xs text-neutral-500">
              {{ t('admin.claimMappings.sourceHint') }}
            </p>
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.claimMappings.includeIn') }}</label>
            <select
              v-model="newMapping.include_in"
              class="w-full border border-neutral-300 rounded-md px-3 py-2 text-sm focus:ring-brand-500 focus:border-brand-500"
            >
              <option
                v-for="opt in INCLUDE_IN_OPTIONS"
                :key="opt"
                :value="opt"
              >
                {{ opt }}
              </option>
            </select>
          </div>
        </div>
        <div class="mt-6 flex justify-end space-x-3">
          <button
            class="px-4 py-2 border border-neutral-300 rounded-md text-sm text-neutral-700 hover:bg-neutral-50"
            @click="showCreateModal = false"
          >
            {{ $t('common.cancel') }}
          </button>
          <button
            :disabled="saving"
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            @click="createMapping"
          >
            {{ saving ? t('common.loading') : t('common.create') }}
          </button>
        </div>
      </div>
    </div>

    <!-- Edit modal -->
    <div
      v-if="showEditModal"
      class="fixed inset-0 bg-black/50 flex items-center justify-center z-50"
      @click.self="showEditModal = false"
    >
      <div class="bg-surface rounded-lg p-6 w-full max-w-md">
        <h3 class="text-lg font-semibold mb-4">
          {{ t('admin.claimMappings.editTitle', { name: editMapping.claim_name }) }}
        </h3>
        <div class="space-y-4">
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ t('admin.claimMappings.claim') }}</label>
            <input
              v-model="editMapping.claim_name"
              type="text"
              class="w-full border border-neutral-300 rounded-md px-3 py-2 text-sm focus:ring-brand-500 focus:border-brand-500 font-mono"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ t('admin.claimMappings.scope') }}</label>
            <input
              v-model="editMapping.scope_name"
              type="text"
              list="claim-scope-options"
              class="w-full border border-neutral-300 rounded-md px-3 py-2 text-sm focus:ring-brand-500 focus:border-brand-500 font-mono"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ t('admin.claimMappings.source') }}</label>
            <input
              v-model="editMapping.source_path"
              type="text"
              list="claim-source-options"
              class="w-full border border-neutral-300 rounded-md px-3 py-2 text-sm focus:ring-brand-500 focus:border-brand-500 font-mono"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ t('admin.claimMappings.includeIn') }}</label>
            <select
              v-model="editMapping.include_in"
              class="w-full border border-neutral-300 rounded-md px-3 py-2 text-sm focus:ring-brand-500 focus:border-brand-500"
            >
              <option
                v-for="opt in INCLUDE_IN_OPTIONS"
                :key="opt"
                :value="opt"
              >
                {{ opt }}
              </option>
            </select>
          </div>
          <div class="flex items-center gap-2">
            <input
              id="edit-enabled"
              v-model="editMapping.enabled"
              type="checkbox"
            >
            <label
              for="edit-enabled"
              class="text-sm font-medium text-neutral-700"
            >{{ t('admin.claimMappings.enabled') }}</label>
          </div>
        </div>
        <div class="mt-6 flex justify-end space-x-3">
          <button
            class="px-4 py-2 border border-neutral-300 rounded-md text-sm text-neutral-700 hover:bg-neutral-50"
            @click="showEditModal = false"
          >
            {{ t('common.cancel') }}
          </button>
          <button
            :disabled="saving"
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            @click="updateMapping"
          >
            {{ saving ? t('common.loading') : t('common.save') }}
          </button>
        </div>
      </div>
    </div>

    <AppConfirmDialog
      :open="confirmOpen"
      :message="confirmMessage"
      @confirm="runConfirm"
      @cancel="confirmOpen = false"
    />
  </div>
</template>
