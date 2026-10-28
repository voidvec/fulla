<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'

// P1-4 (#281): org management console — edit (name/logo/color), member
// roster with removal, ownership transfer, soft delete with the closed
// cascade.
const route = useRoute()
const { t } = useI18n()
const slug = route.params.slug as string

const loading = ref(true)
const saving = ref(false)
const successMessage = ref('')
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

const org = ref<any>(null)
const editForm = ref({ name: '', logo_uri: '', primary_color: '' })
const members = ref<any[]>([])

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

async function fetchOrg() {
  loading.value = true
  try {
    const resp = await axios.get(`/api/admin/organizations/${slug}`)
    org.value = resp.data
    editForm.value = {
      name: resp.data.name || '',
      logo_uri: resp.data.logo_uri || '',
      primary_color: resp.data.primary_color || '',
    }
  } catch (e) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function fetchMembers() {
  try {
    const resp = await axios.get(`/api/admin/organizations/${slug}/members`)
    members.value = resp.data.members ?? []
  } catch {
    members.value = []
  }
}

async function saveChanges() {
  saving.value = true
  try {
    const body: any = {}
    if (editForm.value.name !== (org.value.name || '')) body.name = editForm.value.name
    if (editForm.value.logo_uri !== (org.value.logo_uri || '')) body.logo_uri = editForm.value.logo_uri
    if (editForm.value.primary_color !== (org.value.primary_color || ''))
      body.primary_color = editForm.value.primary_color
    if (Object.keys(body).length === 0) {
      showSuccess(t('admin.organizations.orgDetail.noChanges'))
      saving.value = false
      return
    }
    await axios.patch(`/api/admin/organizations/${slug}`, body, {
      headers: { 'Content-Type': 'application/json' },
    })
    showSuccess(t('admin.organizations.orgDetail.saved'))
    await fetchOrg()
  } catch (e) {
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

function removeMember(m: any) {
  askConfirm(
    t('admin.organizations.orgDetail.removeConfirm', { user: m.username || m.user_id }),
    async () => {
      try {
        await axios.delete(`/api/admin/organizations/${slug}/members/${m.user_id}`)
        showSuccess(t('admin.organizations.orgDetail.memberRemoved'))
        await fetchMembers()
      } catch (e) {
        showError(normalizeError(e))
      }
    },
  )
}

const transferUserId = ref('')
function transferOwnership() {
  const uid = Number(transferUserId.value)
  if (!Number.isInteger(uid) || uid <= 0) {
    showError(t('admin.organizations.orgDetail.transferInvalid'))
    return
  }
  askConfirm(t('admin.organizations.orgDetail.transferConfirm', { id: transferUserId.value }), async () => {
    try {
      await axios.post(
        `/api/admin/organizations/${slug}/transfer-ownership`,
        { user_id: uid },
        { headers: { 'Content-Type': 'application/json' } },
      )
      showSuccess(t('admin.organizations.orgDetail.transferred'))
      transferUserId.value = ''
      await fetchMembers()
    } catch (e) {
      showError(normalizeError(e))
    }
  })
}

function deleteOrg() {
  askConfirm(t('admin.organizations.orgDetail.deleteConfirm', { org: org.value?.name || slug }), async () => {
    try {
      await axios.delete(`/api/admin/organizations/${slug}`)
      window.location.href = '/admin/organizations'
    } catch (e) {
      showError(normalizeError(e))
    }
  })
}

onMounted(async () => {
  await Promise.all([fetchOrg(), fetchMembers()])
})
</script>

<template>
  <div class="p-6">
    <h1 class="text-2xl font-bold mb-4">{{ org?.name || slug }}</h1>
    <p class="text-sm text-neutral-500 mb-4 font-mono">{{ slug }}</p>

    <div v-if="successMessage" class="alert alert-success mb-4">{{ successMessage }}</div>
    <div v-if="errorText" class="alert alert-error mb-4">{{ errorText }}</div>

    <div v-if="loading" class="loading loading-spinner" />

    <template v-else>
      <!-- Edit -->
      <div class="bg-surface shadow rounded-lg p-6 mb-6">
        <h2 class="text-lg font-semibold mb-4">{{ t('admin.organizations.orgDetail.editTitle') }}</h2>
        <div class="grid grid-cols-1 gap-4">
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ t('admin.organizations.orgDetail.name') }}</label>
            <input v-model="editForm.name" class="input input-bordered w-full" />
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">Logo URI</label>
            <input v-model="editForm.logo_uri" class="input input-bordered w-full font-mono text-sm" placeholder="https://cdn.example.com/logo.svg" />
          </div>
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ t('admin.organizations.orgDetail.primaryColor') }}</label>
            <input v-model="editForm.primary_color" class="input input-bordered w-40 font-mono text-sm" placeholder="#4f81c4" />
          </div>
        </div>
        <button class="btn btn-primary mt-4" :disabled="saving" @click="saveChanges">
          <span v-if="saving" class="loading loading-spinner loading-xs" />
          {{ t('common.save') }}
        </button>
      </div>

      <!-- Members -->
      <div class="bg-surface shadow rounded-lg p-6 mb-6">
        <h2 class="text-lg font-semibold mb-4">{{ t('admin.organizations.orgDetail.membersTitle') }}</h2>
        <table class="table table-zebra">
          <thead>
            <tr>
              <th>{{ t('admin.organizations.orgDetail.userId') }}</th>
              <th>{{ t('admin.organizations.orgDetail.username') }}</th>
              <th>{{ t('admin.organizations.orgDetail.role') }}</th>
              <th class="text-right">{{ t('common.actions') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="m in members" :key="m.user_id">
              <td class="font-mono text-sm">{{ m.user_id }}</td>
              <td>{{ m.username || '—' }}</td>
              <td><span class="badge" :class="m.role === 'owner' ? 'badge-primary' : 'badge-ghost'">{{ m.role }}</span></td>
              <td class="text-right">
                <button class="btn btn-xs btn-error btn-outline" @click="removeMember(m)">
                  {{ t('admin.organizations.orgDetail.remove') }}
                </button>
              </td>
            </tr>
            <tr v-if="members.length === 0">
              <td colspan="4" class="text-center text-neutral-400 py-6">{{ t('admin.organizations.orgDetail.noMembers') }}</td>
            </tr>
          </tbody>
        </table>

        <!-- Transfer -->
        <div class="flex items-end gap-2 mt-4 pt-4 border-t">
          <div>
            <label class="block text-sm font-medium text-neutral-700 mb-1">{{ t('admin.organizations.orgDetail.transferTo') }}</label>
            <input v-model="transferUserId" type="number" class="input input-bordered w-40" placeholder="123" />
          </div>
          <button class="btn btn-outline" @click="transferOwnership">{{ t('admin.organizations.orgDetail.transfer') }}</button>
        </div>
      </div>

      <!-- Danger zone -->
      <div class="bg-surface shadow rounded-lg p-6 border border-error-200">
        <h2 class="text-lg font-semibold text-error-600 mb-2">{{ t('admin.organizations.orgDetail.dangerTitle') }}</h2>
        <p class="text-sm text-neutral-500 mb-4">{{ t('admin.organizations.orgDetail.dangerHint') }}</p>
        <button class="btn btn-error" @click="deleteOrg">{{ t('admin.organizations.orgDetail.delete') }}</button>
      </div>
    </template>

    <AppConfirmDialog
      :open="confirmOpen"
      :message="confirmMessage"
      @confirm="runConfirm"
      @cancel="confirmOpen = false"
    />
  </div>
</template>
