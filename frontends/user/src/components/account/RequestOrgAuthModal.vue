<script setup lang="ts">
import { ref, computed, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import http from '../../services/http'
import AppModal from '../ui/AppModal.vue'
import AppSelect from '../ui/AppSelect.vue'

// #249: single owner of the request-org-auth modal (previously duplicated
// verbatim between ApplicationsPage and AuthorizedAppsPage). The `scope`
// prop keeps each page's existing translations; success/error surface as
// events so the pages keep their own alert banners.
const props = defineProps<{
  open: boolean
  app: any
  scope: string
}>()

const emit = defineEmits<{
  (e: 'close'): void
  (e: 'success', message: string): void
  (e: 'error', err: unknown): void
}>()

const { t } = useI18n()
const orgs = ref<any[]>([])
const slug = ref('')
const loading = ref(false)
const noOrgs = ref(false)
const submitting = ref(false)

const options = computed(() =>
  orgs.value.map((o: any) => ({ value: o.slug, label: `${o.name} (${o.slug})` })),
)

// Refetch on every open (matches the pre-extraction behavior: each open
// starts from an empty org list and a cleared selection).
watch(
  () => props.open,
  async (open) => {
    if (!open) return
    orgs.value = []
    slug.value = ''
    noOrgs.value = false
    loading.value = true
    try {
      const resp = await http.get('/api/me/organizations')
      orgs.value = resp.data?.organizations || []
      noOrgs.value = orgs.value.length === 0
    } catch (e: unknown) {
      emit('close')
      emit('error', e)
    } finally {
      loading.value = false
    }
  },
)

async function submit() {
  if (!props.app || !slug.value) return
  submitting.value = true
  try {
    const resp = await http.post(
      `/api/me/organizations/${slug.value}/consent-requests`,
      { client_id: props.app.client_id },
      // The user app's axios instance defaults to form-encoded bodies; the
      // endpoint reads a JSON object (getJsonObject), so send JSON explicitly.
      { headers: { 'Content-Type': 'application/json' } },
    )
    // The backend's message distinguishes first filing from the idempotent
    // already-pending replay — surface it verbatim.
    emit('success', resp.data?.message || t(`${props.scope}.requestOrgAuthSuccess`))
    emit('close')
  } catch (e: unknown) {
    emit('error', e)
    emit('close')
  } finally {
    submitting.value = false
  }
}
</script>

<template>
  <AppModal
    :open="open"
    :title="t(`${scope}.requestOrgAuthTitle`)"
    size="sm"
    @close="emit('close')"
  >
    <p
      v-if="loading"
      class="text-sm text-neutral-500"
    >
      {{ $t('common.loading') }}
    </p>
    <p
      v-else-if="noOrgs"
      class="text-sm text-neutral-600"
      data-testid="request-org-auth-no-orgs"
    >
      {{ t(`${scope}.requestOrgAuthNoOrgs`) }}
    </p>
    <form
      v-else
      class="space-y-4"
      @submit.prevent="submit"
    >
      <AppSelect
        v-model="slug"
        :label="t(`${scope}.requestOrgAuthPick`)"
        :options="options"
        :placeholder="t(`${scope}.requestOrgAuthPick`)"
        required
      />
      <button
        type="submit"
        :disabled="submitting || !slug"
        class="px-4 py-2 text-sm font-medium text-white bg-brand-600 rounded-ctl hover:bg-brand-700
               disabled:opacity-50 transition-colors
               focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        data-testid="request-org-auth-submit"
      >
        {{ t(`${scope}.requestOrgAuthSubmit`) }}
      </button>
    </form>
  </AppModal>
</template>
