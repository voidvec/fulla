<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useAuthStore } from '../stores/auth'
import { useThemeStore } from '../stores/theme'
import AppLogo from '../components/shared/AppLogo.vue'
import LocaleSwitcher from '../components/ui/LocaleSwitcher.vue'

const { t } = useI18n()
const auth = useAuthStore()
const theme = useThemeStore()
const router = useRouter()
const route = useRoute()
const showUserMenu = ref(false)

watch(() => route.path, () => {
  showUserMenu.value = false
})

async function handleLogout() {
  await auth.logout()
  router.push('/login')
}

const navItems = computed(() => [
  { name: t('nav.overview'), path: '/', icon: 'dashboard' },
  { name: t('nav.profile'), path: '/profile', icon: 'profile' },
  { name: t('nav.security'), path: '/security', icon: 'security' },
  { name: t('nav.authorizedApps'), path: '/authorized-apps', icon: 'apps' },
  { name: t('nav.applications'), path: '/apps', icon: 'apps' },
  { name: t('nav.organizations'), path: '/organizations', icon: 'dashboard' },
])
</script>

<template>
  <div class="min-h-screen bg-page">
    <!-- Top Navigation -->
    <header class="bg-surface border-b border-neutral-200 sticky top-0 z-40">
      <div class="max-w-6xl mx-auto px-4 sm:px-6 lg:px-8">
        <div class="flex justify-between h-16">
          <!-- Left: Logo + Nav -->
          <div class="flex items-center gap-8">
            <router-link
              to="/"
              class="shrink-0"
            >
              <AppLogo />
            </router-link>
            <nav class="hidden md:flex items-center gap-0.5">
              <router-link
                v-for="item in navItems"
                :key="item.path"
                :to="item.path"
                class="relative px-3 py-2 rounded-ctl text-sm transition-colors"
                :class="route.path === item.path
                  ? 'text-brand-700 font-semibold'
                  : 'text-neutral-600 font-medium hover:text-neutral-900 hover:bg-neutral-50'"
              >
                {{ item.name }}
                <div
                  v-if="route.path === item.path"
                  class="absolute bottom-0 left-3 right-3 h-0.5 bg-brand-600 rounded-[2px]"
                />
              </router-link>
            </nav>
          </div>

          <!-- Right: Locale switcher + User Menu -->
          <div class="flex items-center gap-2">
            <LocaleSwitcher class="flex items-center" />
            <div class="relative">
              <button
                class="flex items-center gap-2.5 px-2 py-1.5 rounded-ctl hover:bg-neutral-100 transition-colors
                       focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
                @click="showUserMenu = !showUserMenu"
              >
                <div class="w-8 h-8 rounded-full bg-brand-100 text-brand-700 flex items-center justify-center text-xs font-semibold">
                  {{ (auth.user?.name || 'U')[0].toUpperCase() }}
                </div>
                <span class="hidden sm:block text-sm font-medium text-neutral-700">
                  {{ auth.user?.name || t('common.user') }}
                </span>
                <svg
                  class="w-4 h-4 text-neutral-400"
                  viewBox="0 0 16 16"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M4.22 6.22a.75.75 0 011.06 0L8 8.94l2.72-2.72a.75.75 0 111.06 1.06l-3.25 3.25a.75.75 0 01-1.06 0L4.22 7.28a.75.75 0 010-1.06z"
                  />
                </svg>
              </button>

              <!-- Dropdown -->
              <Transition name="dropdown">
                <div
                  v-if="showUserMenu"
                  class="absolute right-0 mt-2 w-56 bg-surface rounded-card shadow-lg border border-neutral-200 py-1.5 z-50"
                >
                  <div class="px-4 py-3 border-b border-neutral-100">
                    <p class="text-sm font-medium text-neutral-900">
                      {{ auth.user?.name }}
                    </p>
                    <p class="text-xs text-neutral-500 mt-0.5 truncate">
                      {{ auth.user?.email }}
                    </p>
                  </div>

                  <router-link
                    to="/profile"
                    class="flex items-center gap-3 px-4 py-2.5 text-sm text-neutral-700 hover:bg-neutral-50 transition-colors"
                    @click="showUserMenu = false"
                  >
                    <svg
                      class="w-4 h-4 text-neutral-400"
                      viewBox="0 0 16 16"
                      fill="currentColor"
                    >
                      <path
                        fill-rule="evenodd"
                        d="M7 8a3 3 0 100-6 3 3 0 000 6zm-2.5 3.5A3.5 3.5 0 001 15v.75a.75.75 0 001.5 0V15a2 2 0 012-2h7a2 2 0 012 2v.75a.75.75 0 001.5 0V15a3.5 3.5 0 00-3.5-3.5h-7z"
                      />
                    </svg>
                    {{ t('nav.profile') }}
                  </router-link>

                  <router-link
                    to="/security"
                    class="flex items-center gap-3 px-4 py-2.5 text-sm text-neutral-700 hover:bg-neutral-50 transition-colors"
                    @click="showUserMenu = false"
                  >
                    <svg
                      class="w-4 h-4 text-neutral-400"
                      viewBox="0 0 16 16"
                      fill="currentColor"
                    >
                      <path
                        fill-rule="evenodd"
                        d="M8 2a3.5 3.5 0 00-3.5 3.5v2.382l-.964.643A1.5 1.5 0 003 9.862v.638a1.5 1.5 0 001.5 1.5h7a1.5 1.5 0 001.5-1.5v-.638a1.5 1.5 0 00-.536-1.137l-.964-.643V5.5A3.5 3.5 0 008 2z"
                      />
                    </svg>
                    {{ t('nav.security') }}
                  </router-link>

                  <button
                    class="flex items-center gap-3 w-full px-4 py-2.5 text-sm text-neutral-700 hover:bg-neutral-50 transition-colors
                           focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring rounded-ctl"
                    :aria-label="theme.mode === 'dark' ? t('appLayout.switchToLightTheme') : t('appLayout.switchToDarkTheme')"
                    @click="theme.toggle()"
                  >
                    <svg
                      class="w-4 h-4 text-neutral-400"
                      viewBox="0 0 16 16"
                      fill="currentColor"
                      aria-hidden="true"
                    >
                      <path
                        v-if="theme.mode === 'dark'"
                        fill-rule="evenodd"
                        d="M8 1a.75.75 0 01.75.75v1.5a.75.75 0 01-1.5 0v-1.5A.75.75 0 018 1zm0 10.5a3.5 3.5 0 100-7 3.5 3.5 0 000 7zm4.95-7.95a.75.75 0 010 1.06l-1.06 1.06a.75.75 0 11-1.06-1.06l1.06-1.06a.75.75 0 011.06 0zM8 14.5a.75.75 0 01-.75-.75v-1.5a.75.75 0 011.5 0v1.5a.75.75 0 01-.75.75zM3.05 4.55a.75.75 0 011.06 0l1.06 1.06A.75.75 0 114.11 6.67L3.05 5.61a.75.75 0 010-1.06zM3.5 8.75a.75.75 0 000-1.5H2a.75.75 0 000 1.5h1.5zm9.5 0a.75.75 0 000-1.5h1.5a.75.75 0 010 1.5H13zM4.11 9.39a.75.75 0 011.06 1.06l-1.06 1.06a.75.75 0 11-1.06-1.06l1.06-1.06zm7.78 0 1.06 1.06a.75.75 0 11-1.06 1.06l-1.06-1.06a.75.75 0 111.06-1.06z"
                      />
                      <path
                        v-else
                        fill-rule="evenodd"
                        d="M9.598 1.591a.748.748 0 01.785-.175 7.001 7.001 0 1010.467 7.467 7 7 0 00-10.467-7.467.75.75 0 01-.785-.175z"
                        clip-rule="evenodd"
                        transform="scale(0.7)"
                      />
                    </svg>
                    {{ theme.mode === 'dark' ? t('appLayout.lightTheme') : t('appLayout.darkTheme') }}
                  </button>

                  <div class="border-t border-neutral-100 my-1" />

                  <button
                    class="flex items-center gap-3 w-full px-4 py-2.5 text-sm text-error-600 hover:bg-error-50 transition-colors"
                    @click="handleLogout"
                  >
                    <svg
                      class="w-4 h-4"
                      viewBox="0 0 16 16"
                      fill="currentColor"
                    >
                      <path
                        fill-rule="evenodd"
                        d="M3.75 2A1.75 1.75 0 002 3.75v8.5C2 13.216 2.784 14 3.75 14h2.5a.75.75 0 000-1.5h-2.5a.25.25 0 01-.25-.25v-8.5a.25.25 0 01.25-.25h2.5a.75.75 0 000-1.5h-2.5zm6.97.47a.75.75 0 011.06 0l4.5 4.5a.75.75 0 010 1.06l-4.5 4.5a.75.75 0 11-1.06-1.06L13.94 8.75H6a.75.75 0 010-1.5h7.94l-3.22-3.22a.75.75 0 010-1.06z"
                      />
                    </svg>
                    {{ t('nav.signOut') }}
                  </button>
                </div>
              </Transition>
            </div>
          </div>
        </div>
      </div>
    </header>

    <!-- Page Content -->
    <main class="max-w-6xl mx-auto px-4 sm:px-6 lg:px-8 py-8">
      <router-view />
    </main>

    <!-- Footer -->
    <footer class="border-t border-neutral-100 py-4">
      <p class="text-center text-xs text-neutral-400">
        {{ t('appLayout.footer') }}
      </p>
    </footer>
  </div>

  <!-- Click-outside for user menu -->
  <div
    v-if="showUserMenu"
    class="fixed inset-0 z-30"
    @click="showUserMenu = false"
  />
</template>

<style scoped>
.dropdown-enter-active { transition: all 150ms ease-out; }
.dropdown-leave-active { transition: all 100ms ease-in; }
.dropdown-enter-from { opacity: 0; transform: translateY(-8px) scale(0.96); }
.dropdown-leave-to { opacity: 0; transform: translateY(-4px) scale(0.98); }
</style>
