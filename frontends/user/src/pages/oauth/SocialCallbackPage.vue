<script setup lang="ts">
import { useRoute } from 'vue-router'
import { useSocialCallback } from '../../composables/useSocialCallback'
import { isSocialProvider } from '../../services/socialFlowContext'
const name = String(useRoute().meta.provider || '')
if (!isSocialProvider(name)) throw new Error('Unsupported provider route')
const providerLabel = name.charAt(0).toUpperCase() + name.slice(1)
const { errorText } = useSocialCallback(name)
</script>

<template>
  <div class="min-h-screen flex items-center justify-center">
    <div class="text-center max-w-md">
      <div
        v-if="errorText"
        class="rounded-lg bg-error-50 border border-error-200 p-4 text-sm text-error-700"
      >
        {{ errorText }}
      </div>
      <div
        v-else
        class="text-neutral-500"
      >
        {{ $t('oauth.social.completing', { provider: providerLabel }) }}
      </div>
    </div>
  </div>
</template>
