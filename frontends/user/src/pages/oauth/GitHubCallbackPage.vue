<script setup lang="ts">
import { useSocialCallback } from '../../composables/useSocialCallback'
const { errorText } = useSocialCallback('github')
</script>

<template>
  <div class="min-h-screen flex items-center justify-center bg-page blueprint-grid px-4">
    <div class="fixed inset-x-0 top-0 h-[3px] bg-brand-600 z-10" />
    <div class="text-center max-w-md w-full">
      <div
        v-if="errorText"
        class="p-6 bg-surface border border-error-200 rounded-card shadow-sm text-left"
      >
        <p class="text-error-700 font-medium">
          {{ $t('oauth.github.errorTitle') }}
        </p>
        <p class="text-error-600 text-sm mt-2">
          {{ errorText }}
        </p>
        <router-link
          to="/login"
          class="mt-4 inline-block text-brand-600 hover:text-brand-800 rounded-ctl
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
        >
          {{ $t('common.backToLogin') }}
        </router-link>
      </div>
      <div v-else>
        <div class="animate-spin w-8 h-8 border-4 border-brand-600 border-t-transparent rounded-full mx-auto" />
        <p class="mt-4 text-neutral-600">
          {{ $t('oauth.github.completing') }}
        </p>
      </div>
    </div>
  </div>
</template>
