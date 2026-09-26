<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '@/services/errorAdapter'
import { getErrorMessage } from '@/services/messages'
import AppEmptyState from '@/components/ui/AppEmptyState.vue'
import DData from '@/components/ui/DData.vue'
import AppConfirmDialog from '@/components/ui/AppConfirmDialog.vue'
import AppButton from '@/components/ui/AppButton.vue'

const { t } = useI18n()

interface Token {
  token_prefix: string
  client_id: string
  user_id: string
  scope: string
  created_at: string
  expires_at: string
}

const tokens = ref<Token[]>([])
const loading = ref(true)
const page = ref(1)
const perPage = ref(50)
const total = ref(0)
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen.
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})
const successMessage = ref('')

// Filters
const clientIdFilter = ref('')
const userIdFilter = ref('')

// Bulk action dropdown
const showBulkMenu = ref(false)

// #182: bulk "revoke all for user" — the target user id is typed directly in
// the bulk dropdown, independent of the results filter.
const bulkUserId = ref('')

// #181: the hand-rolled inline confirm modal is replaced by the shared
// AppConfirmDialog (Esc/backdrop/X map to cancel; testids confirm-dialog-*).
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

const uniqueClientIds = computed(() => {
  const ids = new Set(tokens.value.map(t => t.client_id).filter(Boolean))
  return Array.from(ids)
})

async function fetchTokens() {
  loading.value = true
  try {
    const params: Record<string, string | number> = { page: page.value, per_page: perPage.value }
    if (clientIdFilter.value) params.client_id = clientIdFilter.value
    if (userIdFilter.value) params.user_id = userIdFilter.value

    const resp = await axios.get('/api/admin/tokens', { params })
    tokens.value = resp.data.tokens || []
    total.value = resp.data.total || 0
  } catch (e) {
    const normalized = normalizeError(e)
    errorMessage.value = normalized
    console.error('Failed to fetch tokens:', e)
  } finally {
    loading.value = false
  }
}

function applyFilters() {
  page.value = 1
  fetchTokens()
}

function clearFilters() {
  clientIdFilter.value = ''
  userIdFilter.value = ''
  page.value = 1
  fetchTokens()
}

function showConfirm(message: string, action: () => Promise<void>) {
  askConfirm(message, action)
}

async function revokeToken(tokenPrefix: string) {
  showConfirm(t('admin.tokens.revokeTokenConfirm', { prefix: tokenPrefix }), async () => {
    // Each action owns the banner lifecycle: clear stale banners up front,
    // then show the outcome — never inside fetchTokens, which the actions
    // call afterwards (clearing there would wipe the just-set message).
    errorMessage.value = null
    successMessage.value = ''
    try {
      await axios.delete(`/api/admin/tokens/${tokenPrefix}`)
      await fetchTokens()
    } catch (e) {
      const normalized = normalizeError(e)
      errorMessage.value = normalized
    }
  })
}

async function revokeByClient(clientId: string) {
  showBulkMenu.value = false
  showConfirm(t('admin.tokens.revokeClientConfirm', { name: clientId }), async () => {
    errorMessage.value = null
    successMessage.value = ''
    try {
      const resp = await axios.post('/api/admin/tokens/revoke-by-client', { client_id: clientId })
      // Gap-fix: surface the backend count ("revoked N tokens") instead of a
      // silent success (the response carries the number of deleted rows).
      successMessage.value = t('admin.tokens.revokedCountForClient', { count: resp.data?.count ?? 0, name: clientId })
      await fetchTokens()
    } catch (e) {
      const normalized = normalizeError(e)
      errorMessage.value = normalized
    }
  })
}

// Core revocation, parameterized so both the bulk dropdown section and any
// future caller go through the same guarded path (#182).
async function doRevokeByUser(userId: string) {
  if (!userId) return
  errorMessage.value = null
  successMessage.value = ''
  try {
    const resp = await axios.post('/api/admin/tokens/revoke-by-user', { user_id: userId })
    successMessage.value = t('admin.tokens.revokedCountForUser', { count: resp.data?.count ?? 0, name: userId })
    await fetchTokens()
  } catch (e) {
    const normalized = normalizeError(e)
    errorMessage.value = normalized
  }
}

function revokeByUser() {
  showConfirm(t('admin.tokens.revokeUserConfirm', { name: bulkUserId.value.trim() }), async () => {
    await doRevokeByUser(bulkUserId.value.trim())
  })
}

function formatTime(ts: string) {
  if (!ts) return '—'
  // The backend returns expires_at/created_at as a Unix-epoch-seconds string
  // (e.g. "1751464800"); a numeric string parsed by new Date(string) yields
  // "Invalid Date". Detect epoch-seconds and convert to milliseconds. ISO 8601
  // strings (logs) pass through unchanged. See A-TOK-012.
  const asNum = Number(ts)
  if (!Number.isNaN(asNum) && /^-?\d+$/.test(ts.trim())) {
    try { return new Date(asNum * 1000).toLocaleString() } catch { return ts }
  }
  try { return new Date(ts).toLocaleString() } catch { return ts }
}

onMounted(fetchTokens)
</script>

<template>
  <div>
    <div class="flex justify-between items-center mb-6">
      <h2 class="text-2xl font-bold text-neutral-900">
        {{ $t('admin.tokens.title') }}
      </h2>
      <div class="relative">
        <button
          class="inline-flex items-center px-4 py-2 border border-neutral-300 rounded-md shadow-sm text-sm font-medium text-neutral-700 bg-surface hover:bg-neutral-50"
          @click="showBulkMenu = !showBulkMenu"
        >
          {{ $t('admin.tokens.revokeAll') }}
        </button>
        <div
          v-if="showBulkMenu"
          class="absolute right-0 mt-2 w-64 rounded-md shadow-lg bg-surface ring-1 ring-black ring-opacity-5 z-10"
        >
          <div class="py-1">
            <button
              v-for="cid in uniqueClientIds"
              :key="cid"
              class="block w-full text-left px-4 py-2 text-sm text-neutral-700 hover:bg-neutral-100"
              @click="revokeByClient(cid)"
            >
              {{ cid }}
            </button>
            <p
              v-if="uniqueClientIds.length === 0"
              class="px-4 py-2 text-sm text-neutral-400"
            >
              {{ $t('admin.tokens.noClientsInResults') }}
            </p>
          </div>
          <!-- #182: revoke all tokens for an explicitly typed user id -->
          <div class="border-t border-neutral-200 px-4 py-3 space-y-2">
            <label class="block text-xs font-medium text-neutral-500">
              {{ $t('admin.tokens.revokeAllForUserLabel') }}
            </label>
            <input
              v-model="bulkUserId"
              type="text"
              placeholder="user_id"
              class="w-full border border-neutral-300 rounded-md px-2 py-1.5 text-sm focus:ring-brand-500 focus:border-brand-500"
            >
            <AppButton
              variant="danger"
              size="sm"
              block
              :disabled="!bulkUserId.trim()"
              @click="revokeByUser"
            >
              {{ $t('admin.tokens.revokeAllForUser') }}
            </AppButton>
          </div>
        </div>
      </div>
    </div>

    <!-- Success Banner (bulk revocation count, gap-fix) -->
    <div
      v-if="successMessage"
      class="mb-6 rounded-md bg-success-50 p-4"
    >
      <p
        class="text-sm text-success-700"
        data-testid="tokens-success"
      >
        {{ successMessage }}
      </p>
    </div>

    <!-- Error Banner -->
    <div
      v-if="errorText"
      class="mb-6 rounded-md bg-error-50 p-4"
    >
      <div class="flex">
        <div class="flex-shrink-0">
          <svg
            class="h-5 w-5 text-error-500"
            viewBox="0 0 20 20"
            fill="currentColor"
          >
            <path
              fill-rule="evenodd"
              d="M10 18a8 8 0 100-16 8 8 0 000 16zM8.28 7.22a.75.75 0 00-1.06 1.06L8.94 10l-1.72 1.72a.75.75 0 101.06 1.06L10 11.06l1.72 1.72a.75.75 0 101.06-1.06L11.06 10l1.72-1.72a.75.75 0 00-1.06-1.06L10 8.94 8.28 7.22z"
              clip-rule="evenodd"
            />
          </svg>
        </div>
        <div class="ml-3">
          <p class="text-sm text-error-700">
            {{ errorText }}
          </p>
        </div>
      </div>
    </div>

    <!-- Filter bar -->
    <div class="bg-surface shadow rounded-lg p-4 mb-4 flex items-center gap-4 flex-wrap">
      <div class="flex items-center gap-2">
        <label class="text-sm font-medium text-neutral-700">{{ $t('admin.tokens.clientIdLabel') }}</label>
        <input
          v-model="clientIdFilter"
          type="text"
          :placeholder="$t('admin.tokens.clientFilterPlaceholder')"
          class="border border-neutral-300 rounded-md px-3 py-1.5 text-sm focus:ring-brand-500 focus:border-brand-500"
        >
      </div>
      <div class="flex items-center gap-2">
        <label class="text-sm font-medium text-neutral-700">{{ $t('admin.tokens.userIdLabel') }}</label>
        <input
          v-model="userIdFilter"
          type="text"
          :placeholder="$t('admin.tokens.userFilterPlaceholder')"
          class="border border-neutral-300 rounded-md px-3 py-1.5 text-sm focus:ring-brand-500 focus:border-brand-500"
        >
      </div>
      <button
        class="px-4 py-1.5 bg-brand-600 text-white text-sm font-medium rounded-md hover:bg-brand-700"
        @click="applyFilters"
      >
        {{ $t('common.apply') }}
      </button>
      <button
        class="px-4 py-1.5 border border-neutral-300 text-neutral-700 text-sm font-medium rounded-md hover:bg-neutral-50"
        @click="clearFilters"
      >
        {{ $t('common.clear') }}
      </button>
    </div>

    <!-- Loading state -->
    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ $t('common.loading') }}
    </div>

    <!-- Empty state -->
    <AppEmptyState
      v-else-if="tokens.length === 0"
      :title="$t('admin.tokens.emptyTitle')"
      :description="$t('admin.tokens.emptyDescription')"
    />

    <!-- Token table -->
    <div
      v-else
      class="bg-surface shadow rounded-lg overflow-hidden"
    >
      <table class="min-w-full divide-y divide-neutral-200">
        <thead class="bg-neutral-50">
          <tr>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.tokens.token') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.type') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.tokens.client') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.tokens.user') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.tokens.scope') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.tokens.expires') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.actions') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="token in tokens"
            :key="token.token_prefix"
            class="hover:bg-neutral-50"
          >
            <td class="px-4 py-3">
              <DData
                :value="token.token_prefix"
                label="tok"
              />
            </td>
            <td class="px-4 py-3 text-sm text-neutral-500">
              access
            </td>
            <td class="px-4 py-3">
              <DData
                :value="token.client_id || '—'"
                truncate
              />
            </td>
            <td class="px-4 py-3 text-sm text-neutral-500">
              {{ token.user_id || '—' }}
            </td>
            <td class="px-4 py-3 max-w-[200px]">
              <DData
                :value="token.scope || '—'"
                truncate
              />
            </td>
            <td class="px-4 py-3">
              <DData :value="formatTime(token.expires_at)" />
            </td>
            <td class="px-4 py-3">
              <button
                class="text-sm text-error-600 hover:text-error-700 font-medium rounded-ctl
                       focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring px-2"
                @click="revokeToken(token.token_prefix)"
              >
                {{ $t('admin.tokens.revoke') }}
              </button>
            </td>
          </tr>
        </tbody>
      </table>

      <!-- Pagination -->
      <div class="px-4 py-3 border-t flex justify-between items-center">
        <button
          :disabled="page <= 1"
          class="text-sm text-brand-600 disabled:text-neutral-400"
          @click="page > 1 && (page--, fetchTokens())"
        >
          {{ $t('common.previousArrow') }}
        </button>
        <span class="text-sm text-neutral-500">{{ $t('admin.tokens.pageTotal', { page, total }) }}</span>
        <button
          :disabled="tokens.length < perPage"
          class="text-sm text-brand-600 disabled:text-neutral-400"
          @click="page++; fetchTokens()"
        >
          {{ $t('common.nextArrow') }}
        </button>
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
