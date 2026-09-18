<script setup lang="ts">
import { ref, computed, watch } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useAuthStore } from '../../stores/auth'
import { useThemeStore } from '../../stores/theme'
import AppLogo from '../shared/AppLogo.vue'
import LocaleSwitcher from '../ui/LocaleSwitcher.vue'

const { t } = useI18n()
const auth = useAuthStore()
const theme = useThemeStore()
const router = useRouter()
const route = useRoute()

const sidebarCollapsed = ref(false)
const mobileMenuOpen = ref(false)
const userMenuOpen = ref(false)

// Close user menu on route change
watch(() => route.path, () => {
  userMenuOpen.value = false
  mobileMenuOpen.value = false
})

// Navigation items with SVG icon components (computed so labels follow the locale)
const navSections = computed(() => [
  {
    label: t('nav.overview'),
    items: [
      { name: t('nav.dashboard'), path: '/', icon: 'dashboard' },
    ],
  },
  {
    label: t('nav.management'),
    items: [
      { name: t('nav.applications'), path: '/applications', icon: 'apps' },
      { name: t('nav.organizations'), path: '/organizations', icon: 'roles' },
      { name: t('nav.users'), path: '/users', icon: 'users' },
      { name: t('nav.roles'), path: '/roles', icon: 'roles' },
      { name: t('nav.scopes'), path: '/scopes', icon: 'scopes' },
      { name: t('nav.tokens'), path: '/tokens', icon: 'tokens' },
      { name: t('nav.devices'), path: '/devices', icon: 'devices' },
    ],
  },
  {
    label: t('nav.monitoring'),
    items: [
      { name: t('nav.auditLogs'), path: '/logs', icon: 'logs' },
      { name: t('nav.settings'), path: '/settings', icon: 'settings' },
    ],
  },
])

function isActive(path: string): boolean {
  if (path === '/') return route.path === '/'
  return route.path.startsWith(path)
}

async function handleLogout() {
  await auth.logout()
  router.push('/login')
}

// Breadcrumb: top-level route segments map to the localized nav labels;
// unknown segments (resource ids) keep the capitalized raw value.
const CRUMB_KEYS: Record<string, string> = {
  applications: 'nav.applications',
  users: 'nav.users',
  roles: 'nav.roles',
  scopes: 'nav.scopes',
  tokens: 'nav.tokens',
  devices: 'nav.devices',
  logs: 'nav.auditLogs',
  settings: 'nav.settings',
}

const breadcrumbs = computed(() => {
  const parts = route.path.split('/').filter(Boolean)
  if (parts.length === 0) return [{ name: t('nav.dashboard'), path: '/' }]

  const crumbs = [{ name: t('nav.dashboard'), path: '/' }]
  let current = ''
  for (const part of parts) {
    current += '/' + part
    crumbs.push({
      name: CRUMB_KEYS[part]
        ? t(CRUMB_KEYS[part])
        : part.charAt(0).toUpperCase() + part.slice(1).replace(/-/g, ' '),
      path: current,
    })
  }
  return crumbs
})

// Display name fallback when no user is loaded
const displayName = computed(() => auth.user?.name || t('adminLayout.defaultUser'))
</script>

<template>
  <div class="flex h-screen overflow-hidden bg-page">
    <!-- ============================================================ -->
    <!-- MOBILE OVERLAY -->
    <!-- ============================================================ -->
    <div
      v-if="mobileMenuOpen"
      class="fixed inset-0 z-40 lg:hidden"
      @click="mobileMenuOpen = false"
    >
      <div class="absolute inset-0 bg-neutral-900/50 backdrop-blur-sm" />
    </div>

    <!-- ============================================================ -->
    <!-- SIDEBAR -->
    <!-- ============================================================ -->
    <aside
      :class="[
        'fixed lg:static inset-y-0 left-0 z-50',
        'flex flex-col bg-surface border-r border-neutral-200',
        'transition-all duration-300 ease-out',
        mobileMenuOpen ? 'translate-x-0' : '-translate-x-full lg:translate-x-0',
        sidebarCollapsed ? 'w-[72px]' : 'w-[260px]',
      ]"
    >
      <!-- Logo -->
      <div
        :class="[
          'flex items-center h-16 shrink-0 border-b border-neutral-100',
          sidebarCollapsed ? 'justify-center px-2' : 'px-5',
        ]"
      >
        <router-link
          to="/"
          class="flex items-center"
          @click="mobileMenuOpen = false"
        >
          <AppLogo :size="sidebarCollapsed ? 'sm' : 'md'" />
        </router-link>
      </div>

      <!-- Navigation -->
      <nav class="flex-1 overflow-y-auto py-4 px-3 space-y-6">
        <div
          v-for="section in navSections"
          :key="section.label"
        >
          <p
            v-if="!sidebarCollapsed"
            class="px-3 mb-1.5 text-xs font-semibold text-neutral-400 uppercase tracking-wider"
          >
            {{ section.label }}
          </p>

          <ul class="space-y-0.5">
            <li
              v-for="item in section.items"
              :key="item.path"
            >
              <router-link
                :to="item.path"
                :class="[
                  'relative flex items-center gap-3 rounded-ctl transition-all duration-150 group',
                  sidebarCollapsed ? 'justify-center px-0 py-2.5' : 'px-3 py-2.5',
                  isActive(item.path)
                    ? 'nav-active bg-brand-50 text-brand-700 font-semibold'
                    : 'text-neutral-600 hover:text-neutral-900 hover:bg-neutral-50',
                ]"
                :title="sidebarCollapsed ? item.name : undefined"
                @click="mobileMenuOpen = false"
              >
                <!-- Dashboard icon -->
                <svg
                  v-if="item.icon === 'dashboard'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M2 4.75A.75.75 0 012.75 4h14.5a.75.75 0 010 1.5H2.75A.75.75 0 012 4.75zM2 10a.75.75 0 01.75-.75h4.5a.75.75 0 010 1.5h-4.5A.75.75 0 012 10zm8.25-.75a.75.75 0 01.75-.75h4.5a.75.75 0 010 1.5h-4.5a.75.75 0 01-.75-.75zM2 15.25a.75.75 0 01.75-.75h4.5a.75.75 0 010 1.5h-4.5a.75.75 0 01-.75-.75zm8.25-.75a.75.75 0 01.75-.75h4.5a.75.75 0 010 1.5h-4.5a.75.75 0 01-.75-.75z"
                  />
                </svg>

                <!-- Applications icon -->
                <svg
                  v-else-if="item.icon === 'apps'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M2.75 4.5A2.25 2.25 0 015 2.25h2.5A2.25 2.25 0 019.75 4.5v2.5A2.25 2.25 0 017.5 9.25H5a2.25 2.25 0 01-2.25-2.25v-2.5zM5 3.75a.75.75 0 00-.75.75v2.5c0 .414.336.75.75.75h2.5a.75.75 0 00.75-.75v-2.5a.75.75 0 00-.75-.75H5zm6 6a.75.75 0 01.75-.75h5.5a.75.75 0 010 1.5h-5.5a.75.75 0 01-.75-.75zm.75 3.25a.75.75 0 100 1.5h5.5a.75.75 0 100-1.5h-5.5zm0 4a.75.75 0 100 1.5h5.5a.75.75 0 100-1.5h-5.5zM5.75 9a.75.75 0 01.75.75v5.5a.75.75 0 01-1.5 0v-5.5a.75.75 0 01.75-.75z"
                  />
                </svg>

                <!-- Users icon -->
                <svg
                  v-else-if="item.icon === 'users'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path d="M7 10a3 3 0 100-6 3 3 0 000 6zM3.5 11.5A3.5 3.5 0 000 15v1.25a.75.75 0 001.5 0V15a2 2 0 012-2h7a2 2 0 012 2v1.25a.75.75 0 001.5 0V15a3.5 3.5 0 00-3.5-3.5h-7zM14 10a2.5 2.5 0 100-5 2.5 2.5 0 000 5zM12.25 11.5a.75.75 0 000 1.5h4.5a2.25 2.25 0 012.25 2.25v1a.75.75 0 001.5 0v-1A3.75 3.75 0 0016.75 11.5h-4.5z" />
                </svg>

                <!-- Roles icon -->
                <svg
                  v-else-if="item.icon === 'roles'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M10 3a1.5 1.5 0 00-1.5 1.5A1.5 1.5 0 007 4.5 1.5 1.5 0 005.5 6 1.5 1.5 0 007 7.5 1.5 1.5 0 008.5 6a1.5 1.5 0 001.5-1.5A1.5 1.5 0 0010 3zM7 8.5a3 3 0 100 6 3 3 0 000-6zm7-4a1 1 0 100-2 1 1 0 000 2zm-1.5 3.5a1 1 0 112 0 1 1 0 01-2 0zm2.5 3.5a.75.75 0 100-1.5.75.75 0 000 1.5z"
                  />
                </svg>

                <!-- Scopes icon -->
                <svg
                  v-else-if="item.icon === 'scopes'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M8 2a2 2 0 00-2 2v1H4a2 2 0 00-2 2v9a2 2 0 002 2h12a2 2 0 002-2V7a2 2 0 00-2-2h-2V4a2 2 0 00-2-2H8zm3.5 3H12V4a.5.5 0 00-.5-.5h-3a.5.5 0 00-.5.5v1h3.5zM4 7h12v9H4V7z"
                  />
                </svg>

                <!-- Tokens icon -->
                <svg
                  v-else-if="item.icon === 'tokens'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M10 2a4 4 0 00-4 4v4H4a2 2 0 00-2 2v4a2 2 0 002 2h12a2 2 0 002-2v-4a2 2 0 00-2-2h-2V6a4 4 0 00-4-4zm1.5 6.5a1 1 0 11-2 0 1 1 0 012 0zM4 14h12v4H4v-4z"
                  />
                </svg>

                <!-- Devices icon (device approval) -->
                <svg
                  v-else-if="item.icon === 'devices'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M8 2a2 2 0 00-2 2v12a2 2 0 002 2h4a2 2 0 002-2V4a2 2 0 00-2-2H8zm.75 13.5a.75.75 0 000 1.5h2.5a.75.75 0 000-1.5h-2.5z"
                  />
                </svg>

                <!-- Logs icon -->
                <svg
                  v-else-if="item.icon === 'logs'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M4 3.5A1.5 1.5 0 015.5 2h9A1.5 1.5 0 0116 3.5v13a1.5 1.5 0 01-1.5 1.5h-9A1.5 1.5 0 014 16.5v-13zM6 6a.75.75 0 01.75-.75h6.5a.75.75 0 010 1.5h-6.5A.75.75 0 016 6zm.75 3.25a.75.75 0 000 1.5h5.5a.75.75 0 000-1.5h-5.5zM6 12.5a.75.75 0 01.75-.75h6.5a.75.75 0 010 1.5h-6.5A.75.75 0 016 12.5z"
                  />
                </svg>

                <!-- Settings icon -->
                <svg
                  v-else-if="item.icon === 'settings'"
                  class="w-5 h-5 shrink-0"
                  viewBox="0 0 20 20"
                  fill="currentColor"
                >
                  <path
                    fill-rule="evenodd"
                    d="M7.84 1.804A1 1 0 018.82 1h2.36a1 1 0 01.98.804l.331 1.652a6.993 6.993 0 011.929 1.115l1.598-.54a1 1 0 011.186.447l1.18 2.044a1 1 0 01-.205 1.251l-1.267 1.113a7.047 7.047 0 010 2.228l1.267 1.113a1 1 0 01.205 1.251l-1.18 2.044a1 1 0 01-1.186.447l-1.598-.54a6.993 6.993 0 01-1.929 1.115l-.33 1.652a1 1 0 01-.98.804H8.82a1 1 0 01-.98-.804l-.331-1.652a6.993 6.993 0 01-1.929-1.115l-1.598.54a1 1 0 01-1.186-.447l-1.18-2.044a1 1 0 01.205-1.251l1.267-1.113a7.047 7.047 0 010-2.228L1.822 7.71a1 1 0 01-.205-1.251l1.18-2.044a1 1 0 011.186-.447l1.598.54A6.993 6.993 0 017.51 3.456l.33-1.652zM10 13a3 3 0 100-6 3 3 0 000 6z"
                  />
                </svg>

                <span
                  v-if="!sidebarCollapsed"
                  class="text-sm"
                  :class="isActive(item.path) ? 'font-semibold' : 'font-normal'"
                >
                  {{ item.name }}
                </span>
              </router-link>
            </li>
          </ul>
        </div>
      </nav>

      <!-- Sidebar footer -->
      <div class="shrink-0 border-t border-neutral-100 p-3">
        <!-- Collapse toggle -->
        <button
          class="hidden lg:flex items-center gap-3 w-full rounded-ctl px-3 py-2.5 text-sm text-neutral-500
                 hover:text-neutral-700 hover:bg-neutral-100 transition-colors"
          :class="sidebarCollapsed ? 'justify-center' : ''"
          @click="sidebarCollapsed = !sidebarCollapsed"
        >
          <svg
            class="w-4 h-4 shrink-0 transition-transform duration-300"
            :class="sidebarCollapsed ? 'rotate-180' : ''"
            viewBox="0 0 16 16"
            fill="currentColor"
          >
            <path
              fill-rule="evenodd"
              d="M10.22 3.22a.75.75 0 011.06 1.06L7.56 8l3.72 3.72a.75.75 0 11-1.06 1.06L5.94 8.53a.75.75 0 010-1.06l4.28-4.25z"
            />
          </svg>
          <span v-if="!sidebarCollapsed">{{ $t('adminLayout.collapse') }}</span>
        </button>
      </div>
    </aside>

    <!-- ============================================================ -->
    <!-- MAIN CONTENT -->
    <!-- ============================================================ -->
    <div class="flex-1 flex flex-col min-w-0 overflow-hidden">
      <!-- Top Header -->
      <header class="h-16 shrink-0 bg-surface border-b border-neutral-200 flex items-center justify-between px-4 lg:px-6">
        <!-- Left: Mobile toggle + breadcrumb -->
        <div class="flex items-center gap-4 min-w-0">
          <button
            class="lg:hidden p-2 -ml-2 rounded-ctl text-neutral-500 hover:text-neutral-700 hover:bg-neutral-100
                   focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
            :aria-label="$t('adminLayout.toggleMenu')"
            @click="mobileMenuOpen = !mobileMenuOpen"
          >
            <svg
              class="w-5 h-5"
              viewBox="0 0 20 20"
              fill="currentColor"
            >
              <path
                fill-rule="evenodd"
                d="M2 4.75A.75.75 0 012.75 4h14.5a.75.75 0 010 1.5H2.75A.75.75 0 012 4.75zM2 10a.75.75 0 01.75-.75h14.5a.75.75 0 010 1.5H2.75A.75.75 0 012 10zm0 5.25a.75.75 0 01.75-.75h14.5a.75.75 0 010 1.5H2.75a.75.75 0 01-.75-.75z"
              />
            </svg>
          </button>

          <!-- Breadcrumbs (desktop) -->
          <nav
            class="hidden sm:flex items-center gap-1.5 text-sm min-w-0"
            :aria-label="$t('adminLayout.breadcrumb')"
          >
            <template
              v-for="(crumb, idx) in breadcrumbs"
              :key="crumb.path"
            >
              <svg
                v-if="idx > 0"
                class="w-4 h-4 text-neutral-300 shrink-0"
                viewBox="0 0 16 16"
                fill="currentColor"
              >
                <path
                  fill-rule="evenodd"
                  d="M6.22 3.22a.75.75 0 011.06 0l4.25 4.25a.75.75 0 010 1.06l-4.25 4.25a.75.75 0 01-1.06-1.06L9.94 8 6.22 4.28a.75.75 0 010-1.06z"
                />
              </svg>
              <router-link
                v-if="idx < breadcrumbs.length - 1"
                :to="crumb.path"
                class="text-neutral-400 hover:text-neutral-600 transition-colors truncate"
              >
                {{ crumb.name }}
              </router-link>
              <span
                v-else
                class="text-neutral-900 font-medium truncate"
              >
                {{ crumb.name }}
              </span>
            </template>
          </nav>
        </div>

        <!-- Right: Locale + User -->
        <div class="flex items-center gap-3">
          <!-- Locale switcher (desktop + mobile) -->
          <div class="flex items-center">
            <LocaleSwitcher />
          </div>

          <!-- User menu -->
          <div class="relative">
            <button
              class="flex items-center gap-2.5 px-2 py-1.5 rounded-ctl hover:bg-neutral-100 transition-colors
                     focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
              @click="userMenuOpen = !userMenuOpen"
            >
              <div class="w-8 h-8 rounded-full bg-brand-100 text-brand-700 flex items-center justify-center text-xs font-semibold">
                {{ displayName[0].toUpperCase() }}
              </div>
              <span class="hidden sm:block text-sm font-medium text-neutral-700">
                {{ displayName }}
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
                v-if="userMenuOpen"
                class="absolute right-0 mt-2 w-64 bg-surface rounded-xl shadow-lg border border-neutral-200 py-1.5 z-50"
              >
                <div class="px-4 py-3 border-b border-neutral-100">
                  <p class="text-sm font-medium text-neutral-900">
                    {{ displayName }}
                  </p>
                  <p class="text-xs text-neutral-500 mt-0.5">
                    {{ $t('adminLayout.administrator') }}
                  </p>
                </div>

                <router-link
                  to="/settings"
                  class="flex items-center gap-3 px-4 py-2.5 text-sm text-neutral-700 hover:bg-neutral-50 transition-colors"
                  @click="userMenuOpen = false"
                >
                  <svg
                    class="w-4 h-4 text-neutral-400"
                    viewBox="0 0 16 16"
                    fill="currentColor"
                  >
                    <path
                      fill-rule="evenodd"
                      d="M7.84 1.804A1 1 0 018.82 1h2.36a1 1 0 01.98.804l.331 1.652a6.993 6.993 0 011.929 1.115l1.598-.54a1 1 0 011.186.447l1.18 2.044a1 1 0 01-.205 1.251l-1.267 1.113a7.047 7.047 0 010 2.228l1.267 1.113a1 1 0 01.205 1.251l-1.18 2.044a1 1 0 01-1.186.447l-1.598-.54a6.993 6.993 0 01-1.929 1.115l-.33 1.652a1 1 0 01-.98.804H8.82a1 1 0 01-.98-.804l-.331-1.652a6.993 6.993 0 01-1.929-1.115l-1.598.54a1 1 0 01-1.186-.447l-1.18-2.044a1 1 0 01.205-1.251l1.267-1.113a7.047 7.047 0 010-2.228L1.822 7.71a1 1 0 01-.205-1.251l1.18-2.044a1 1 0 011.186-.447l1.598.54A6.993 6.993 0 017.51 3.456l.33-1.652zM8 10a2 2 0 100-4 2 2 0 000 4z"
                    />
                  </svg>
                  {{ $t('nav.settings') }}
                </router-link>

                <button
                  class="flex items-center gap-3 w-full px-4 py-2.5 text-sm text-neutral-700 hover:bg-neutral-50 transition-colors
                         focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring rounded-ctl"
                  :aria-label="theme.mode === 'dark' ? $t('adminLayout.switchToLightTheme') : $t('adminLayout.switchToDarkTheme')"
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
                  {{ theme.mode === 'dark' ? $t('adminLayout.lightTheme') : $t('adminLayout.darkTheme') }}
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
                  {{ $t('nav.signOut') }}
                </button>
              </div>
            </Transition>
          </div>
        </div>
      </header>

      <!-- Page Content -->
      <main class="flex-1 overflow-y-auto">
        <div class="max-w-[1280px] mx-auto p-6 lg:p-8">
          <router-view />
        </div>
      </main>
    </div>

    <!-- Click-outside for user menu -->
    <div
      v-if="userMenuOpen"
      class="fixed inset-0 z-40"
      @click="userMenuOpen = false"
    />
  </div>
</template>

<style scoped>
.dropdown-enter-active { transition: all 150ms ease-out; }
.dropdown-leave-active { transition: all 100ms ease-in; }
.dropdown-enter-from { opacity: 0; transform: translateY(-8px) scale(0.96); }
.dropdown-leave-to { opacity: 0; transform: translateY(-4px) scale(0.98); }

/* Active nav tick — 2px brand rule at the sidebar's left edge */
.nav-active::before {
  content: '';
  position: absolute;
  left: -12px;
  top: 7px;
  bottom: 7px;
  width: 2px;
  border-radius: 2px;
  background: var(--color-brand-600);
}
</style>
