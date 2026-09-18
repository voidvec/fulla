<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppEmptyState from '../../components/ui/AppEmptyState.vue'

const { t } = useI18n()
const orgs = ref<any[]>([])
const loading = ref(true)
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})
const success = ref('')
const showCreate = ref(false)
const creating = ref(false)
const createForm = ref({ slug: '', name: '', logo_uri: '', primary_color: '' })

function showError(msg: NormalizedError | string) {
  errorMessage.value = msg
  setTimeout(() => { errorMessage.value = null }, 5000)
}

async function fetchOrgs() {
  loading.value = true
  try {
    const resp = await axios.get('/api/admin/organizations')
    orgs.value = resp.data.organizations || []
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

async function createOrg() {
  creating.value = true
  try {
    await axios.post('/api/admin/organizations', createForm.value, {
      headers: { 'Content-Type': 'application/json' },
    })
    success.value = t('admin.organizations.created')
    setTimeout(() => { success.value = '' }, 3000)
    showCreate.value = false
    createForm.value = { slug: '', name: '', logo_uri: '', primary_color: '' }
    await fetchOrgs()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally {
    creating.value = false
  }
}

onMounted(fetchOrgs)
</script>

<template>
  <div>
    <div class="flex justify-between items-center mb-6">
      <h2 class="text-2xl font-bold text-neutral-900">
        {{ $t('admin.organizations.title') }}
      </h2>
      <button
        class="px-4 py-2 bg-brand-600 text-white rounded-md hover:bg-brand-700 text-sm font-medium"
        @click="showCreate = !showCreate"
      >
        {{ $t('admin.organizations.create') }}
      </button>
    </div>
    <p class="text-sm text-neutral-500 mb-6">
      {{ $t('admin.organizations.intro') }}
    </p>

    <div
      v-if="success"
      class="mb-4 p-3 bg-success-50 border border-success-200 text-success-700 rounded-md text-sm"
    >
      {{ success }}
    </div>
    <div
      v-if="errorText"
      class="mb-4 p-3 bg-error-50 border border-error-200 text-error-700 rounded-md text-sm"
    >
      {{ errorText }}
    </div>

    <div
      v-if="showCreate"
      class="mb-6 bg-surface shadow rounded-lg p-6"
    >
      <form
        class="grid grid-cols-1 md:grid-cols-2 gap-4"
        @submit.prevent="createOrg"
      >
        <div>
          <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.organizations.slug') }}</label>
          <input
            v-model="createForm.slug"
            required
            pattern="[a-z0-9][a-z0-9-]{1,48}[a-z0-9]"
            class="mt-1 w-full rounded-md border border-neutral-300 px-3 py-2 text-sm"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.organizations.name') }}</label>
          <input
            v-model="createForm.name"
            required
            maxlength="100"
            class="mt-1 w-full rounded-md border border-neutral-300 px-3 py-2 text-sm"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.organizations.logoUri') }}</label>
          <input
            v-model="createForm.logo_uri"
            class="mt-1 w-full rounded-md border border-neutral-300 px-3 py-2 text-sm"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-700">{{ $t('admin.organizations.primaryColor') }}</label>
          <input
            v-model="createForm.primary_color"
            class="mt-1 w-full rounded-md border border-neutral-300 px-3 py-2 text-sm"
          >
        </div>
        <div class="md:col-span-2">
          <button
            type="submit"
            :disabled="creating"
            class="px-4 py-2 bg-brand-600 text-white rounded-md hover:bg-brand-700 text-sm font-medium disabled:opacity-50"
          >
            {{ creating ? t('common.saving') : t('admin.organizations.submit') }}
          </button>
        </div>
      </form>
    </div>

    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ t('common.loading') }}
    </div>

    <AppEmptyState
      v-else-if="orgs.length === 0"
      :title="$t('admin.organizations.emptyTitle')"
      :description="$t('admin.organizations.emptyDescription')"
    />

    <div
      v-else
      class="bg-surface shadow rounded-lg overflow-hidden"
    >
      <table class="min-w-full divide-y divide-neutral-200">
        <thead class="bg-neutral-50">
          <tr>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.organizations.name') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.organizations.slug') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.organizations.issuerOverride') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="org in orgs"
            :key="org.id"
            class="hover:bg-neutral-50"
          >
            <td class="px-6 py-3 text-sm font-medium text-neutral-900">
              {{ org.name }}
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500 font-mono text-xs">
              {{ org.slug }}
            </td>
            <td class="px-6 py-3 text-sm text-neutral-500">
              {{ org.issuer_override || '—' }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>
