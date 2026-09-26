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
        <button
          class="px-3 py-1.5 text-sm text-error-600 border border-error-200 rounded-ctl hover:bg-error-50 transition-colors
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          @click="revokeApp(app.client_id, app.name || app.client_id)"
        >
          {{ $t('account.authorizedApps.revoke') }}
        </button>
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

