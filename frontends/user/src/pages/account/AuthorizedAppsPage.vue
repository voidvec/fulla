<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import http from '../../services/http'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'
import AppAlert from '../../components/ui/AppAlert.vue'
import AppCard from '../../components/ui/AppCard.vue'
import AppEmptyState from '../../components/ui/AppEmptyState.vue'
import DData from '../../components/ui/DData.vue'
import AppModal from '../../components/ui/AppModal.vue'
import AppSelect from '../../components/ui/AppSelect.vue'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'

const { t } = useI18n()
const apps = ref<any[]>([])
const loading = ref(true)
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen;
// plain strings (chrome copy via t()) keep snapshot semantics.
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

async function fetchApps() {
  loading.value = true
  try {
    const resp = await http.get('/api/me/authorized-apps')
    // Backend envelope: {authorized_apps: [...], total} (UserSelfServiceController).
    // The old `resp.data.apps ||` first fallback matched only a mock shape the
    // real backend never returns — PR-review cleanup removed it so the page
    // parses the actual contract.
    apps.value = resp.data?.authorized_apps || []
  } catch {
    error.value = t('account.authorizedApps.loadFailed')
  } finally {
    loading.value = false
  }
}

function revokeApp(clientId: string, appName: string) {
  askConfirm(t('account.authorizedApps.revokeConfirm', { app: appName }), async () => {
    try {
      await http.delete(`/api/me/authorized-apps/${clientId}`)
      success.value = t('account.authorizedApps.revoked', { app: appName })
      setTimeout(() => { success.value = '' }, 3000)
      await fetchApps()
    } catch (e: unknown) {
      error.value = normalizeError(e)
    }
  })
}

// Member-side "request organization authorization" (same contract as the
// applications page): files a consent-request for an authorized app against
// one of the user's organizations. Org list fetched lazily on dialog open —
// no pre-flight on page load; zero memberships shows an info message
// instead of an empty select.
const requestAuthOpen = ref(false)
const requestAuthApp = ref<any>(null)
const requestAuthOrgs = ref<any[]>([])
const requestAuthSlug = ref('')
const requestAuthLoading = ref(false)
const requestAuthNoOrgs = ref(false)
const requestAuthSubmitting = ref(false)

const requestAuthOptions = computed(() =>
  requestAuthOrgs.value.map((o: any) => ({ value: o.slug, label: `${o.name} (${o.slug})` })),
)

async function openRequestAuth(app: any) {
  requestAuthApp.value = app
  requestAuthOrgs.value = []
  requestAuthSlug.value = ''
  requestAuthNoOrgs.value = false
  requestAuthOpen.value = true
  requestAuthLoading.value = true
  try {
    const resp = await http.get('/api/me/organizations')
    requestAuthOrgs.value = resp.data?.organizations || []
    requestAuthNoOrgs.value = requestAuthOrgs.value.length === 0
  } catch (e: unknown) {
    requestAuthOpen.value = false
    error.value = normalizeError(e)
  } finally {
    requestAuthLoading.value = false
  }
}

async function submitRequestAuth() {
  if (!requestAuthApp.value || !requestAuthSlug.value) return
  requestAuthSubmitting.value = true
  error.value = null
  try {
    const resp = await http.post(
      `/api/me/organizations/${requestAuthSlug.value}/consent-requests`,
      { client_id: requestAuthApp.value.client_id },
    )
    // The backend's message distinguishes first filing from the idempotent
    // already-pending replay — surface it verbatim.
    success.value = resp.data?.message || t('account.authorizedApps.requestOrgAuthSuccess')
    setTimeout(() => { success.value = '' }, 3000)
    requestAuthOpen.value = false
  } catch (e: unknown) {
    error.value = normalizeError(e)
    requestAuthOpen.value = false
  } finally {
    requestAuthSubmitting.value = false
  }
}

onMounted(fetchApps)
</script>

<template>
  <div>
    <h1 class="text-2xl font-bold text-neutral-900 mb-6">
      {{ $t('account.authorizedApps.title') }}
    </h1>
    <p class="text-neutral-500 mb-6">
      {{ $t('account.authorizedApps.intro') }}
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
        :title="$t('account.authorizedApps.emptyTitle')"
        :description="$t('account.authorizedApps.emptyDesc')"
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
            <span class="text-sm text-neutral-500">{{ $t('account.authorizedApps.clientId') }}</span>
            <DData :value="app.client_id" />
          </div>
          <div
            v-if="app.scope"
            class="flex flex-wrap items-center gap-1.5 mt-1.5"
          >
            <span class="text-xs text-neutral-400">{{ $t('account.authorizedApps.scopes') }}</span>
            <DData
              v-for="s in app.scope.split(' ').filter(Boolean)"
              :key="s"
              :value="s"
            />
          </div>
        </div>
        <div class="flex items-center gap-2">
          <button
            class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            data-testid="request-org-auth"
            @click="openRequestAuth(app)"
          >
            {{ $t('account.authorizedApps.requestOrgAuth') }}
          </button>
          <button
            class="px-3 py-1.5 text-sm text-error-600 border border-error-200 rounded-ctl hover:bg-error-50 transition-colors
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            @click="revokeApp(app.client_id, app.name || app.client_id)"
          >
            {{ $t('account.authorizedApps.revoke') }}
          </button>
        </div>
        </div>
      </AppCard>
    </div>

    <AppModal
      :open="requestAuthOpen"
      :title="$t('account.authorizedApps.requestOrgAuthTitle')"
      size="sm"
      @close="requestAuthOpen = false"
    >
      <p
        v-if="requestAuthLoading"
        class="text-sm text-neutral-500"
      >
        {{ $t('common.loading') }}
      </p>
      <p
        v-else-if="requestAuthNoOrgs"
        class="text-sm text-neutral-600"
        data-testid="request-org-auth-no-orgs"
      >
        {{ $t('account.authorizedApps.requestOrgAuthNoOrgs') }}
      </p>
      <form
        v-else
        class="space-y-4"
        @submit.prevent="submitRequestAuth"
      >
        <AppSelect
          v-model="requestAuthSlug"
          :label="$t('account.authorizedApps.requestOrgAuthPick')"
          :options="requestAuthOptions"
          :placeholder="$t('account.authorizedApps.requestOrgAuthPick')"
          required
        />
        <button
          type="submit"
          :disabled="requestAuthSubmitting || !requestAuthSlug"
          class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
                 disabled:opacity-50 transition-colors
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          data-testid="request-org-auth-submit"
        >
          {{ $t('account.authorizedApps.requestOrgAuthSubmit') }}
        </button>
      </form>
    </AppModal>

    <AppConfirmDialog
      :open="confirmOpen"
      :message="confirmMessage"
      danger
      @confirm="runConfirm"
      @cancel="confirmOpen = false"
    />
  </div>
</template>

