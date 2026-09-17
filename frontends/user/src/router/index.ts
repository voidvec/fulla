import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from '../stores/auth'

const router = createRouter({
  history: createWebHistory(),
  routes: [
    // Auth pages (AuthLayout)
    {
      path: '/login',
      name: 'login',
      component: () => import('../pages/auth/LoginPage.vue'),
      meta: { guest: true, layout: 'auth' },
    },
    {
      path: '/register',
      name: 'register',
      component: () => import('../pages/auth/RegisterPage.vue'),
      meta: { guest: true, layout: 'auth' },
    },
    {
      path: '/forgot-password',
      name: 'forgot-password',
      component: () => import('../pages/auth/ForgotPasswordPage.vue'),
      meta: { guest: true, layout: 'auth' },
    },
    {
      path: '/reset-password',
      name: 'reset-password',
      component: () => import('../pages/auth/ResetPasswordPage.vue'),
      meta: { guest: true, layout: 'auth' },
    },
    {
      path: '/verify-email',
      name: 'verify-email',
      component: () => import('../pages/auth/VerifyEmailPage.vue'),
      meta: { layout: 'auth' },
    },

    // OAuth protocol pages (standalone)
    {
      path: '/callback',
      name: 'callback',
      component: () => import('../pages/oauth/CallbackPage.vue'),
    },
    {
      path: '/callback/github',
      name: 'github-callback',
      component: () => import('../pages/oauth/GitHubCallbackPage.vue'),
    },
    // #70: Google/WeChat login callbacks — the generalized
    // SocialCallbackPage with the provider carried in route meta.
    {
      path: '/callback/google',
      name: 'google-callback',
      component: () => import('../pages/oauth/SocialCallbackPage.vue'),
      meta: { provider: 'google' },
    },
    {
      path: '/callback/wechat',
      name: 'wechat-callback',
      component: () => import('../pages/oauth/SocialCallbackPage.vue'),
      meta: { provider: 'wechat' },
    },
    {
      path: '/consent',
      name: 'consent',
      component: () => import('../pages/oauth/ConsentPage.vue'),
      // Gap-fix E7: the consent form needs the session user id; an anonymous
      // visit used to render a form that could only submit user_id='' and 500.
      // The guard restores the session (or sends the full target — query
      // included — to /login via the redirect param) before rendering.
      meta: { layout: 'auth', auth: true },
    },
    // Note: no /device/verify route — the page called a nonexistent endpoint
    // (/oauth2/device/verify), and the real /oauth2/device/approve is
    // admin-gated server-side, so device approval moved to the admin console
    // (gap-fix E2 / plan D5).

    // Protected account pages (AppLayout)
    {
      path: '/',
      component: () => import('../layouts/AppLayout.vue'),
      meta: { auth: true },
      children: [
        { path: '', name: 'dashboard', component: () => import('../pages/account/DashboardPage.vue') },
        { path: 'profile', name: 'profile', component: () => import('../pages/account/ProfilePage.vue') },
        { path: 'security', name: 'security', component: () => import('../pages/account/SecurityPage.vue') },
        { path: 'authorized-apps', name: 'authorized-apps', component: () => import('../pages/account/AuthorizedAppsPage.vue') },
        // v1.4.0: open platform + organizations (self-service).
        { path: 'apps', name: 'applications', component: () => import('../pages/account/ApplicationsPage.vue') },
        { path: 'organizations', name: 'organizations', component: () => import('../pages/account/OrganizationsPage.vue') },
      ],
    },
  ],
})

router.beforeEach(async (to, _from, next) => {
  const auth = useAuthStore()

  // Try to restore session on first navigation to protected route. The
  // restore is a cached singleton promise (stores/auth) — awaiting it here
  // joins the store-init restore rather than racing it. The decision uses
  // the CURRENT isAuthenticated, never the promise result: the cached
  // result may predate a logout/token revocation.
  if (to.meta.auth && !auth.isAuthenticated) {
    await auth.restoreSession()
    if (auth.isAuthenticated) {
      next()
      return
    }
    next({ name: 'login', query: { redirect: to.fullPath } })
  } else if (to.meta.guest) {
    // U-1 (browser-e2e 2026-09-08): isAuthenticated starts OPTIMISTICALLY
    // true whenever a refresh_token sits in localStorage. If the token was
    // revoked server-side (e.g. password changed elsewhere), that optimistic
    // flag used to bounce the user off /login into a zombie dashboard of
    // 401s. Await the restore — it clears the flag when the refresh token
    // is dead — then decide on the live value.
    await auth.restoreSession()
    if (auth.isAuthenticated) {
      next({ name: 'dashboard' })
    } else {
      next()
    }
  } else {
    next()
  }
})

export default router
