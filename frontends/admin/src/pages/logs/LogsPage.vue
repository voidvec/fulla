<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '@/services/errorAdapter'
import { getErrorMessage } from '@/services/messages'
import AppEmptyState from '@/components/ui/AppEmptyState.vue'
import DData from '@/components/ui/DData.vue'

const logs = ref<any[]>([])
const loading = ref(true)
const page = ref(1)
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen.
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

// Filters (A-LOG-004). action/outcome/actor_id are passed as query params; the
// backend applies them server-side (gap-fix: actor_id filter existed on the
// backend but had no input here).
const actionFilter = ref('')
const outcomeFilter = ref('')
const actorIdFilter = ref('')

// CSV export (P1-6 #283 — audit wave B): the export endpoint requires an
// explicit time window (from/to, epoch seconds) unless all=true is sent.
// The inputs are datetime-local strings; empty inputs default to the last
// 24 hours so a one-click export never trips WINDOW_REQUIRED by accident.
const exportFrom = ref('')
const exportTo = ref('')
const exporting = ref(false)

function exportWindowSeconds(): { from: number; to: number } {
  const now = Math.floor(Date.now() / 1000)
  const from = exportFrom.value
    ? Math.floor(new Date(exportFrom.value).getTime() / 1000)
    : now - 24 * 3600
  const to = exportTo.value
    ? Math.floor(new Date(exportTo.value).getTime() / 1000)
    : now
  return { from, to }
}

// A blob request's error body arrives as a Blob, which normalizeError cannot
// parse (its envelope detection expects an object) — decode it back to the
// Error Envelope shape first so the catalog code (e.g.
// AUDIT_EXPORT_WINDOW_REQUIRED / AUDIT_EXPORT_TOO_LARGE) resolves normally.
async function normalizeBlobError(e: unknown): Promise<NormalizedError | string> {
  const data = (e as { response?: { data?: unknown } })?.response?.data
  if (data instanceof Blob) {
    try {
      const parsed = JSON.parse(await data.text())
      return normalizeError({ response: { status: (e as { response?: { status?: number } })?.response?.status, data: parsed } })
    } catch {
      // fall through to the generic normalization below
    }
  }
  return normalizeError(e)
}

async function exportCsv() {
  if (exporting.value) return
  exporting.value = true
  errorMessage.value = null
  const { from, to } = exportWindowSeconds()
  try {
    const params: Record<string, string | number> = { from, to }
    if (actionFilter.value) params.action = actionFilter.value
    if (outcomeFilter.value) params.outcome = outcomeFilter.value
    if (actorIdFilter.value) params.actor_id = actorIdFilter.value
    const resp = await axios.get('/api/admin/logs/export', { params, responseType: 'blob' })
    const url = URL.createObjectURL(resp.data as Blob)
    const link = document.createElement('a')
    link.href = url
    link.download = `audit-export-${from}-${to}.csv`
    document.body.appendChild(link)
    link.click()
    link.remove()
    URL.revokeObjectURL(url)
  } catch (e) {
    errorMessage.value = await normalizeBlobError(e)
  } finally {
    exporting.value = false
  }
}

async function fetchLogs() {
  loading.value = true
  errorMessage.value = null
  try {
    const params: Record<string, string | number> = { page: page.value, per_page: 50 }
    if (actionFilter.value) params.action = actionFilter.value
    if (outcomeFilter.value) params.outcome = outcomeFilter.value
    if (actorIdFilter.value) params.actor_id = actorIdFilter.value
    const resp = await axios.get('/api/admin/logs', { params })
    logs.value = resp.data.logs || []
  } catch (e) {
    const normalized = normalizeError(e)
    errorMessage.value = normalized
  } finally {
    loading.value = false
  }
}

function applyFilters() {
  page.value = 1
  fetchLogs()
}

function clearFilters() {
  actionFilter.value = ''
  outcomeFilter.value = ''
  actorIdFilter.value = ''
  page.value = 1
  fetchLogs()
}

function formatTime(ts: string) {
  if (!ts) return '—'
  try { return new Date(ts).toLocaleString() } catch { return ts }
}

onMounted(fetchLogs)
</script>

<template>
  <div>
    <h2 class="text-2xl font-bold text-neutral-900 mb-6">
      {{ $t('admin.logs.title') }}
    </h2>

    <!-- Filter bar -->
    <div class="bg-surface shadow rounded-lg p-4 mb-4 flex items-center gap-4 flex-wrap">
      <div class="flex items-center gap-2">
        <label class="text-sm font-medium text-neutral-700">{{ $t('admin.logs.actionLabel') }}</label>
        <input
          v-model="actionFilter"
          type="text"
          :placeholder="$t('admin.logs.actionPlaceholder')"
          class="border border-neutral-300 rounded-md px-3 py-1.5 text-sm focus:ring-brand-500 focus:border-brand-500"
          @keyup.enter="applyFilters"
        >
      </div>
      <div class="flex items-center gap-2">
        <label class="text-sm font-medium text-neutral-700">{{ $t('admin.logs.outcomeLabel') }}</label>
        <select
          v-model="outcomeFilter"
          class="border border-neutral-300 rounded-md px-3 py-1.5 text-sm focus:ring-brand-500 focus:border-brand-500"
        >
          <option value="">
            {{ $t('admin.logs.anyOutcome') }}
          </option>
          <option value="success">
            success
          </option>
          <option value="failure">
            failure
          </option>
        </select>
      </div>
      <div class="flex items-center gap-2">
        <label class="text-sm font-medium text-neutral-700">{{ $t('admin.logs.actorIdLabel') }}</label>
        <input
          v-model="actorIdFilter"
          type="text"
          :placeholder="$t('admin.logs.actorPlaceholder')"
          class="border border-neutral-300 rounded-md px-3 py-1.5 text-sm focus:ring-brand-500 focus:border-brand-500"
          @keyup.enter="applyFilters"
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
      <!-- CSV export (P1-6): window defaults to the last 24h when empty -->
      <div class="flex items-center gap-2 ml-auto">
        <label class="text-sm font-medium text-neutral-700">{{ $t('admin.logs.exportFromLabel') }}</label>
        <input
          v-model="exportFrom"
          type="datetime-local"
          class="border border-neutral-300 rounded-md px-2 py-1.5 text-sm focus:ring-brand-500 focus:border-brand-500"
        >
        <label class="text-sm font-medium text-neutral-700">{{ $t('admin.logs.exportToLabel') }}</label>
        <input
          v-model="exportTo"
          type="datetime-local"
          class="border border-neutral-300 rounded-md px-2 py-1.5 text-sm focus:ring-brand-500 focus:border-brand-500"
        >
        <button
          :disabled="exporting"
          class="px-4 py-1.5 border border-brand-600 text-brand-600 text-sm font-medium rounded-md hover:bg-brand-50 disabled:opacity-50"
          @click="exportCsv"
        >
          {{ exporting ? $t('common.loading') : $t('admin.logs.exportLabel') }}
        </button>
      </div>
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

    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ $t('common.loading') }}
    </div>

    <AppEmptyState
      v-else-if="logs.length === 0"
      :title="$t('admin.logs.emptyTitle')"
      :description="$t('admin.logs.emptyDescription')"
    />

    <div
      v-else
      class="bg-surface shadow rounded-lg overflow-hidden"
    >
      <table class="min-w-full divide-y divide-neutral-200">
        <thead class="bg-neutral-50">
          <tr>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.logs.time') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.logs.action') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.logs.actor') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.logs.target') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.logs.outcome') }}
            </th>
            <th class="px-4 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.logs.ip') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="log in logs"
            :key="log.id"
            class="hover:bg-neutral-50"
          >
            <td class="px-4 py-3">
              <DData :value="formatTime(log.timestamp)" />
            </td>
            <td class="px-4 py-3 text-sm font-medium text-neutral-900">
              {{ log.action }}
            </td>
            <td class="px-4 py-3">
              <DData
                :label="log.actor_type ? `${log.actor_type}:` : undefined"
                :value="log.actor_id?.substring(0, 12) || '—'"
              />
            </td>
            <td class="px-4 py-3">
              <DData
                v-if="log.target_type || log.target_id"
                :label="log.target_type || undefined"
                :value="log.target_id?.substring(0, 8) || '—'"
                :title="`${log.target_type || ''}:${log.target_id || ''}`"
              />
              <span v-else>—</span>
            </td>
            <td class="px-4 py-3">
              <span
                class="px-2 py-0.5 text-xs rounded-full"
                :class="log.outcome === 'success' ? 'bg-success-100 text-success-700' : 'bg-error-100 text-error-700'"
              >
                {{ log.outcome }}
              </span>
            </td>
            <td class="px-4 py-3">
              <DData :value="log.ip || '—'" />
            </td>
          </tr>
        </tbody>
      </table>

      <div class="px-4 py-3 border-t flex justify-between items-center">
        <button
          :disabled="page <= 1"
          class="text-sm text-brand-600 disabled:text-neutral-400"
          @click="page > 1 && (page--, fetchLogs())"
        >
          {{ $t('common.previousArrow') }}
        </button>
        <span class="text-sm text-neutral-500">{{ $t('admin.logs.pageOf', { page }) }}</span>
        <button
          :disabled="logs.length < 50"
          class="text-sm text-brand-600 disabled:text-neutral-400"
          @click="page++; fetchLogs()"
        >
          {{ $t('common.nextArrow') }}
        </button>
      </div>
    </div>
  </div>
</template>
