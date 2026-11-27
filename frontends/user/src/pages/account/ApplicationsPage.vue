<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
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
// T2.2 (UF-04c): the application's owner (personal, or an organization the
// caller manages) and the grant types it may use. The form sends what it
// shows: the server's default when allowed_grant_types is left out is
// authorization_code alone, which is also the form's initial state.
const newOrgSlug = ref('')
const newGrants = ref<string[]>(['authorization_code'])

// The only owners an application may be created under or transferred to:
// the caller's own account, or an organization where they are owner/admin
// (ApplicationService::isManagerRole; the server refuses any other slug the
// same way it refuses an unknown one). Loaded once per page. The names are
// decoration on the cards, so a failed load is quiet there and is reported
// only by an action that needs the list.
const manageableOrgs = ref<{ id: number; slug: string; name: string; role: string }[]>([])
let orgsRequest: Promise<boolean> | null = null
const orgsError = ref<NormalizedError | string | null>(null)

function loadManageableOrgs(): Promise<boolean> {
  if (!orgsRequest) {
    orgsError.value = null
    orgsRequest = http.get('/api/me/organizations')
      .then((resp) => {
        const all = resp.data?.organizations || []
        manageableOrgs.value = all.filter((o: any) => o.role === 'owner' || o.role === 'admin')
        return true
      })
      .catch((e: unknown) => {
        orgsError.value = normalizeError(e)
        // Not memoized: the next action that needs the list asks again.
        orgsRequest = null
        return false
      })
  }
  return orgsRequest
}

function ownerSlugOf(app: any): string {
  return manageableOrgs.value.find((o) => o.id === app.org_id)?.slug ?? ''
}

function ownerNameOf(app: any): string {
  if (app.org_id == null) return t('account.applications.ownerPersonal')
  return manageableOrgs.value.find((o) => o.id === app.org_id)?.name || t('account.applications.ownerOrg')
}

// At least one grant type has to stay selected (the server rejects an empty
// list), so the last one checked cannot be cleared.
function toggleNewGrant(gt: string) {
  const selected = newGrants.value.includes(gt)
  if (selected && newGrants.value.length === 1) return
  newGrants.value = selected
    ? newGrants.value.filter((g) => g !== gt)
    : [...newGrants.value, gt]
}

// The create form opens: make sure the owner list is there.
watch(showCreate, (open) => {
  if (open) void loadManageableOrgs()
})

// client_credentials needs a client secret, which a PUBLIC client cannot
// hold; drop it when the type rules it out.
watch(newType, (type) => {
  if (type === 'PUBLIC') newGrants.value = newGrants.value.filter((g) => g !== 'client_credentials')
})

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
    body.allowed_grant_types = [...newGrants.value]
    if (newOrgSlug.value) body.org_slug = newOrgSlug.value
    const resp = await http.post('/api/me/applications', JSON.stringify(body), {
      headers: { 'Content-Type': 'application/json' },
    })
    oneTimeSecret.value = resp.data?.client_secret || ''
    oneTimeSecretFor.value = resp.data?.client_id || ''
    showCreate.value = false
    newName.value = ''
    newOrgSlug.value = ''
    newGrants.value = ['authorization_code']
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

// T2.2 (UF-04b): move the management anchor — personal <-> an organization
// the caller manages. The server only supports personal -> org and
// org -> personal (rejects org -> org with 400), and keeps the client ID,
// existing consents and issued tokens; returning an org app to personal
// ownership preserves creator_user_id, so another admin loses access.
const myUsername = ref('')
let meRequest: Promise<string> | null = null
function loadMyUsername(): Promise<string> {
  if (!meRequest) {
    meRequest = http.get('/api/me')
      .then((resp) => {
        myUsername.value = resp.data?.username || ''
        return myUsername.value
      })
      .catch(() => {
        meRequest = null
        return ''
      })
  }
  return meRequest
}

const transferOpen = ref(false)
const transferring = ref(false)
const transferringApp = ref<any>(null)
// '' = personal ownership (org -> personal) or placeholder (personal -> org),
// otherwise an organization slug.
const transferTarget = ref('')
const transferCreatorLoading = ref(false)
const transferCreatorKind = ref<'self' | 'other' | 'unknown'>('unknown')
const transferCreatorLabel = ref('')
let transferInspectSeq = 0

// Only personal -> org lists organizations; org -> personal never lists other orgs.
const transferTargets = computed(() =>
  transferringApp.value?.org_id == null ? manageableOrgs.value : [])

// Personal -> org requires choosing an organization; org -> personal only has
// one option ('' -> org_slug: null) and is immediately ready once resolved.
const transferUnchanged = computed(() => {
  const app = transferringApp.value
  if (!app) return true
  return app.org_id == null && transferTarget.value === ''
})

const transferPersonalOptionLabel = computed(() => {
  if (transferCreatorLoading.value) return t('account.applications.transferLoadingCreator')
  if (transferCreatorKind.value === 'self') return t('account.applications.transferTargetSelf')
  if (transferCreatorKind.value === 'other') {
    return t('account.applications.transferTargetOther', { creator: transferCreatorLabel.value })
  }
  const creatorId = transferringApp.value?.creator_user_id
  return creatorId != null
    ? t('account.applications.transferTargetUnknownWithId', { id: creatorId })
    : t('account.applications.transferTargetUnknown')
})

const transferCreatorWarningText = computed(() => {
  const app = transferringApp.value
  if (!app || app.org_id == null || transferCreatorLoading.value) return ''
  if (transferCreatorKind.value === 'self') return ''
  if (transferCreatorKind.value === 'other') {
    return t('account.applications.transferCreatorWarning', { creator: transferCreatorLabel.value })
  }
  return app.creator_user_id != null
    ? t('account.applications.transferCreatorUnknownWarningWithId', { id: app.creator_user_id })
    : t('account.applications.transferCreatorUnknownWarning')
})

async function resolveTransferCreator(app: any, seq: number) {
  transferCreatorLoading.value = true
  transferCreatorKind.value = 'unknown'
  transferCreatorLabel.value = ''
  try {
    const orgSlug = ownerSlugOf(app)
    const creatorId = app.creator_user_id
    if (!orgSlug || creatorId == null) return
    const [username, membersResp] = await Promise.all([
      loadMyUsername(),
      http.get(`/api/me/organizations/${orgSlug}/members`).catch(() => null),
    ])
    if (seq !== transferInspectSeq || transferringApp.value !== app) return
    const membersList: any[] = membersResp?.data?.members || []
    const creator = membersList.find((m) => Number(m.user_id) === Number(creatorId))
    if (creator && creator.username) {
      if (username && creator.username === username) {
        transferCreatorKind.value = 'self'
        transferCreatorLabel.value = creator.username
      } else if (username) {
        transferCreatorKind.value = 'other'
        transferCreatorLabel.value = `${creator.username} (#${creatorId})`
      } else {
        // Caller username unavailable: stay on the conservative unknown fallback
        // rather than claiming either identity.
        transferCreatorKind.value = 'unknown'
      }
    }
  } finally {
    if (seq === transferInspectSeq) transferCreatorLoading.value = false
  }
}

async function openTransfer(app: any) {
  error.value = null
  const seq = ++transferInspectSeq
  transferringApp.value = app
  transferTarget.value = ''
  transferCreatorLoading.value = app.org_id != null
  transferCreatorKind.value = 'unknown'
  transferCreatorLabel.value = ''
  transferOpen.value = true
  if (!await loadManageableOrgs()) {
    if (seq !== transferInspectSeq) return
    transferOpen.value = false
    transferCreatorLoading.value = false
    error.value = orgsError.value ?? t('account.applications.loadFailed')
    return
  }
  if (seq !== transferInspectSeq) return
  transferTarget.value = ''
  if (app.org_id != null) {
    await resolveTransferCreator(app, seq)
  }
}

async function confirmTransfer() {
  const app = transferringApp.value
  if (!app || transferring.value || transferUnchanged.value || transferCreatorLoading.value) return
  transferring.value = true
  error.value = null
  try {
    const targetSlug = app.org_id == null ? transferTarget.value : null
    await http.post(`/api/me/applications/${app.client_id}/transfer`, JSON.stringify({
      // The key is required; null means "back to personal".
      org_slug: targetSlug || null,
    }), { headers: { 'Content-Type': 'application/json' } })
    const appName = app.name || app.client_id
    if (targetSlug) {
      const owner = manageableOrgs.value.find((o) => o.slug === targetSlug)?.name || targetSlug
      success.value = t('account.applications.transferred', { app: appName, owner })
    } else if (transferCreatorKind.value === 'self') {
      success.value = t('account.applications.transferred', {
        app: appName,
        owner: t('account.applications.ownerPersonal'),
      })
    } else if (transferCreatorKind.value === 'other') {
      success.value = t('account.applications.transferredToCreator', {
        app: appName,
        creator: transferCreatorLabel.value,
      })
    } else {
      success.value = app.creator_user_id != null
        ? t('account.applications.transferredToUnknownCreatorWithId', {
            app: appName,
            id: app.creator_user_id,
          })
        : t('account.applications.transferredToUnknownCreator', { app: appName })
    }
    transferOpen.value = false
    setTimeout(() => { success.value = '' }, 3000)
    await fetchApps()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  } finally {
    transferring.value = false
  }
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
  if (gt === 'client_credentials' && editingApp.value?.client_type === 'PUBLIC') return
  const index = editGrants.value.indexOf(gt)
  if (index >= 0) {
    if (editGrants.value.length === 1) return
    editGrants.value.splice(index, 1)
  } else {
    editGrants.value.push(gt)
  }
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
    // Confirm under the name the application has NOW — a rename should not
    // be reported with the previous one.
    const currentName = typeof body.name === 'string' && body.name
      ? body.name
      : (original.name || original.client_id)
    success.value = t('account.applications.updated', { app: currentName })
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

onMounted(() => {
  void fetchApps()
  // For the owner names on the cards; a failure stays quiet here.
  void loadManageableOrgs()
})
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
        <!-- T2.2 (UF-04c): the owner. Personal by default; the organizations
             listed are the ones the caller manages. -->
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.owner') }}</label>
          <select
            v-model="newOrgSlug"
            data-testid="app-create-owner"
            class="mt-1 rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
            <option value="">{{ $t('account.applications.ownerPersonalOption') }}</option>
            <option
              v-for="org in manageableOrgs"
              :key="org.slug"
              :value="org.slug"
            >{{ org.name }}</option>
          </select>
          <p class="mt-1 text-xs text-neutral-400">
            {{ $t('account.applications.ownerHint') }}
          </p>
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
        <!-- T2.2 (UF-04c): grant types. client_credentials needs a secret a
             PUBLIC client cannot hold, so it is not offered for that type. -->
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
                :checked="newGrants.includes(gt)"
                :disabled="(gt === 'client_credentials' && newType === 'PUBLIC')
                           || (newGrants.length === 1 && newGrants.includes(gt))"
                :data-testid="`app-create-grant-${gt}`"
                class="rounded border-neutral-300"
                @change="toggleNewGrant(gt)"
              >
              {{ gt }}
            </label>
          </div>
          <p class="mt-1 text-xs text-neutral-400">
            {{ $t('account.applications.grantsHint') }}
          </p>
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
            <!-- T2.2 (UF-04b): who manages the application. The list carries
                 only the organization id; its name comes from the caller's
                 organizations. -->
            <div
              class="text-xs text-neutral-500 mt-1"
              data-testid="app-owner"
            >
              {{ $t('account.applications.managedBy') }}: {{ ownerNameOf(app) }}
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
            <!-- T2.2 (UF-04b): transfer — frozen for suspended apps like
                 every other management write. -->
            <button
              class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring disabled:opacity-50"
              data-testid="app-transfer-button"
              :disabled="app.status === 'suspended'"
              :title="app.status === 'suspended' ? $t('account.applications.suspendedFrozen') : undefined"
              @click="openTransfer(app)"
            >
              {{ $t('account.applications.transfer') }}
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

    <!-- T2.2 (UF-04b): transfer dialog. The submit stays disabled while the
         selection is the current owner, so confirming without choosing
         another owner cannot move anything. -->
    <AppModal
      :open="transferOpen"
      :title="$t('account.applications.transferTitle', { app: transferringApp?.name || transferringApp?.client_id })"
      size="sm"
      @close="transferOpen = false"
    >
      <div
        class="space-y-4"
        data-testid="app-transfer-dialog"
      >
        <p class="text-sm text-neutral-600">
          {{ $t('account.applications.transferBody') }}
        </p>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.applications.transferTarget') }}</label>
          <select
            v-model="transferTarget"
            data-testid="app-transfer-target"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-surface
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
            <template v-if="transferringApp?.org_id == null">
              <option
                disabled
                value=""
              >{{ $t('account.applications.transferSelectPlaceholder') }}</option>
              <option
                v-for="org in transferTargets"
                :key="org.slug"
                :value="org.slug"
              >{{ org.name }}</option>
            </template>
            <option
              v-else
              value=""
            >{{ transferPersonalOptionLabel }}</option>
          </select>
          <p
            v-if="transferringApp?.org_id == null && transferTargets.length === 0"
            class="mt-1 text-xs text-neutral-500"
            data-testid="app-transfer-no-targets"
          >
            {{ $t('account.applications.transferNoTargets') }}
          </p>
        </div>
        <AppAlert
          v-if="transferCreatorWarningText"
          type="warning"
          data-testid="app-transfer-creator-warning"
        >
          {{ transferCreatorWarningText }}
        </AppAlert>
        <div class="flex justify-end gap-2 pt-2">
          <button
            type="button"
            class="px-4 py-2 text-sm text-neutral-600 border border-neutral-300 rounded-ctl hover:bg-neutral-50"
            @click="transferOpen = false"
          >
            {{ $t('common.cancel') }}
          </button>
          <button
            type="button"
            :disabled="transferring || transferUnchanged || transferCreatorLoading"
            data-testid="app-transfer-submit"
            class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                   disabled:opacity-50 transition-colors
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            @click="confirmTransfer"
          >
            {{ transferring ? $t('account.applications.transferring') : $t('account.applications.transfer') }}
          </button>
        </div>
      </div>
    </AppModal>

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
                :disabled="(gt === 'client_credentials' && editingApp?.client_type === 'PUBLIC')
                           || (editGrants.length === 1 && editGrants.includes(gt))"
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
