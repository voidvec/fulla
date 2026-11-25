<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import http from '../../services/http'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppAlert from '../../components/ui/AppAlert.vue'
import AppBadge from '../../components/ui/AppBadge.vue'
import AppCard from '../../components/ui/AppCard.vue'
import AppModal from '../../components/ui/AppModal.vue'
import AppEmptyState from '../../components/ui/AppEmptyState.vue'
import DData from '../../components/ui/DData.vue'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'
import RequestOrgAuthModal from '../../components/account/RequestOrgAuthModal.vue'

const { t } = useI18n()
const apps = ref<any[]>([])
const loading = ref(true)
const error = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = error.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})
const success = ref('')
const showCreate = ref(false)
const creating = ref(false)
// Shown EXACTLY once after create/rotate (backend contract).
const oneTimeSecret = ref('')
const oneTimeSecretFor = ref('')
const newName = ref('')
const newType = ref<'PUBLIC' | 'CONFIDENTIAL'>('PUBLIC')
const newRedirectUris = ref('')
const newScopes = ref('openid profile')
// P1-1 (#279): optional brand image (https-only at the server).
const newLogoUri = ref('')

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

async function fetchApps() {
  loading.value = true
  try {
    const resp = await http.get('/api/me/applications')
    apps.value = resp.data?.applications || []
  } catch {
    error.value = t('account.applications.loadFailed')
  } finally {
    loading.value = false
  }
}

async function createApp() {
  creating.value = true
  error.value = null
  try {
    const uris = newRedirectUris.value.split(/[\s,]+/).filter(Boolean)
    const scopes = newScopes.value.split(/[\s,]+/).filter(Boolean)
    const body: any = {
      name: newName.value,
      client_type: newType.value,
    }
    if (uris.length) body.redirect_uris = uris
    if (scopes.length) body.scopes = scopes
    if (newLogoUri.value.trim()) body.logo_uri = newLogoUri.value.trim()
    const resp = await http.post('/api/me/applications', JSON.stringify(body), {
      headers: { 'Content-Type': 'application/json' },
    })
    oneTimeSecret.value = resp.data?.client_secret || ''
    oneTimeSecretFor.value = resp.data?.client_id || ''
    showCreate.value = false
    newName.value = ''
    success.value = t('account.applications.created')
    setTimeout(() => { success.value = '' }, 3000)
    await fetchApps()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  } finally {
    creating.value = false
  }
}

function rotateSecret(app: any) {
  askConfirm(t('account.applications.rotateConfirm', { app: app.name || app.client_id }), async () => {
    try {
      const resp = await http.post(`/api/me/applications/${app.client_id}/rotate-secret`, {})
      oneTimeSecret.value = resp.data?.client_secret || ''
      oneTimeSecretFor.value = app.client_id
      success.value = t('account.applications.rotated', { app: app.name || app.client_id })
      setTimeout(() => { success.value = '' }, 3000)
      // M11 (review): the list still shows the pre-rotation state — refresh.
      await fetchApps()
    } catch (e: unknown) {
      error.value = normalizeError(e)
    }
  })
}

function deleteApp(app: any) {
  askConfirm(t('account.applications.deleteConfirm', { app: app.name || app.client_id }), async () => {
    try {
      await http.delete(`/api/me/applications/${app.client_id}`)
      success.value = t('account.applications.deleted', { app: app.name || app.client_id })
      setTimeout(() => { success.value = '' }, 3000)
      await fetchApps()
    } catch (e: unknown) {
      error.value = normalizeError(e)
    }
  })
}

// T1.3 (UF-04a): edit modal — PATCH /api/me/applications/{clientId} with
// ONLY the dirty fields (absent keys stay unchanged per contract,
// minProperties 1; R5 e2e asserts a name-only edit sends just {name}).
// redirect_uris validate like the backend: https-only, loopback hosts
// exempt. A suspended app is management-frozen (the backend answers 403) so
// its edit entry is disabled up front. scopes/logo_uri are absent from the
// list response, so they are send-only-when-typed: blank keeps the stored
// value; the explicit clear checkbox sends "" (the contract's clear
// sentinel for logo_uri).
const GRANT_TYPES = ['authorization_code', 'refresh_token', 'client_credentials', 'device_code']

const editOpen = ref(false)
const savingEdit = ref(false)
const editingApp = ref<any>(null)
const editName = ref('')
const editRedirectUris = ref('')
const editGrants = ref<string[]>([])
const editScopes = ref('')
const editLogoUri = ref('')
const editClearLogo = ref(false)

function openEdit(app: any) {
  editingApp.value = app
  editName.value = app.name || ''
  editRedirectUris.value = (app.redirect_uris || []).join(', ')
  editGrants.value = [...(app.allowed_grant_types || ['authorization_code'])]
  editScopes.value = ''
  editLogoUri.value = ''
  editClearLogo.value = false
  editOpen.value = true
}

function toggleEditGrant(gt: string) {
  const index = editGrants.value.indexOf(gt)
  if (index >= 0) editGrants.value.splice(index, 1)
  else editGrants.value.push(gt)
}

// Backend-mirroring validation (F-014): every URI must parse and be https,
// except loopback hosts (plain-HTTP LAN/dev callback URIs are legal there).
function invalidRedirectUri(uris: string[]): string | null {
  for (const uri of uris) {
    let parsed: URL
    try {
      parsed = new URL(uri)
    } catch {
      return uri
    }
    const loopback = ['localhost', '127.0.0.1', '[::1]'].includes(parsed.hostname)
    if (parsed.protocol !== 'https:' && !loopback) return uri
  }
  return null
}

async function saveEdit() {
  if (!editingApp.value || savingEdit.value) return
  error.value = null
  const original = editingApp.value
  const uris = editRedirectUris.value.split(/[\s,]+/).filter(Boolean)
  const badUri = invalidRedirectUri(uris)
  if (badUri) {
    error.value = t('account.applications.invalidRedirectUri', { uri: badUri })
    return
  }
  if (editGrants.value.length === 0) {
    error.value = t('account.applications.grantsRequired')
    return
  }
  // Dirty-field body: only keys whose value differs from the row we opened
  // with (plus the send-when-typed scopes/logo_uri overrides).
  const body: Record<string, unknown> = {}
  if (editName.value !== (original.name || '')) body.name = editName.value
  const originalUris = (original.redirect_uris || []).join(', ')
  if (uris.join(', ') !== originalUris) body.redirect_uris = uris
  const originalGrants = [...(original.allowed_grant_types || ['authorization_code'])].sort().join(',')
  if ([...editGrants.value].sort().join(',') !== originalGrants) body.allowed_grant_types = [...editGrants.value]
  const typedScopes = editScopes.value.split(/[\s,]+/).filter(Boolean)
  if (typedScopes.length) body.scopes = typedScopes
  if (editClearLogo.value) body.logo_uri = ''
  else if (editLogoUri.value.trim()) body.logo_uri = editLogoUri.value.trim()

  if (Object.keys(body).length === 0) {
    // Nothing to send: contract requires minProperties 1 — close instead of
    // firing a doomed request.
    editOpen.value = false
    return
  }

  savingEdit.value = true
  try {
    await http.patch(`/api/me/applications/${original.client_id}`, JSON.stringify(body), {
      headers: { 'Content-Type': 'application/json' },
    })
    editOpen.value = false
    success.value = t('account.applications.updated', { app: original.name || original.client_id })
    setTimeout(() => { success.value = '' }, 3000)
    await fetchApps()
  } catch (e: unknown) {
    // A 403 here (suspension raced the open) renders the catalog copy via
    // the shared banner; the modal stays open so the state is visible.
    error.value = normalizeError(e)
  } finally {
    savingEdit.value = false
  }
}

// Member-side "request organization authorization": files a consent-request
// for one of this page's apps against an org the user belongs to. The org
// list is fetched lazily when the dialog opens — no pre-flight on page load;
// zero memberships swaps the form for an info message instead of an empty
// select.
const requestAuthOpen = ref(false)
const requestAuthApp = ref<any>(null)

function openRequestAuth(app: any) {
  requestAuthApp.value = app
  requestAuthOpen.value = true
}

function onRequestAuthSuccess(message: string) {
  success.value = message
  setTimeout(() => { success.value = '' }, 3000)
}

function onRequestAuthError(e: unknown) {
  error.value = normalizeError(e)
}

onMounted(fetchApps)
</script>

<template>
  <div>
    <div class="flex items-center justify-between mb-6">
      <h1 class="text-2xl font-bold text-neutral-900">
        {{ $t('account.applications.title') }}
      </h1>
      <button
        class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700 transition-colors
               focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        @click="showCreate = !showCreate"
      >
        {{ $t('account.applications.create') }}
      </button>
    </div>
    <p class="text-neutral-500 mb-6">
      {{ $t('account.applications.intro') }}
    </p>

    <AppAlert
      v-if="success"
      type="success"
      class="mb-4"
    >
      {{ success }}
    </AppAlert>
    <AppAlert
      v-if="errorText"
      type="error"
      class="mb-4"
    >
      {{ errorText }}
    </AppAlert>

    <AppCard
      v-if="oneTimeSecret"
      class="mb-6 border-2 border-warning-300"
    >
      <p class="font-medium text-neutral-900 mb-1">
        {{ $t('account.applications.secretTitle', { id: oneTimeSecretFor }) }}
      </p>
      <p class="text-sm text-neutral-500 mb-2">
        {{ $t('account.applications.secretOnce') }}
      </p>
      <DData :value="oneTimeSecret" />
      <button
        class="mt-2 text-sm underline text-neutral-400 hover:text-neutral-600"
        @click="oneTimeSecret = ''; oneTimeSecretFor = ''"
      >
        {{ $t('common.dismiss') }}
      </button>
    </AppCard>

    <AppCard
      v-if="showCreate"
      class="mb-6"
    >
      <form
        class="space-y-4"
        @submit.prevent="createApp"
      >
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.name') }}</label>
          <input
            v-model="newName"
            required
            maxlength="100"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.clientType') }}</label>
          <select
            v-model="newType"
            class="mt-1 rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
            <option value="PUBLIC">PUBLIC</option>
            <option value="CONFIDENTIAL">CONFIDENTIAL</option>
          </select>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.redirectUris') }}</label>
          <input
            v-model="newRedirectUris"
            :placeholder="$t('account.applications.redirectUrisPlaceholder')"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.scopes') }}</label>
          <input
            v-model="newScopes"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <!-- P1-1 (#279): brand image for the login/consent screens. -->
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.logoUri') }}</label>
          <input
            v-model="newLogoUri"
            :placeholder="$t('account.applications.logoUriPlaceholder')"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <button
          type="submit"
          :disabled="creating || !newName"
          class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                 disabled:opacity-50 transition-colors
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        >
          {{ $t('account.applications.submit') }}
        </button>
      </form>
    </AppCard>

    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ $t('common.loading') }}
    </div>

    <AppCard
      v-else-if="apps.length === 0"
      padding="none"
    >
      <AppEmptyState
        :title="$t('account.applications.emptyTitle')"
        :description="$t('account.applications.emptyDesc')"
      />
    </AppCard>

    <div
      v-else
      class="space-y-3"
    >
      <AppCard
        v-for="app in apps"
        :key="app.client_id"
        padding="sm"
      >
        <div class="flex items-center justify-between">
          <div>
            <p class="font-medium text-neutral-900">
              {{ app.name || app.client_id }}
            </p>
            <div class="flex items-center gap-1.5 mt-0.5">
              <span class="text-sm text-neutral-500">{{ $t('account.applications.clientId') }}</span>
              <DData :value="app.client_id" />
            </div>
            <div class="flex items-center gap-1.5 mt-1.5">
              <AppBadge
                :variant="app.client_type === 'PUBLIC' ? 'info' : 'neutral'"
                size="sm"
              >{{ app.client_type }}</AppBadge>
              <AppBadge
                :variant="app.status === 'active' ? 'success' : 'warning'"
                size="sm"
              >{{ app.status }}</AppBadge>
            </div>
            <div
              v-if="(app.redirect_uris || []).length"
              class="text-xs text-neutral-400 mt-1"
            >
              {{ app.redirect_uris.join(', ') }}
            </div>
          </div>
          <div class="flex items-center gap-2">
            <!-- T1.3 (UF-04a): edit — disabled for suspended apps (management
                 writes are frozen server-side; the tooltip says why). -->
            <button
              class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring disabled:opacity-50"
              data-testid="app-edit-button"
              :disabled="app.status === 'suspended'"
              :title="app.status === 'suspended' ? $t('account.applications.suspendedFrozen') : undefined"
              @click="openEdit(app)"
            >
              {{ $t('account.applications.edit') }}
            </button>
            <button
              class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
              data-testid="request-org-auth"
              @click="openRequestAuth(app)"
            >
              {{ $t('account.applications.requestOrgAuth') }}
            </button>
            <button
              v-if="app.client_type === 'CONFIDENTIAL'"
              class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
              @click="rotateSecret(app)"
            >
              {{ $t('account.applications.rotate') }}
            </button>
            <button
              class="px-3 py-1.5 text-sm text-error-600 border border-error-200 rounded-ctl hover:bg-error-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
              @click="deleteApp(app)"
            >
              {{ $t('account.applications.delete') }}
            </button>
          </div>
        </div>
      </AppCard>
    </div>

    <RequestOrgAuthModal
      :open="requestAuthOpen"
      :app="requestAuthApp"
      scope="account.applications"
      @close="requestAuthOpen = false"
      @success="onRequestAuthSuccess"
      @error="onRequestAuthError"
    />

    <AppConfirmDialog
      :open="confirmOpen"
      :message="confirmMessage"
      danger
      @confirm="runConfirm"
      @cancel="confirmOpen = false"
    />

    <!-- T1.3 (UF-04a): edit modal. Sends PATCH with only the dirty fields —
         absent keys stay unchanged on the server (partial semantics). -->
    <AppModal
      :open="editOpen"
      :title="$t('account.applications.editTitle', { app: editingApp?.name || editingApp?.client_id })"
      size="md"
      @close="editOpen = false"
    >
      <form
        class="space-y-4"
        data-testid="app-edit-form"
        @submit.prevent="saveEdit"
      >
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.name') }}</label>
          <input
            v-model="editName"
            required
            maxlength="100"
            data-testid="app-edit-name"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.redirectUris') }}</label>
          <input
            v-model="editRedirectUris"
            :placeholder="$t('account.applications.redirectUrisPlaceholder')"
            data-testid="app-edit-redirect-uris"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.grants') }}</label>
          <div class="mt-1 flex flex-wrap gap-3">
            <label
              v-for="gt in GRANT_TYPES"
              :key="gt"
              class="flex items-center gap-1.5 text-sm text-neutral-700"
            >
              <input
                type="checkbox"
                :checked="editGrants.includes(gt)"
                :data-testid="`app-edit-grant-${gt}`"
                class="rounded border-neutral-300"
                @change="toggleEditGrant(gt)"
              >
              {{ gt }}
            </label>
          </div>
          <p class="mt-1 text-xs text-neutral-400">
            {{ $t('account.applications.grantsHint') }}
          </p>
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.scopes') }}</label>
          <input
            v-model="editScopes"
            :placeholder="$t('account.applications.editScopesHint')"
            data-testid="app-edit-scopes"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.logoUri') }}</label>
          <input
            v-model="editLogoUri"
            :disabled="editClearLogo"
            :placeholder="$t('account.applications.editLogoHint')"
            data-testid="app-edit-logo-uri"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface disabled:opacity-50
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
          <label class="mt-1 flex items-center gap-1.5 text-xs text-neutral-500">
            <input
              v-model="editClearLogo"
              type="checkbox"
              data-testid="app-edit-clear-logo"
              class="rounded border-neutral-300"
            >
            {{ $t('account.applications.clearLogo') }}
          </label>
        </div>
        <div class="flex justify-end gap-2 pt-2">
          <button
            type="button"
            class="px-4 py-2 text-sm text-neutral-600 border border-neutral-300 rounded-ctl hover:bg-neutral-50"
            @click="editOpen = false"
          >
            {{ $t('common.cancel') }}
          </button>
          <button
            type="submit"
            :disabled="savingEdit || !editName"
            data-testid="app-edit-submit"
            class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                   disabled:opacity-50 transition-colors
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
            {{ savingEdit ? $t('account.applications.saving') : $t('common.save') }}
          </button>
        </div>
      </form>
    </AppModal>
  </div>
</template>
