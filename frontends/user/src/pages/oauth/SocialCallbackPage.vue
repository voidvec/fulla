<script setup lang="ts">
import { useRoute } from 'vue-router'
import { useSocialCallback } from '../../composables/useSocialCallback'
import { isSocialProvider } from '../../services/socialFlowContext'
const name = String(useRoute().meta.provider || '')
if (!isSocialProvider(name)) throw new Error('Unsupported provider route')
const providerLabel = name.charAt(0).toUpperCase() + name.slice(1)
const { errorText, needsLinking } = useSocialCallback(name)
</script>

<template>
  <div class="min-h-screen flex items-center justify-center">
    <div class="text-center max-w-md">
      <div
        v-if="errorText"
        class="rounded-lg bg-error-50 border border-error-200 p-4 text-sm text-error-700"
      >
        {{ errorText }}
        <!-- The account is not linked: offer the way forward (the link flow
             lives on the security page). The route needs a session, so a
             signed-out visitor is sent through sign-in first and lands here
             afterwards. -->
        <p
          v-if="needsLinking"
          class="mt-3"
        >
          <router-link
            to="/security"
            data-testid="social-link-guide"
            class="text-brand-700 font-medium hover:text-brand-800 rounded-ctl
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          >
            {{ $t('oauth.social.linkGuide') }}
          </router-link>
        </p>
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
