<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '@/services/errorAdapter'
import { getErrorMessage } from '@/services/messages'
import DData from '@/components/ui/DData.vue'

const scopes = ref<any[]>([])
const loading = ref(true)
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText / oidcErrorText) so switching locale re-translates
// text already on screen.
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

// #110-B contract: GET /api/admin/oidc/keys reports the live signing
// keystore — every loaded key plus the rotation state — not a single flat
// key. The legacy single-key path surfaces as one entry (kid "key-1").
interface OidcKeyEntry {
  kid: string
  kty: string
  alg: string
  use: string
  status: string
}

interface OidcKeysResponse {
  status: string
  jwks_uri: string
  discovery_uri: string
  keys: OidcKeyEntry[]
  active_kid: string
  key_count: number
  note: string
}

const oidcKeys = ref<OidcKeysResponse | null>(null)
const oidcLoading = ref(true)
const oidcErrorMessage = ref<NormalizedError | string | null>(null)
const oidcErrorText = computed(() => {
  const e = oidcErrorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})
const copySuccess = ref('')

async function fetchScopes() {
  loading.value = true
  errorMessage.value = null
  try {
    const resp = await axios.get('/api/admin/scopes')
    scopes.value = resp.data.scopes || []
  } catch (e) {
    const normalized = normalizeError(e)
    errorMessage.value = normalized
    console.error('Failed to fetch scopes:', e)
  } finally {
    loading.value = false
  }
}

async function fetchOidcKeys() {
  oidcLoading.value = true
  oidcErrorMessage.value = null
  try {
    const resp = await axios.get('/api/admin/oidc/keys')
    oidcKeys.value = resp.data
  } catch (e) {
    const normalized = normalizeError(e)
    oidcErrorMessage.value = normalized
    console.error('Failed to fetch OIDC keys:', e)
  } finally {
    oidcLoading.value = false
  }
}

async function copyToClipboard(text: string, label: string) {
  try {
    await navigator.clipboard.writeText(text)
    copySuccess.value = label
    setTimeout(() => {
      copySuccess.value = ''
    }, 2000)
  } catch (e) {
    console.error('Failed to copy:', e)
  }
}

onMounted(() => {
  fetchScopes()
  fetchOidcKeys()
})
</script>

<template>
  <div>
    <h2 class="text-2xl font-bold text-neutral-900 mb-6">
      {{ $t('admin.settings.title') }}
    </h2>

    <!-- Error Banner for Scopes -->
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

    <!-- Scopes Section -->
    <div class="bg-surface shadow rounded-lg overflow-hidden">
      <div class="px-6 py-4 border-b">
        <h3 class="text-lg font-medium text-neutral-900">
          {{ $t('admin.settings.oauth2Scopes') }}
        </h3>
      </div>

      <div
        v-if="loading"
        class="p-6 text-center text-neutral-500"
      >
        {{ $t('common.loading') }}
      </div>

      <table
        v-else
        class="min-w-full divide-y divide-neutral-200"
      >
        <thead class="bg-neutral-50">
          <tr>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.name') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('common.description') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.settings.mappedRole') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.settings.defaultColumn') }}
            </th>
            <th class="px-6 py-3 text-left text-xs font-medium text-neutral-500 uppercase">
              {{ $t('admin.settings.adminOnlyColumn') }}
            </th>
          </tr>
        </thead>
        <tbody class="bg-surface divide-y divide-neutral-200">
          <tr
            v-for="scope in scopes"
            :key="scope.id"
            class="hover:bg-neutral-50"
          >
            <td class="px-6 py-4 text-sm font-medium text-neutral-900 font-mono">
              {{ scope.name }}
            </td>
            <td class="px-6 py-4 text-sm text-neutral-500">
              {{ scope.description || '—' }}
            </td>
            <td class="px-6 py-4 text-sm text-neutral-500">
              {{ scope.mapped_role || '—' }}
            </td>
            <td class="px-6 py-4">
              <span
                v-if="scope.is_default"
                class="text-success-600"
              >✓</span>
              <span
                v-else
                class="text-neutral-300"
              >—</span>
            </td>
            <td class="px-6 py-4">
              <span
                v-if="scope.requires_admin_role"
                class="text-error-600"
              >✓</span>
              <span
                v-else
                class="text-neutral-300"
              >—</span>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <!-- OIDC Signing Keys Section -->
    <div class="bg-surface shadow rounded-lg overflow-hidden mt-8">
      <div class="px-6 py-4 border-b">
        <h3 class="text-lg font-medium text-neutral-900">
          {{ $t('admin.settings.oidcSigningKeys') }}
        </h3>
      </div>

      <!-- Error Banner for OIDC Keys -->
      <div
        v-if="oidcErrorText"
        class="m-6 rounded-md bg-error-50 p-4"
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
              {{ oidcErrorText }}
            </p>
          </div>
        </div>
      </div>

      <div
        v-if="oidcLoading"
        class="p-6 text-center text-neutral-500"
      >
        {{ $t('common.loading') }}
      </div>

      <div
        v-else-if="oidcKeys"
        class="p-6 space-y-6"
      >
        <!-- Rotation summary -->
        <dl class="grid grid-cols-1 sm:grid-cols-3 gap-x-6 gap-y-4">
          <div>
            <dt class="text-sm font-medium text-neutral-500">
              {{ $t('admin.settings.keyCount') }}
            </dt>
            <dd class="mt-1">
              <DData :value="String(oidcKeys.key_count)" />
            </dd>
          </div>
          <div>
            <dt class="text-sm font-medium text-neutral-500">
              {{ $t('admin.settings.activeKeyId') }}
            </dt>
            <dd class="mt-1">
              <DData :value="oidcKeys.active_kid" />
            </dd>
          </div>
          <div>
            <dt class="text-sm font-medium text-neutral-500">
              {{ $t('admin.settings.status') }}
            </dt>
            <dd class="mt-1">
              <span class="inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium bg-success-100 text-success-700">
                {{ oidcKeys.status }}
              </span>
            </dd>
          </div>
        </dl>

        <!-- Loaded keys -->
        <div class="border-t pt-4">
          <table class="min-w-full text-sm">
            <thead>
              <tr class="text-left text-neutral-500">
                <th class="py-2 pr-4 font-medium">{{ $t('admin.settings.keyId') }}</th>
                <th class="py-2 pr-4 font-medium">{{ $t('admin.settings.keyType') }}</th>
                <th class="py-2 pr-4 font-medium">{{ $t('admin.settings.algorithm') }}</th>
                <th class="py-2 pr-4 font-medium">{{ $t('admin.settings.usage') }}</th>
                <th class="py-2 pr-4 font-medium">{{ $t('admin.settings.status') }}</th>
              </tr>
            </thead>
            <tbody class="text-neutral-700">
              <tr
                v-for="key in oidcKeys.keys"
                :key="key.kid"
                class="border-t"
              >
                <td class="py-2 pr-4 font-mono">
                  {{ key.kid }}
                  <span
                    v-if="key.kid === oidcKeys.active_kid"
                    class="ml-1 text-xs font-medium text-success-700"
                  >({{ $t('admin.settings.activeKey') }})</span>
                </td>
                <td class="py-2 pr-4">{{ key.kty }}</td>
                <td class="py-2 pr-4">{{ key.alg }}</td>
                <td class="py-2 pr-4">{{ key.use }}</td>
                <td class="py-2">
                  <span
                    class="inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium"
                    :class="key.status === 'active' ? 'bg-success-100 text-success-700' : 'bg-neutral-100 text-neutral-700'"
                  >
                    {{ key.status }}
                  </span>
                </td>
              </tr>
            </tbody>
          </table>
        </div>

        <!-- URLs -->
        <div class="border-t pt-4 space-y-3">
          <div class="flex items-center justify-between">
            <div>
              <span class="text-sm font-medium text-neutral-500">{{ $t('admin.settings.jwksEndpoint') }}</span>
              <div class="mt-1">
                <DData
                  :value="oidcKeys.jwks_uri"
                  truncate
                />
              </div>
            </div>
            <button
              class="ml-4 inline-flex items-center px-3 py-1.5 border border-neutral-300 text-xs font-medium rounded text-neutral-700 bg-surface hover:bg-neutral-50 focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-brand-500"
              @click="copyToClipboard(oidcKeys.jwks_uri, 'jwks')"
            >
              {{ copySuccess === 'jwks' ? $t('common.copied') : $t('common.copy') }}
            </button>
          </div>
          <div class="flex items-center justify-between">
            <div>
              <span class="text-sm font-medium text-neutral-500">{{ $t('admin.settings.discoveryEndpoint') }}</span>
              <div class="mt-1">
                <DData
                  :value="oidcKeys.discovery_uri"
                  truncate
                />
              </div>
            </div>
            <button
              class="ml-4 inline-flex items-center px-3 py-1.5 border border-neutral-300 text-xs font-medium rounded text-neutral-700 bg-surface hover:bg-neutral-50 focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-brand-500"
              @click="copyToClipboard(oidcKeys.discovery_uri, 'discovery')"
            >
              {{ copySuccess === 'discovery' ? $t('common.copied') : $t('common.copy') }}
            </button>
          </div>
        </div>

        <!-- Note -->
        <div class="border-t pt-4">
          <div class="rounded-md bg-brand-50 p-4">
            <div class="flex">
              <div class="flex-shrink-0">
                <svg
                  class="h-5 w-5 text-brand-400"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                  aria-hidden="true"
                >
                  <path
                    fill-rule="evenodd"
                    d="M18 10a8 8 0 11-16 0 8 8 0 0116 0zm-7-4a1 1 0 11-2 0 1 1 0 012 0zM9 9a.75.75 0 000 1.5h.253a.25.25 0 01.244.304l-.459 2.066A1.75 1.75 0 0010.747 15H11a.75.75 0 000-1.5h-.253a.25.25 0 01-.244-.304l.459-2.066A1.75 1.75 0 009.253 9H9z"
                    clip-rule="evenodd"
                  />
                </svg>
              </div>
              <div class="ml-3">
                <p class="text-sm text-brand-700">
                  {{ oidcKeys.note }}
                </p>
              </div>
            </div>
          </div>
        </div>
      </div>

      <div
        v-else
        class="p-6 text-center text-neutral-500"
      >
        {{ $t('admin.settings.loadFailed') }}
      </div>
    </div>
  </div>
</template>
