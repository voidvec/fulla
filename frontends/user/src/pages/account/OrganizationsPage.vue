<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import http from '../../services/http'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppAlert from '../../components/ui/AppAlert.vue'
import AppBadge from '../../components/ui/AppBadge.vue'
import AppCard from '../../components/ui/AppCard.vue'
import AppEmptyState from '../../components/ui/AppEmptyState.vue'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'

const { t } = useI18n()
const orgs = ref<any[]>([])
const loading = ref(true)
const error = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = error.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})
const success = ref('')

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

const showCreate = ref(false)
const creating = ref(false)
const newSlug = ref('')
const newName = ref('')

const expandedSlug = ref('')
const members = ref<any[]>([])
const inviteEmail = ref('')
const inviteRole = ref<'member' | 'admin'>('member')
const lastInviteToken = ref('')

const acceptToken = ref('')
const accepting = ref(false)

// v1.5.0 M3: ownership succession (R-M3-5) -- the nominee-facing accept
// banner rides the org list response (the nominee may be a non-member).
const pendingNominations = ref<any[]>([])
const nominateUserId = ref('')

async function acceptSuccession(slug: string) {
  error.value = null
  try {
    await http.post(`/api/me/organizations/${slug}/successor-nomination/accept`, {})
    success.value = t('account.organizations.successionAccepted')
    setTimeout(() => { success.value = '' }, 3000)
    await fetchOrgs()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  }
}

async function nominateSuccessor(slug: string) {
  const parsed = Number.parseInt(nominateUserId.value, 10)
  if (Number.isNaN(parsed)) return
  error.value = null
  try {
    await http.post(`/api/me/organizations/${slug}/successor-nomination`, {
      user_id: parsed,
    })
    success.value = t('account.organizations.successionPending', { id: nominateUserId.value })
    setTimeout(() => { success.value = '' }, 3000)
    nominateUserId.value = ''
    await fetchOrgs()
    await toggleMembers(slug)
    await toggleMembers(slug)
  } catch (e: unknown) {
    error.value = normalizeError(e)
  }
}

async function withdrawSuccession(slug: string) {
  error.value = null
  try {
    await http.delete(`/api/me/organizations/${slug}/successor-nomination`)
    await fetchOrgs()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  }
}

// v1.5.0 M2: organization consents panel (owner/admin).
const expandedConsentsSlug = ref('')
const consents = ref<any[]>([])
const grantClientId = ref('')
const grantRedirectUri = ref('')
const grantLink = ref('')
let consentsRequestSeq = 0

// Org consent-requests approval queue (manager side): members file requests
// from their app lists; owners/admins approve/reject them here. Fetched
// alongside the consents list when the panel opens; the section hides
// entirely while the queue is empty.
const pendingRequests = ref<any[]>([])
let pendingRequestsSeq = 0

function isManager(role: string | undefined): boolean {
  return role === 'owner' || role === 'admin'
}

// Consent-request timestamps arrive as Postgres "YYYY-MM-DD HH:MM:SS";
// an approval queue only needs it short — date + minutes, verbatim (no
// locale-dependent Date parsing surprises in the e2e suite).
function shortDateTime(value: unknown): string {
  if (typeof value !== 'string' || !value) return ''
  const m = /^(\d{4}-\d{2}-\d{2})[T ](\d{2}:\d{2})/.exec(value)
  return m ? `${m[1]} ${m[2]}` : value
}

async function fetchPendingRequests(slug: string) {
  const seq = ++pendingRequestsSeq
  try {
    const resp = await http.get(`/api/me/organizations/${slug}/consent-requests`)
    if (seq !== pendingRequestsSeq || expandedConsentsSlug.value !== slug) return
    pendingRequests.value = resp.data?.requests || []
  } catch (e: unknown) {
    if (seq === pendingRequestsSeq && expandedConsentsSlug.value === slug) error.value = normalizeError(e)
  }
}

async function toggleConsents(slug: string) {
  if (expandedConsentsSlug.value === slug) {
    expandedConsentsSlug.value = ''
    return
  }
  const seq = ++consentsRequestSeq
  expandedConsentsSlug.value = slug
  consents.value = []
  pendingRequests.value = []
  grantClientId.value = ''
  grantRedirectUri.value = ''
  grantLink.value = ''
  fetchPendingRequests(slug)
  try {
    const resp = await http.get(`/api/me/organizations/${slug}/consents`)
    if (seq !== consentsRequestSeq || expandedConsentsSlug.value !== slug) return
    consents.value = resp.data?.consents || []
  } catch (e: unknown) {
    if (seq === consentsRequestSeq) error.value = normalizeError(e)
  }
}

// An approval resolves the request AND creates consent rows — refresh both
// lists (and drop the resolved request from the pending queue via refetch).
async function refreshConsentPanel(slug: string) {
  const [consentsResp, requestsResp] = await Promise.all([
    http.get(`/api/me/organizations/${slug}/consents`),
    http.get(`/api/me/organizations/${slug}/consent-requests`),
  ])
  consents.value = consentsResp.data?.consents || []
  pendingRequests.value = requestsResp.data?.requests || []
}

function approveConsentRequest(slug: string, req: any) {
  askConfirm(
    t('account.organizations.pendingApproveConfirm', {
      requester: req.requester_username || req.requested_by,
      client: req.client_id,
    }),
    async () => {
      error.value = null
      try {
        await http.post(`/api/me/organizations/${slug}/consent-requests/${req.id}/approve`, {})
        success.value = t('account.organizations.approved')
        setTimeout(() => { success.value = '' }, 3000)
        await refreshConsentPanel(slug)
      } catch (e: unknown) {
        error.value = normalizeError(e)
      }
    },
  )
}

function rejectConsentRequest(slug: string, req: any) {
  askConfirm(
    t('account.organizations.pendingRejectConfirm', {
      requester: req.requester_username || req.requested_by,
      client: req.client_id,
    }),
    async () => {
      error.value = null
      try {
        await http.post(`/api/me/organizations/${slug}/consent-requests/${req.id}/reject`, {})
        success.value = t('account.organizations.rejected')
        setTimeout(() => { success.value = '' }, 3000)
        await refreshConsentPanel(slug)
      } catch (e: unknown) {
        error.value = normalizeError(e)
      }
    },
  )
}

function revokeConsents(slug: string, clientId: string) {
  askConfirm(t('account.organizations.consentsRevokeConfirm'), async () => {
    error.value = null
    try {
      await http.delete(`/api/me/organizations/${slug}/consents/${clientId}`)
      success.value = t('account.organizations.consentsRevoked')
      setTimeout(() => { success.value = '' }, 3000)
      const resp = await http.get(`/api/me/organizations/${slug}/consents`)
      consents.value = resp.data?.consents || []
    } catch (e: unknown) {
      error.value = normalizeError(e)
    }
  })
}

// R-M2-3: the portal's entire "start an org authorization" surface is a
// generated authorize link carrying the org hint -- the org manager opens
// it in their own browser session; the consent screen then records the
// grant on the organization's behalf (R-M2-2 server-side).
// The PKCE challenge is a ceremony-only value: every shipped config
// requires a code_challenge at the consent POST, but the code produced by
// this grant flow is never exchanged (R-M2-3) -- it expires unused. A
// random S256-shaped challenge (43+ chars of the PKCE alphabet) satisfies
// the gate; no verifier is kept because nothing redeems the code.
function randomPkceChallenge(): string {
  const bytes = new Uint8Array(32)
  crypto.getRandomValues(bytes)
  let binary = ''
  bytes.forEach((b) => { binary += String.fromCharCode(b) })
  return btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
}

function generateGrantLink(slug: string) {
  if (!grantClientId.value || !grantRedirectUri.value) return
  const params = new URLSearchParams({
    response_type: 'code',
    client_id: grantClientId.value,
    redirect_uri: grantRedirectUri.value,
    scope: 'openid profile org',
    state: `orggrant-${Date.now()}`,
    prompt: 'consent',
    org_id: slug,
    code_challenge: randomPkceChallenge(),
    code_challenge_method: 'S256',
  })
  grantLink.value = `${window.location.origin}/oauth2/authorize?${params.toString()}`
}

async function fetchOrgs() {
  loading.value = true
  try {
    const resp = await http.get('/api/me/organizations')
    orgs.value = resp.data?.organizations || []
    pendingNominations.value = resp.data?.pending_succession_nominations || []
  } catch {
    error.value = t('account.organizations.loadFailed')
  } finally {
    loading.value = false
  }
}

async function createOrg() {
  creating.value = true
  error.value = null
  try {
    await http.post('/api/me/organizations', {
      slug: newSlug.value,
      name: newName.value,
    })
    success.value = t('account.organizations.created')
    setTimeout(() => { success.value = '' }, 3000)
    showCreate.value = false
    newSlug.value = ''
    newName.value = ''
    await fetchOrgs()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  } finally {
    creating.value = false
  }
}

// M11 (review): one members panel is shared across orgs — a late response
// for org A must never render into org B's panel. Sequence-guard every
// expansion and reset the per-panel state on switch.
let membersRequestSeq = 0

async function toggleMembers(slug: string) {
  if (expandedSlug.value === slug) {
    expandedSlug.value = ''
    return
  }
  const seq = ++membersRequestSeq
  expandedSlug.value = slug
  members.value = []
  inviteEmail.value = ''
  lastInviteToken.value = ''
  try {
    const resp = await http.get(`/api/me/organizations/${slug}/members`)
    if (seq !== membersRequestSeq || expandedSlug.value !== slug) return
    members.value = resp.data?.members || []
  } catch (e: unknown) {
    if (seq === membersRequestSeq) error.value = normalizeError(e)
  }
}

async function invite(slug: string) {
  if (!inviteEmail.value) return
  error.value = null
  try {
    const resp = await http.post(`/api/me/organizations/${slug}/invitations`, {
      email: inviteEmail.value,
      role: inviteRole.value,
    })
    lastInviteToken.value = resp.data?.token || ''
    inviteEmail.value = ''
    success.value = t('account.organizations.invited')
    setTimeout(() => { success.value = '' }, 3000)
  } catch (e: unknown) {
    error.value = normalizeError(e)
  }
}

function removeMember(slug: string, userId: string | number) {
  askConfirm(t('account.organizations.removeConfirm'), async () => {
    try {
      await http.delete(`/api/me/organizations/${slug}/members/${userId}`)
      await toggleMembers(slug)
      await toggleMembers(slug)
    } catch (e: unknown) {
      error.value = normalizeError(e)
    }
  })
}

async function acceptInvite() {
  if (!acceptToken.value) return
  accepting.value = true
  error.value = null
  try {
    await http.post('/api/me/org-invitations/accept', { token: acceptToken.value })
    acceptToken.value = ''
    success.value = t('account.organizations.accepted')
    setTimeout(() => { success.value = '' }, 3000)
    await fetchOrgs()
  } catch (e: unknown) {
    error.value = normalizeError(e)
  } finally {
    accepting.value = false
  }
}

onMounted(fetchOrgs)
</script>

<template>
  <div>
    <div class="flex items-center justify-between mb-6">
      <h1 class="text-2xl font-bold text-neutral-900">
        {{ $t('account.organizations.title') }}
      </h1>
      <button
        class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700 transition-colors
               focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        @click="showCreate = !showCreate"
      >
        {{ $t('account.organizations.create') }}
      </button>
    </div>
    <p class="text-neutral-500 mb-6">
      {{ $t('account.organizations.intro') }}
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
      v-if="pendingNominations.length > 0"
      class="mb-6"
      data-testid="succession-banner"
    >
      <p class="font-medium text-neutral-900 mb-2">
        {{ $t('account.organizations.successionBannerTitle') }}
      </p>
      <div
        v-for="n in pendingNominations"
        :key="n.slug"
        class="flex items-center justify-between gap-2 py-1"
      >
        <span class="text-neutral-700">
          {{ $t('account.organizations.successionBannerDesc', { name: n.name }) }}
        </span>
        <button
          class="px-4 py-2 text-sm text-white bg-brand-600 rounded-ctl hover:bg-brand-700 transition-colors"
          @click="acceptSuccession(n.slug)"
        >
          {{ $t('account.organizations.successionAccept') }}
        </button>
      </div>
    </AppCard>

    <AppCard class="mb-6">
      <p class="font-medium text-neutral-900 mb-2">
        {{ $t('account.organizations.acceptTitle') }}
      </p>
      <div class="flex items-center gap-2">
        <input
          v-model="acceptToken"
          :placeholder="$t('account.organizations.acceptPlaceholder')"
          class="flex-1 rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        >
        <button
          :disabled="accepting || !acceptToken"
          class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                 disabled:opacity-50 transition-colors
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          @click="acceptInvite"
        >
          {{ $t('account.organizations.accept') }}
        </button>
      </div>
    </AppCard>

    <AppCard
      v-if="showCreate"
      class="mb-6"
    >
      <form
        class="space-y-4"
        @submit.prevent="createOrg"
      >
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.organizations.slug') }}</label>
          <input
            v-model="newSlug"
            required
            pattern="[a-z0-9][a-z0-9-]{1,48}[a-z0-9]"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <div>
          <label class="block text-sm font-medium text-neutral-500">{{ $t('account.organizations.name') }}</label>
          <input
            v-model="newName"
            required
            maxlength="100"
            class="mt-1 w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
        </div>
        <button
          type="submit"
          :disabled="creating"
          class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                 disabled:opacity-50 transition-colors
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        >
          {{ $t('account.organizations.submit') }}
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
      v-else-if="orgs.length === 0"
      padding="none"
    >
      <AppEmptyState
        :title="$t('account.organizations.emptyTitle')"
        :description="$t('account.organizations.emptyDesc')"
      />
    </AppCard>

    <div
      v-else
      class="space-y-3"
    >
      <AppCard
        v-for="org in orgs"
        :key="org.id"
        padding="sm"
      >
        <div class="flex items-center justify-between">
          <div>
            <p class="font-medium text-neutral-900">
              {{ org.name }}
              <span class="text-sm text-neutral-400 ml-1">@{{ org.slug }}</span>
            </p>
            <AppBadge
              :variant="org.role === 'owner' ? 'warning' : org.role === 'admin' ? 'info' : 'neutral'"
              size="sm"
              class="mt-1"
            >{{ org.role }}</AppBadge>
          </div>
          <div class="flex items-center gap-2">
            <button
              class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
              @click="toggleMembers(org.slug)"
            >
              {{ $t('account.organizations.members') }}
            </button>
            <button
              v-if="isManager(org.role)"
              class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
              @click="toggleConsents(org.slug)"
            >
              {{ $t('account.organizations.consents') }}
            </button>
          </div>
        </div>
        <div
          v-if="expandedSlug === org.slug"
          class="mt-4 border-t border-neutral-100 pt-4 space-y-2"
        >
          <div
            v-for="m in members"
            :key="m.user_id"
            class="flex items-center justify-between"
          >
            <span class="text-neutral-900">
              {{ m.display_name || m.username || m.user_id }}
              <AppBadge
                size="sm"
                class="ml-1"
              >{{ m.role }}</AppBadge>
            </span>
            <button
              class="text-sm text-error-600 hover:text-error-800"
              @click="removeMember(org.slug, m.user_id)"
            >
              {{ $t('account.organizations.remove') }}
            </button>
          </div>
          <div
            v-if="org.role === 'owner' || org.role === 'admin'"
            class="flex items-center gap-2 pt-2 border-t border-neutral-100"
          >
            <input
              v-model="inviteEmail"
              :placeholder="$t('account.organizations.inviteEmail')"
              class="flex-1 rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            >
            <select
              v-model="inviteRole"
              class="rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white"
            >
              <option value="member">member</option>
              <option value="admin">admin</option>
            </select>
            <button
              class="px-4 py-2 text-sm text-white bg-brand-600 rounded-ctl hover:bg-brand-700"
              @click="invite(org.slug)"
            >
              {{ $t('account.organizations.invite') }}
            </button>
          </div>
          <div
            v-if="org.role === 'owner'"
            class="pt-2 border-t border-neutral-100 space-y-2"
            data-testid="succession-nominate"
          >
            <p
              v-if="org.successor_nomination"
              class="text-sm text-neutral-500"
            >
              {{ $t('account.organizations.successionPending', { id: org.successor_nomination.nominee_user_id }) }}
              <button
                class="ml-2 underline text-neutral-400 hover:text-neutral-600"
                @click="withdrawSuccession(org.slug)"
              >
                {{ $t('account.organizations.successionWithdraw') }}
              </button>
            </p>
            <div class="flex items-center gap-2">
              <input
                v-model="nominateUserId"
                :placeholder="$t('account.organizations.successionNominate')"
                class="flex-1 rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                       focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
              >
              <button
                class="px-4 py-2 text-sm text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                       disabled:opacity-50"
                :disabled="!nominateUserId"
                @click="nominateSuccessor(org.slug)"
              >
                {{ $t('account.organizations.successionNominateAction') }}
              </button>
            </div>
          </div>
          <p
            v-if="lastInviteToken"
            class="text-xs text-neutral-500 break-all"
          >
            {{ $t('account.organizations.inviteToken') }}: {{ lastInviteToken }}
            <button
              class="ml-2 underline text-neutral-400 hover:text-neutral-600"
              @click="lastInviteToken = ''"
            >
              {{ $t('common.dismiss') }}
            </button>
          </p>
        </div>
        <div
          v-if="expandedConsentsSlug === org.slug"
          class="mt-4 border-t border-neutral-100 pt-4 space-y-3"
          data-testid="org-consents-panel"
        >
          <div
            v-if="pendingRequests.length > 0"
            class="space-y-2 pb-3 border-b border-neutral-100"
            data-testid="org-pending-requests"
          >
            <p class="font-medium text-neutral-900">
              {{ $t('account.organizations.pendingRequests') }}
            </p>
            <div
              v-for="r in pendingRequests"
              :key="r.id"
              class="flex items-center justify-between gap-2"
            >
              <span class="text-sm text-neutral-900 break-all">
                {{ r.requester_username || r.requested_by }} · {{ r.client_id }} ·
                {{ shortDateTime(r.requested_at) }}
              </span>
              <span class="flex items-center gap-2 whitespace-nowrap">
                <button
                  class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                         focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
                  data-testid="approve-consent-request"
                  @click="approveConsentRequest(org.slug, r)"
                >
                  {{ $t('account.organizations.approve') }}
                </button>
                <button
                  class="px-3 py-1.5 text-sm text-error-600 border border-error-200 rounded-ctl hover:bg-error-50 transition-colors
                         focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
                  data-testid="reject-consent-request"
                  @click="rejectConsentRequest(org.slug, r)"
                >
                  {{ $t('account.organizations.reject') }}
                </button>
              </span>
            </div>
          </div>
          <div
            v-for="group in consents"
            :key="group.client_id"
            class="flex items-center justify-between gap-2"
          >
            <span class="text-neutral-900 break-all">
              {{ group.client_id }}
              <span class="text-xs text-neutral-500 ml-1">
                {{ group.scopes?.map((s: any) => s.scope).join(', ') }}
              </span>
            </span>
            <button
              class="text-sm text-error-600 hover:text-error-800 whitespace-nowrap"
              data-testid="revoke-org-consent"
              @click="revokeConsents(org.slug, group.client_id)"
            >
              {{ $t('account.organizations.consentsRevoke') }}
            </button>
          </div>
          <p
            v-if="consents.length === 0"
            class="text-sm text-neutral-500"
          >
            {{ $t('account.organizations.consentsEmpty') }}
          </p>
          <div class="pt-2 border-t border-neutral-100 space-y-2">
            <p class="font-medium text-neutral-900">
              {{ $t('account.organizations.consentsStart') }}
            </p>
            <input
              v-model="grantClientId"
              :placeholder="$t('account.organizations.consentsClientId')"
              class="w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            >
            <input
              v-model="grantRedirectUri"
              :placeholder="$t('account.organizations.consentsRedirectUri')"
              class="w-full rounded-ctl border border-neutral-300 px-3 py-2 text-neutral-900 bg-white
                     focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            >
            <button
              class="px-4 py-2 text-sm text-white bg-brand-600 rounded-ctl hover:bg-brand-700 transition-colors
                     disabled:opacity-50"
              :disabled="!grantClientId || !grantRedirectUri"
              @click="generateGrantLink(org.slug)"
            >
              {{ $t('account.organizations.consentsGenerate') }}
            </button>
            <p
              v-if="grantLink"
              class="text-xs text-neutral-500 break-all"
              data-testid="org-grant-link"
            >
              {{ $t('account.organizations.consentsLinkTitle') }}:
              {{ grantLink }}
            </p>
          </div>
        </div>
      </AppCard>
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
