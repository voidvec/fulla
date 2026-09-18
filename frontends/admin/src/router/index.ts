import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from '../stores/auth'

const router = createRouter({
  history: createWebHistory('/admin/'),
  routes: [
    {
      path: '/login',
      name: 'login',
      component: () => import('../pages/LoginPage.vue'),
      meta: { requiresAuth: false },
    },
    // Note: no /callback route — the admin SPA logs in via the json-mode
    // /oauth2/login + /oauth2/token flow and never receives a browser
    // redirect. The former CallbackPage called a nonexistent store method
    // (gap-fix E1 dead code) and has been removed. The redirect_uri passed
    // to login/token remains the registered '.../admin/callback' string.
    {
      path: '/',
      component: () => import('../components/layout/AdminLayout.vue'),
      meta: { requiresAuth: true },
      children: [
        {
          path: '',
          name: 'dashboard',
          component: () => import('../pages/dashboard/DashboardPage.vue'),
        },
        {
          path: 'applications',
          name: 'applications',
          component: () => import('../pages/applications/ApplicationsPage.vue'),
        },
        {
          path: 'applications/:id',
          name: 'application-detail',
          component: () => import('../pages/applications/ApplicationDetailPage.vue'),
        },
        {
          path: 'organizations',
          name: 'organizations',
          component: () => import('../pages/organizations/OrganizationsPage.vue'),
        },
        {
          path: 'users',
          name: 'users',
          component: () => import('../pages/users/UsersPage.vue'),
        },
        {
          path: 'users/:id',
          name: 'user-detail',
          component: () => import('../pages/users/UserDetailPage.vue'),
        },
        {
          path: 'roles',
          name: 'roles',
          component: () => import('../pages/roles/RolesPage.vue'),
        },
        {
          path: 'scopes',
          name: 'scopes',
          component: () => import('../pages/scopes/ScopesPage.vue'),
        },
        {
          path: 'logs',
          name: 'logs',
          component: () => import('../pages/logs/LogsPage.vue'),
        },
        {
          path: 'tokens',
          name: 'tokens',
          component: () => import('../pages/tokens/TokensPage.vue'),
        },
        {
          path: 'devices',
          name: 'devices',
          component: () => import('../pages/devices/DeviceApprovePage.vue'),
        },
        {
          path: 'settings',
          name: 'settings',
          component: () => import('../pages/settings/SettingsPage.vue'),
        },
      ],
    },
  ],
})

router.beforeEach(async (to, _from, next) => {
  const auth = useAuthStore()
  if (to.meta.requiresAuth !== false && !auth.isAuthenticated) {
    // Wait for the one-shot session restoration before deciding. On a fresh
    // load with a valid persisted refresh token, this flips isAuthenticated to
    // true so the user is not bounced to /login. See A-LOGIN-014.
    const restored = await auth.ensureSessionRestored()
    if (restored) {
      next()
    } else {
      next({ name: 'login' })
    }
  } else {
    next()
  }
})

export default router
