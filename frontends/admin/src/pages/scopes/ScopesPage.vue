<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'

const { t } = useI18n()

const scopes = ref<any[]>([])
const loading = ref(true)
const showCreateModal = ref(false)
const showEditModal = ref(false)
const selectedScope = ref<any>(null)
const saving = ref(false)
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

const newScope = ref({ name: '', description: '', mapped_role: '', is_default: false, requires_admin_role: false })
const editScope = ref({ description: '', mapped_role: '', is_default: false, requires_admin_role: false })

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

async function fetchScopes() {
  loading.value = true
  try {
    const resp = await axios.get('/api/admin/scopes')
    scopes.value = resp.data.scopes || []
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function createScope() {
  if (!newScope.value.name.trim()) return
  saving.value = true
  try {
    await axios.post('/api/admin/scopes', newScope.value, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.scopes.created', { name: newScope.value.name }))
    showCreateModal.value = false
    newScope.value = { name: '', description: '', mapped_role: '', is_default: false, requires_admin_role: false }
    await fetchScopes()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

function openEditModal(scope: any) {
  selectedScope.value = scope
  editScope.value = {
    description: scope.description || '',
    mapped_role: scope.mapped_role || '',
    is_default: scope.is_default || false,
    requires_admin_role: scope.requires_admin_role || false,
  }
  showEditModal.value = true
}

async function updateScope() {
  if (!selectedScope.value) return
  saving.value = true
  try {
    await axios.put(`/api/admin/scopes/${selectedScope.value.id}`, editScope.value, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.scopes.updated'))
    showEditModal.value = false
    await fetchScopes()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

function deleteScope(scope: any) {
  askConfirm(t('admin.scopes.deleteConfirm', { name: scope.name }), async () => {
    try {
      await axios.delete(`/api/admin/scopes/${scope.id}`)
      showSuccess(t('admin.scopes.deleted', { name: scope.name }))
      await fetchScopes()
    } catch (e: unknown) {
      showError(normalizeError(e))
    }
  })
}

// #43: system-seeded scopes are non-deletable (mirrors the backend's
// RoleScopeAdminService protected list + the V006 seed). The legacy bare
// 'read'/'write' are dropped; the resource-prefixed family is now built-in.
const BUILTIN_SCOPES = [
  'openid', 'profile', 'email', 'admin',
  'users:read', 'users:write', 'clients:read', 'clients:write',
  'tokens:read', 'tokens:write', 'roles:read', 'roles:write', 'audit:read',
]

onMounted(fetchScopes)
</script>

<template>
  <div>
    <div class="flex justify-between items-center mb-6">
      <h2 class="text-2xl font-bold text-neutral-900">
        {{ $t('admin.scopes.title') }}
      </h2>
      <button
        class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700"
        @click="showCreateModal = true"
      >
        {{ $t('admin.scopes.create') }}
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
              {{ $t('common.name') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.description') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.scopes.mappedRole') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.scopes.flags') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.actions') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="scope in scopes"
            :key="scope.id"
            class="hover:bg-neutral-50"
          >
            <td class="px-6 py-3">
              <div class="flex items-center gap-2">
                <span class="text-sm font-medium text-neutral-900 font-mono">{{ scope.name }}</span>
                <span
                  v-if="BUILTIN_SCOPES.includes(scope.name)"
                  class="px-1.5 py-0.5 text-xs bg-neutral-100 text-neutral-500 rounded"
                >{{ $t('common.builtin') }}</span>
              </div>
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500">
              {{ scope.description || '—' }}
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500">
              {{ scope.mapped_role || '—' }}
            </td>
            <td class="px-6 py-3">
              <div class="flex gap-1 flex-wrap">
                <span
                  v-if="scope.is_default"
                  class="px-1.5 py-0.5 text-xs bg-brand-100 text-brand-700 rounded"
                >{{ $t('admin.scopes.defaultBadge') }}</span>
                <span
                  v-if="scope.requires_admin_role"
                  class="px-1.5 py-0.5 text-xs bg-warning-100 text-warning-700 rounded"
                >{{ $t('admin.scopes.adminOnlyBadge') }}</span>
              </div>
            </td>
            <td class="px-6 py-3 text-sm space-x-3">
              <button
                class="text-brand-600 hover:text-brand-900 transition-colors"
                @click="openEditModal(scope)"
              >
                {{ $t('common.edit') }}
              </button>
              <button
                v-if="!BUILTIN_SCOPES.includes(scope.name)"
                class="text-error-600 hover:text-error-700 transition-colors"
                @click="deleteScope(scope)"
              >
                {{ $t('common.delete') }}
              </button>
            </td>
          </tr>
          <tr v-if="scopes.length === 0">
            <td
              colspan="5"
              class="px-6 py-12 text-center text-neutral-500"
            >
              {{ $t('admin.scopes.noneFound') }}
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
          {{ $t('admin.scopes.createTitle') }}
        </h3>
        <div class="space-y-4">
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.name') }} <span class="text-error-500">*</span></label>
            <input
              v-model="newScope.name"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm font-mono"
              :placeholder="$t('admin.scopes.namePlaceholder')"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.description') }}</label>
            <input
              v-model="newScope.description"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
              :placeholder="$t('admin.scopes.descriptionPlaceholder')"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.scopes.mappedRole') }}</label>
            <input
              v-model="newScope.mapped_role"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
              :placeholder="$t('admin.scopes.mappedRolePlaceholder')"
            >
          </div>
          <div class="flex gap-6">
            <label class="flex items-center gap-2 cursor-pointer">
              <input
                v-model="newScope.is_default"
                type="checkbox"
                class="h-4 w-4 rounded border-neutral-300 text-brand-600"
              >
              <span class="text-sm text-neutral-700">{{ $t('admin.scopes.defaultScope') }}</span>
            </label>
            <label class="flex items-center gap-2 cursor-pointer">
              <input
                v-model="newScope.requires_admin_role"
                type="checkbox"
                class="h-4 w-4 rounded border-neutral-300 text-brand-600"
              >
              <span class="text-sm text-neutral-700">{{ $t('common.requiresAdminRole') }}</span>
            </label>
          </div>
        </div>
        <div class="flex justify-end gap-3 mt-6">
          <button
            class="px-4 py-2 border border-neutral-300 rounded-md text-sm"
            @click="showCreateModal = false"
          >
            {{ $t('common.cancel') }}
          </button>
          <button
            :disabled="saving || !newScope.name.trim()"
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            @click="createScope"
          >
            {{ saving ? $t('common.creating') : $t('common.create') }}
          </button>
        </div>
      </div>
    </div>

    <!-- Edit Modal -->
    <div
      v-if="showEditModal"
      class="fixed inset-0 bg-black/50 flex items-center justify-center z-50"
    >
      <div class="bg-surface rounded-lg shadow-xl p-6 w-full max-w-md">
        <h3 class="text-lg font-semibold mb-4">
          {{ $t('admin.scopes.editTitle') }} <code class="font-mono text-brand-600">{{ selectedScope?.name }}</code>
        </h3>
        <div class="space-y-4">
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.description') }}</label>
            <input
              v-model="editScope.description"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('admin.scopes.mappedRole') }}</label>
            <input
              v-model="editScope.mapped_role"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
            >
          </div>
          <div class="flex gap-6">
            <label class="flex items-center gap-2 cursor-pointer">
              <input
                v-model="editScope.is_default"
                type="checkbox"
                class="h-4 w-4 rounded border-neutral-300 text-brand-600"
              >
              <span class="text-sm text-neutral-700">{{ $t('admin.scopes.defaultScope') }}</span>
            </label>
            <label class="flex items-center gap-2 cursor-pointer">
              <input
                v-model="editScope.requires_admin_role"
                type="checkbox"
                class="h-4 w-4 rounded border-neutral-300 text-brand-600"
              >
              <span class="text-sm text-neutral-700">{{ $t('common.requiresAdminRole') }}</span>
            </label>
          </div>
        </div>
        <div class="flex justify-end gap-3 mt-6">
          <button
            class="px-4 py-2 border border-neutral-300 rounded-md text-sm"
            @click="showEditModal = false"
          >
            {{ $t('common.cancel') }}
          </button>
          <button
            :disabled="saving"
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            @click="updateScope"
          >
            {{ saving ? $t('common.saving') : $t('common.save') }}
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
