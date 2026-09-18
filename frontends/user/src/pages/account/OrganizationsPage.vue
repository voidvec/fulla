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

async function fetchOrgs() {
  loading.value = true
  try {
    const resp = await http.get('/api/me/organizations')
    orgs.value = resp.data?.organizations || []
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

async function removeMember(slug: string, userId: string | number) {
  if (!confirm(t('account.organizations.removeConfirm'))) return
  try {
    await http.delete(`/api/me/organizations/${slug}/members/${userId}`)
    await toggleMembers(slug)
    await toggleMembers(slug)
  } catch (e: unknown) {
    error.value = normalizeError(e)
  }
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
          <button
            class="px-3 py-1.5 text-sm text-brand-600 border border-brand-200 rounded-ctl hover:bg-brand-50 transition-colors
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            @click="toggleMembers(org.slug)"
          >
            {{ $t('account.organizations.members') }}
          </button>
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
              class="px-3 py-2 text-sm text-white bg-brand-600 rounded-ctl hover:bg-brand-700"
              @click="invite(org.slug)"
            >
              {{ $t('account.organizations.invite') }}
            </button>
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
      </AppCard>
    </div>
  </div>
</template>
