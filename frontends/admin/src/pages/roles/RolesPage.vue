<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'

const { t } = useI18n()

const roles = ref<any[]>([])
const loading = ref(true)
const showCreateModal = ref(false)
const showEditModal = ref(false)
const selectedRole = ref<any>(null)
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

const newRoleName = ref('')
const newRoleDescription = ref('')
const editDescription = ref('')

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

async function fetchRoles() {
  loading.value = true
  try {
    const resp = await axios.get('/api/admin/roles')
    roles.value = resp.data.roles || []
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function createRole() {
  if (!newRoleName.value.trim()) return
  saving.value = true
  try {
    await axios.post('/api/admin/roles', {
      name: newRoleName.value.trim(),
      description: newRoleDescription.value.trim(),
    }, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.roles.created', { name: newRoleName.value }))
    showCreateModal.value = false
    newRoleName.value = ''
    newRoleDescription.value = ''
    await fetchRoles()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

function openEditModal(role: any) {
  selectedRole.value = role
  editDescription.value = role.description || ''
  showEditModal.value = true
}

async function updateRole() {
  if (!selectedRole.value) return
  saving.value = true
  try {
    await axios.put(`/api/admin/roles/${selectedRole.value.id}`, {
      description: editDescription.value,
    }, { headers: { 'Content-Type': 'application/json' } })
    showSuccess(t('admin.roles.updated'))
    showEditModal.value = false
    await fetchRoles()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

function deleteRole(role: any) {
  askConfirm(t('admin.roles.deleteConfirm', { name: role.name }), async () => {
    try {
      await axios.delete(`/api/admin/roles/${role.id}`)
      showSuccess(t('admin.roles.deleted', { name: role.name }))
      await fetchRoles()
    } catch (e: unknown) {
      showError(normalizeError(e))
    }
  })
}

const BUILTIN_ROLES = ['admin', 'user']

onMounted(fetchRoles)
</script>

<template>
  <div>
    <div class="flex justify-between items-center mb-6">
      <h2 class="text-2xl font-bold text-neutral-900">
        {{ $t('admin.roles.title') }}
      </h2>
      <button
        class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700"
        @click="showCreateModal = true"
      >
        {{ $t('admin.roles.create') }}
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
              {{ $t('admin.roles.users') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.actions') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="role in roles"
            :key="role.id"
            class="hover:bg-neutral-50"
          >
            <td class="px-6 py-3">
              <div class="flex items-center gap-2">
                <span class="text-sm font-medium text-neutral-900">{{ role.name }}</span>
                <span
                  v-if="BUILTIN_ROLES.includes(role.name)"
                  class="px-1.5 py-0.5 text-xs bg-neutral-100 text-neutral-500 rounded"
                >{{ $t('common.builtin') }}</span>
              </div>
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500">
              {{ role.description || '—' }}
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500">
              {{ role.user_count }}
            </td>
            <td class="px-6 py-3 text-sm space-x-3">
              <button
                class="text-brand-600 hover:text-brand-900 transition-colors"
                @click="openEditModal(role)"
              >
                {{ $t('common.edit') }}
              </button>
              <button
                v-if="!BUILTIN_ROLES.includes(role.name)"
                class="text-error-600 hover:text-error-700 transition-colors"
                @click="deleteRole(role)"
              >
                {{ $t('common.delete') }}
              </button>
            </td>
          </tr>
          <tr v-if="roles.length === 0">
            <td
              colspan="4"
              class="px-6 py-12 text-center text-neutral-500"
            >
              {{ $t('admin.roles.noneFound') }}
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
          {{ $t('admin.roles.createTitle') }}
        </h3>
        <div class="space-y-4">
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.name') }} <span class="text-error-500">*</span></label>
            <input
              v-model="newRoleName"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
              :placeholder="$t('admin.roles.namePlaceholder')"
            >
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.description') }}</label>
            <input
              v-model="newRoleDescription"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
              :placeholder="$t('admin.roles.optionalDescription')"
            >
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
            :disabled="saving || !newRoleName.trim()"
            class="px-4 py-2 bg-brand-600 text-white rounded-md text-sm hover:bg-brand-700 disabled:opacity-50"
            @click="createRole"
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
          {{ $t('admin.roles.editTitle', { name: selectedRole?.name }) }}
        </h3>
        <div>
          <label class="block text-sm font-medium text-neutral-700 mb-1">{{ $t('common.description') }}</label>
          <input
            v-model="editDescription"
            class="block w-full px-3 py-2 border border-neutral-300 rounded-md text-sm"
            :placeholder="$t('admin.roles.optionalDescription')"
          >
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
            @click="updateRole"
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
