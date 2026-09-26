<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import http from '../../services/http'
import { userService } from '../../services/userService'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import AppButton from '../../components/ui/AppButton.vue'
import { getErrorMessage } from '../../services/messages'
import { useAuthStore } from '../../stores/auth'
import { base64UrlEncode, base64UrlDecode } from '../../utils/pkce'
import type { SocialLink } from '../../types'
import AppAlert from '../../components/ui/AppAlert.vue'
import AppBadge from '../../components/ui/AppBadge.vue'
import AppCard from '../../components/ui/AppCard.vue'
import AppModal from '../../components/ui/AppModal.vue'
import AppConfirmDialog from '../../components/ui/AppConfirmDialog.vue'
import DData from '../../components/ui/DData.vue'
// U-5: renders the backend-provided otpauth:// URI as a scannable QR code
// (the setup panel's copy promised a QR but only showed the manual key).
import QrcodeVue from 'qrcode.vue'

const { t } = useI18n()
const auth = useAuthStore()
const router = useRouter()
const loading = ref(true)
const profile = ref<any>(null)
const success = ref('')
// #158: catalog-backed errors are stored as NormalizedError and resolved at
// render time (errorText) so switching locale re-translates text on screen;
// plain strings (chrome copy via t()) keep snapshot semantics.
const error = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = error.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

// Password change
const oldPassword = ref('')
const newPassword = ref('')
const confirmNewPassword = ref('')
const changingPassword = ref(false)

// MFA
const mfaSetupData = ref<any>(null)
const mfaVerifyCode = ref('')
const settingUpMfa = ref(false)
const disablingMfa = ref(false)
const disablePassword = ref('')
// One-time backup codes shown exactly once after MFA setup verification.
const backupCodes = ref<string[]>([])
const showBackupCodes = ref(false)

// Connected social accounts (B2 link/unlink)
const socialLinks = ref<SocialLink[]>([])
const socialLinksLoaded = ref(false)
const unlinkingProvider = ref('')
const linkingProvider = ref('')
const providerLabels: Record<string, string> = { github: 'GitHub', google: 'Google', wechat: 'WeChat' }

// #181: destructive actions route through the shared confirm dialog instead
// of native confirm() (which needed page.on('dialog') shims in the e2e suite).
const confirmOpen = ref(false)
const confirmMessage = ref('')
const confirmAction = ref<(() => Promise<void>) | null>(null)
function askConfirm(message: string, action: () => Promise<void>) {
  confirmMessage.value = message
  confirmAction.value = action
  confirmOpen.value = true
}
async function runConfirm() {
  confirmOpen.value = false
  const action = confirmAction.value
  confirmAction.value = null
  if (action) await action()
}

// #71: the link entry point goes through the server, which mints a one-time
// state bound to (user, provider) and returns the full authorize URL (the
// VITE_GITHUB_CLIENT_ID env dependency is gone).
async function beginSocialLink(provider: string) {
  if (linkingProvider.value) return
  linkingProvider.value = provider
  try {
    const { authorize_url: authorizeUrl } = await userService.beginSocialLink(provider)
    // Flow marker for the callback page (survives the full-page round trip
    // within this tab); the non-empty state in the callback is the backstop.
    sessionStorage.setItem('social_link_flow', provider)
    window.location.href = authorizeUrl
  } catch (e: unknown) {
    // Show the failure inline instead of leaving the card silently inert.
    window.alert(normalizeError(e).message)
  } finally {
    linkingProvider.value = ''
  }
}

async function fetchSocialLinks() {
  try {
    socialLinks.value = await userService.getSocialLinks()
  } catch {
    // The card shows its own empty state; a backend hiccup here must not
    // break the rest of the security page.
    socialLinks.value = []
  } finally {
    socialLinksLoaded.value = true
  }
}

function unlinkSocial(provider: string) {
  const label = providerLabels[provider] || provider
  // W4: after unlinking, sign-in with this identity fails until it is
  // linked to an account again -- say so up front, not after the fact.
  askConfirm(t('account.security.social.unlinkConfirm', { provider: label }), async () => {
    unlinkingProvider.value = provider
    try {
      await userService.unlinkSocialAccount(provider)
      showSuccess(t('account.security.social.unlinked', { provider: label }))
      await fetchSocialLinks()
    } catch (e: unknown) {
      showError(normalizeError(e))
    } finally {
      unlinkingProvider.value = ''
    }
  })
}

async function fetchProfile() {
  try {
    const resp = await http.get('/api/me')
    profile.value = resp.data
    if (webauthnSupported) fetchWebauthnCredentials()
  } catch {} finally { loading.value = false }
  fetchSocialLinks()
}

function showSuccess(msg: string) { success.value = msg; error.value = null; setTimeout(() => { success.value = '' }, 4000) }
function showError(msg: NormalizedError | string) { error.value = msg; success.value = '' }

async function changePassword() {
  if (newPassword.value !== confirmNewPassword.value) { showError(t('common.passwordsDoNotMatch')); return }
  if (newPassword.value.length < 8) { showError(t('common.passwordMinLength')); return }
  changingPassword.value = true
  try {
    await http.put('/api/me/password', { old_password: oldPassword.value, new_password: newPassword.value }, { headers: { 'Content-Type': 'application/json' } })
    // U-1: the server revoked every token for this account as part of the
    // change (response note: "All existing sessions have been revoked").
    // Drop the local session and land on /login with a notice — keeping the
    // stale token left the SPA in a zombie state (guest guard bounced the
    // user off /login into a broken dashboard full of 401s). SPA navigation
    // rather than a full page load (PR #180 review M9): logoutLocal() has
    // already cleared the state, and router keeps the base prefix.
    auth.logoutLocal()
    oldPassword.value = ''; newPassword.value = ''; confirmNewPassword.value = ''
    await router.replace({ path: '/login', query: { pw_changed: '1' } })
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally { changingPassword.value = false }
}

async function setupMfa() {
  settingUpMfa.value = true
  try {
    const resp = await http.post('/api/me/mfa/setup')
    mfaSetupData.value = resp.data
  } catch (e: unknown) {
    showError(normalizeError(e))
    settingUpMfa.value = false
  }
}

async function verifyMfaSetup() {
  try {
    const resp = await http.post('/api/me/mfa/verify', new URLSearchParams({ code: mfaVerifyCode.value }))
    // One-shot recovery codes: the backend returns 10 single-use backup codes
    // with an explicit "cannot be shown again" warning (MfaController). The
    // old code discarded them, leaving no self-service recovery path. Hold
    // the confirmation layer open until the user acknowledges saving.
    const codes: string[] = resp.data?.backup_codes || []
    mfaSetupData.value = null
    settingUpMfa.value = false
    mfaVerifyCode.value = ''
    if (codes.length > 0) {
      backupCodes.value = codes
      showBackupCodes.value = true
    } else {
      showSuccess(t('account.security.mfa.enabledSuccess'))
    }
    await fetchProfile()
  } catch (e: unknown) {
    showError(normalizeError(e))
  }
}

function dismissBackupCodes() {
  showBackupCodes.value = false
  backupCodes.value = []
  showSuccess(t('account.security.mfa.enabledSuccess'))
}

async function copyBackupCodes() {
  try {
    await navigator.clipboard.writeText(backupCodes.value.join('\n'))
  } catch {
    // Clipboard API unavailable (permissions/insecure context); the download
    // button remains as the fallback path.
  }
}

function downloadBackupCodes() {
  const blob = new Blob([backupCodes.value.join('\n')], { type: 'text/plain' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = 'fulla-backup-codes.txt'
  a.click()
  URL.revokeObjectURL(url)
}

async function disableMfa() {
  if (!disablePassword.value) { showError(t('account.security.mfa.passwordRequired')); return }
  disablingMfa.value = true
  try {
    await http.post('/api/me/mfa/disable', new URLSearchParams({ password: disablePassword.value }))
    showSuccess(t('account.security.mfa.disabled'))
    disablePassword.value = ''
    await fetchProfile()
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally { disablingMfa.value = false }
}

// Account deletion
const deleteConfirmUsername = ref('')
const deletingAccount = ref(false)

// WebAuthn / Passkeys
const webauthnCredentials = ref<any[]>([])
const registeringPasskey = ref(false)
const passkeyName = ref('')

async function fetchWebauthnCredentials() {
  try {
    const resp = await http.get('/api/me/webauthn/credentials')
    webauthnCredentials.value = resp.data.credentials || resp.data || []
  } catch {}
}

async function registerPasskey() {
  registeringPasskey.value = true
  try {
    // Step 1: Get creation options from the server. The payload nests the
    // PublicKeyCredentialCreationOptions under `options`, and the challenge
    // (and user.id) are base64url strings that must be decoded to bytes
    // before the browser API accepts them (atob throws on the -_ alphabet).
    const beginResp = await http.post('/api/me/webauthn/register/begin')
    const options = beginResp.data?.options
    if (!options?.challenge) throw new Error('Passkey registration: invalid server options')

    // Step 2: Call the browser WebAuthn API (#142: pass the SERVER's
    // rp/pubKeyCredParams/authenticatorSelection verbatim — the server now
    // verifies against exactly what it advertised: ES256-only,
    // userVerification=required, and excludeCredentials for already-
    // registered passkeys).
    const credential = await navigator.credentials.create({
      publicKey: {
        challenge: base64UrlDecode(options.challenge),
        rp: options.rp || { name: 'Fulla', id: window.location.hostname },
        user: {
          id: base64UrlDecode(options.user?.id || base64UrlEncode(new TextEncoder().encode(profile.value?.username || 'user'))),
          name: options.user?.name || profile.value?.username || 'user',
          displayName: options.user?.displayName || profile.value?.username || 'User',
        },
        pubKeyCredParams: options.pubKeyCredParams || [{ alg: -7, type: 'public-key' }],
        authenticatorSelection: options.authenticatorSelection || { userVerification: 'required' },
        timeout: options.timeout || 60000,
        excludeCredentials: (options.excludeCredentials || []).map((e: { id: string; type?: string }) => ({
          id: base64UrlDecode(e.id),
          type: e.type || 'public-key',
        })),
      }
    }) as PublicKeyCredential

    if (!credential) { showError(t('account.security.passkeys.cancelled')); return }

    // Step 3: Send the credential to the server in ITS contract shape
    // (#142): the browser attestation envelope {id, rawId, response:
    // {attestationObject, clientDataJSON}} — the server verifies the
    // attestation itself; a client-side public_key field would be trusted
    // material and is no longer part of the contract.
    const attestationResponse = credential.response as AuthenticatorAttestationResponse
    const label = passkeyName.value.trim() || t('account.security.passkeys.defaultName', { date: new Date().toISOString().slice(0, 10) })
    await http.post('/api/me/webauthn/register/finish', {
      id: base64UrlEncode(new Uint8Array(credential.rawId)),
      rawId: base64UrlEncode(new Uint8Array(credential.rawId)),
      response: {
        attestationObject: base64UrlEncode(new Uint8Array(attestationResponse.attestationObject)),
        clientDataJSON: base64UrlEncode(new Uint8Array(attestationResponse.clientDataJSON)),
      },
      name: label,
    }, { headers: { 'Content-Type': 'application/json' } })

    passkeyName.value = ''
    showSuccess(t('account.security.passkeys.success'))
    await fetchWebauthnCredentials()
  } catch (e: any) {
    if (e.name === 'NotAllowedError') {
      showError(t('account.security.passkeys.timedOut'))
    } else if (e.name === 'InvalidStateError') {
      // excludeCredentials hit: this browser already holds a passkey for
      // the account.
      showError(t('account.security.passkeys.alreadyRegistered'))
    } else if (!e.response) {
      // M-3: WebAuthn DOMExceptions and local throws carry no axios
      // response; normalizeError would misreport them all as "network
      // connection failed".
      showError(t('account.security.passkeys.registrationFailed'))
    } else {
      showError(normalizeError(e))
    }
  } finally { registeringPasskey.value = false }
}

const webauthnSupported = typeof window !== 'undefined' && !!window.PublicKeyCredential

async function deleteAccount() {
  // U-2: the empty-string check is load-bearing. A blank-username account
  // (or any future state where profile.username === '') would otherwise
  // pass '' === '' with zero typing — the exact one-click self-deletion
  // path found in browser-e2e. The backend now generates usernames on
  // registration, but this guard protects any pre-backfill/edge state.
  if (!deleteConfirmUsername.value || deleteConfirmUsername.value !== profile.value?.username) {
    showError(t('account.security.danger.usernameMismatch'))
    return
  }
  deletingAccount.value = true
  try {
    await http.delete('/api/me')
    // Clear session and redirect to login
    localStorage.clear()
    window.location.href = '/login'
  } catch (e: unknown) {
    showError(normalizeError(e))
  } finally { deletingAccount.value = false }
}

onMounted(fetchProfile)
</script>

<template>
  <div>
    <h1 class="text-2xl font-bold text-neutral-900 mb-6">
      {{ $t('account.security.title') }}
    </h1>

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

    <div
      v-if="loading"
      class="text-center py-12 text-neutral-500"
    >
      {{ $t('common.loading') }}
    </div>

    <div
      v-else
      class="space-y-6"
    >
      <!-- Change Password -->
      <AppCard>
        <h2 class="text-lg font-semibold text-neutral-900 mb-4">
          {{ $t('account.security.changePassword') }}
        </h2>
        <form
          class="space-y-4 max-w-md"
          @submit.prevent="changePassword"
        >
          <div>
            <label
              class="block text-sm font-medium text-neutral-700 mb-1"
              for="current-password-input"
            >{{ $t('account.security.currentPassword') }}</label>
            <input
              id="current-password-input"
              v-model="oldPassword"
              type="password"
              required
              autocomplete="current-password"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-ctl text-sm focus:ring-2 focus:ring-brand-500"
            >
          </div>
          <div>
            <label
              class="block text-sm font-medium text-neutral-700 mb-1"
              for="new-password-input"
            >{{ $t('common.newPassword') }}</label>
            <input
              id="new-password-input"
              v-model="newPassword"
              type="password"
              required
              autocomplete="new-password"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-ctl text-sm focus:ring-2 focus:ring-brand-500"
            >
          </div>
          <div>
            <label
              class="block text-sm font-medium text-neutral-700 mb-1"
              for="confirm-password-input"
            >{{ $t('common.confirmNewPassword') }}</label>
            <input
              id="confirm-password-input"
              v-model="confirmNewPassword"
              type="password"
              required
              autocomplete="new-password"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-ctl text-sm focus:ring-2 focus:ring-brand-500"
            >
          </div>
          <button
            type="submit"
            :disabled="changingPassword"
            class="px-4 py-2 bg-brand-600 text-white rounded-ctl text-sm hover:bg-brand-700 disabled:opacity-50"
          >
            {{ changingPassword ? $t('account.security.changing') : $t('account.security.changePassword') }}
          </button>
        </form>
      </AppCard>

      <!-- MFA -->
      <AppCard>
        <h2 class="text-lg font-semibold text-neutral-900 mb-4">
          {{ $t('account.security.mfa.title') }}
        </h2>

        <div
          v-if="profile?.mfa_enabled"
          class="space-y-4"
        >
          <div class="flex items-center gap-2">
            <AppBadge
              variant="success"
              size="sm"
            >{{ $t('account.security.mfa.enabled') }}</AppBadge>
            <p class="text-sm text-neutral-600">
              {{ $t('account.security.mfa.protected') }}
            </p>
          </div>
          <div class="border-t pt-4">
            <p class="text-sm text-neutral-600 mb-2">
              {{ $t('account.security.mfa.disablePrompt') }}
            </p>
            <div class="flex gap-2 max-w-md">
              <input
                id="mfa-disable-password-input"
                v-model="disablePassword"
                type="password"
                :placeholder="$t('account.security.mfa.disablePlaceholder')"
                :aria-label="$t('common.password')"
                class="flex-1 px-3 py-2 border border-neutral-300 rounded-ctl text-sm"
              >
              <button
                :disabled="disablingMfa"
                class="px-4 py-2 bg-error-600 text-white rounded-ctl text-sm hover:bg-error-700 disabled:opacity-50"
                @click="disableMfa"
              >
                {{ disablingMfa ? $t('account.security.mfa.disabling') : $t('account.security.mfa.disable') }}
              </button>
            </div>
          </div>
        </div>

        <div
          v-else-if="mfaSetupData"
          class="space-y-4"
        >
          <p class="text-sm text-neutral-600">
            {{ $t('account.security.mfa.scanQr') }}
          </p>
          <!-- U-5: QR of the backend-provided otpauth:// URI (MfaController
               returns otpauth_uri alongside the secret). -->
          <div
            v-if="mfaSetupData.otpauth_uri"
            class="bg-surface p-4 rounded-ctl border border-neutral-200 inline-block mx-auto"
            data-testid="mfa-setup-qr"
          >
            <QrcodeVue
              :value="mfaSetupData.otpauth_uri"
              :size="180"
              level="M"
            />
          </div>
          <div class="bg-neutral-50 p-4 rounded-ctl text-center">
            <p class="text-xs text-neutral-500 mb-2">
              {{ $t('account.security.mfa.manualKey') }}
            </p>
            <code class="text-sm font-mono bg-surface px-3 py-1 rounded border select-all">{{ mfaSetupData.secret }}</code>
          </div>
          <div class="max-w-xs">
            <label
              class="block text-sm font-medium text-neutral-700 mb-1"
              for="mfa-verify-code-input"
            >{{ $t('account.security.mfa.verificationCode') }}</label>
            <input
              id="mfa-verify-code-input"
              v-model="mfaVerifyCode"
              type="text"
              inputmode="numeric"
              maxlength="6"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-ctl text-sm text-center font-mono tabular-nums tracking-[0.42em]
                     focus:outline-none focus-visible:ring-[3px] focus-visible:ring-ring focus:border-brand-700"
              placeholder="000000"
            >
          </div>
          <div class="flex gap-2">
            <button
              :disabled="mfaVerifyCode.length !== 6"
              class="px-4 py-2 bg-brand-600 text-white rounded-ctl text-sm hover:bg-brand-700 disabled:opacity-50"
              @click="verifyMfaSetup"
            >
              {{ $t('account.security.mfa.verifyEnable') }}
            </button>
            <button
              class="px-4 py-2 border border-neutral-300 rounded-ctl text-sm hover:bg-neutral-50"
              @click="mfaSetupData = null; settingUpMfa = false"
            >
              {{ $t('common.cancel') }}
            </button>
          </div>
        </div>

        <div v-else>
          <p class="text-sm text-neutral-600 mb-4">
            {{ $t('account.security.mfa.totpIntro') }}
          </p>
          <button
            :disabled="settingUpMfa"
            class="px-4 py-2 bg-brand-600 text-white rounded-ctl text-sm hover:bg-brand-700 disabled:opacity-50"
            @click="setupMfa"
          >
            {{ settingUpMfa ? $t('account.security.mfa.settingUp') : $t('account.security.mfa.enable') }}
          </button>
        </div>
      </AppCard>

      <!-- WebAuthn / Passkeys -->
      <AppCard v-if="webauthnSupported">
        <h2 class="text-lg font-semibold text-neutral-900 mb-4">
          {{ $t('account.security.passkeys.title') }}
        </h2>
        <p class="text-sm text-neutral-600 mb-4">
          {{ $t('account.security.passkeys.intro') }}
        </p>

        <!-- Registered credentials -->
        <div
          v-if="webauthnCredentials.length > 0"
          class="space-y-2 mb-4"
        >
          <div
            v-for="cred in webauthnCredentials"
            :key="cred.credential_id"
            class="flex items-center justify-between p-3 bg-neutral-50 rounded-ctl"
          >
            <div>
              <p class="text-sm font-medium text-neutral-900">
                {{ cred.name || $t('account.security.passkeys.fallbackName') }}
              </p>
              <p class="text-xs text-neutral-500">
                {{ $t('account.security.passkeys.signCounter', { count: cred.sign_count ?? 0 }) }}
              </p>
            </div>
            <AppBadge
              variant="success"
              size="sm"
            >{{ $t('account.security.passkeys.active') }}</AppBadge>
          </div>
        </div>
        <div
          v-else
          class="mb-4 p-3 bg-neutral-50 rounded-ctl text-sm text-neutral-500"
        >
          {{ $t('account.security.passkeys.empty') }}
        </div>

        <div class="flex flex-col sm:flex-row gap-2">
          <input
            id="passkey-name-input"
            v-model="passkeyName"
            type="text"
            :placeholder="$t('account.security.passkeys.namePlaceholder')"
            :aria-label="$t('account.security.passkeys.namePlaceholder')"
            class="flex-1 px-3 py-2 text-sm rounded-ctl border border-neutral-300 focus:outline-none focus:ring-2 focus:ring-brand-500/20 focus:border-brand-500"
          >
          <button
            :disabled="registeringPasskey"
            class="px-4 py-2 bg-brand-600 text-white rounded-ctl text-sm hover:bg-brand-700 disabled:opacity-50 whitespace-nowrap"
            @click="registerPasskey"
          >
            {{ registeringPasskey ? $t('account.security.passkeys.registering') : $t('account.security.passkeys.add') }}
          </button>
        </div>
      </AppCard>

      <!-- Connected social accounts (B2 link/unlink) -->
      <AppCard>
        <h2 class="text-lg font-semibold text-neutral-900 mb-4">
          {{ $t('account.security.social.title') }}
        </h2>
        <p class="text-sm text-neutral-600 mb-4">
          {{ $t('account.security.social.intro') }}
        </p>

        <div
          v-if="socialLinksLoaded && socialLinks.length > 0"
          class="space-y-2 mb-4"
        >
          <div
            v-for="link in socialLinks"
            :key="link.provider"
            class="flex items-center justify-between p-3 bg-neutral-50 rounded-ctl"
          >
            <div>
              <p class="text-sm font-medium text-neutral-900">
                {{ providerLabels[link.provider] || link.provider }}
              </p>
              <p class="text-xs text-neutral-500">
                {{ link.linked_at ? $t('account.security.social.linkedOn', { date: new Date(link.linked_at).toLocaleDateString() }) : '' }}
              </p>
            </div>
            <button
              :disabled="unlinkingProvider === link.provider"
              class="px-3 py-1.5 border border-error-200 text-error-600 rounded-ctl text-sm hover:bg-error-50 disabled:opacity-50"
              @click="unlinkSocial(link.provider)"
            >
              {{ unlinkingProvider === link.provider ? $t('account.security.social.unlinking') : $t('account.security.social.unlink') }}
            </button>
          </div>
        </div>
        <div
          v-else-if="socialLinksLoaded"
          class="mb-4 p-3 bg-neutral-50 rounded-ctl text-sm text-neutral-500"
        >
          {{ $t('account.security.social.empty') }}
        </div>

        <AppButton
          v-if="!unlinkingProvider"
          :disabled="linkingProvider !== ''"
          @click="beginSocialLink('github')"
        >
          {{ linkingProvider === 'github' ? $t('account.security.social.redirecting') : $t('account.security.social.linkGithub') }}
        </AppButton>

        <AppButton
          v-if="!unlinkingProvider"
          class="mt-2"
          :disabled="linkingProvider !== ''"
          @click="beginSocialLink('google')"
        >
          {{ linkingProvider === 'google' ? $t('account.security.social.redirecting') : $t('account.security.social.linkGoogle') }}
        </AppButton>
      </AppCard>

      <!-- Delete Account -->
      <div class="bg-surface rounded-card shadow-sm border border-error-200 p-6">
        <h2 class="text-lg font-semibold text-error-700 mb-2">
          {{ $t('account.security.danger.title') }}
        </h2>
        <p class="text-sm text-neutral-600 mb-4">
          {{ $t('account.security.danger.intro') }}
        </p>
        <div class="space-y-3 max-w-md">
          <div>
            <label
              class="block text-sm font-medium text-neutral-700 mb-1"
              for="delete-confirm-username"
            >
              {{ $t('account.security.danger.typePrefix') }} <strong>{{ profile?.username }}</strong> {{ $t('account.security.danger.typeSuffix') }}
            </label>
            <input
              id="delete-confirm-username"
              v-model="deleteConfirmUsername"
              type="text"
              autocomplete="off"
              class="block w-full px-3 py-2 border border-neutral-300 rounded-ctl text-sm focus:ring-2 focus:ring-error-500 focus:border-error-500"
              :placeholder="profile?.username"
            >
          </div>
          <button
            :disabled="deletingAccount || !deleteConfirmUsername || deleteConfirmUsername !== profile?.username"
            class="px-4 py-2 bg-error-600 text-white rounded-ctl text-sm hover:bg-error-700 disabled:opacity-50 disabled:cursor-not-allowed"
            @click="deleteAccount"
          >
            {{ deletingAccount ? $t('account.security.danger.deleting') : $t('account.security.danger.delete') }}
          </button>
        </div>
      </div>

      <!-- One-time MFA backup codes (gap-fix P0): the backend returns the 10
           single-use recovery codes exactly once and never again. The modal
           blocks until the user explicitly confirms saving them — AppModal's
           close (Esc/backdrop) is deliberately not wired for that reason. -->
      <AppModal
        :open="showBackupCodes"
        :aria-label="$t('account.security.backup.ariaLabel')"
        size="sm"
      >
        <h2 class="text-lg font-semibold text-neutral-900 mb-2">
          {{ $t('account.security.backup.title') }}
        </h2>
        <p class="text-sm text-error-600 font-medium mb-4">
          {{ $t('account.security.backup.warning') }}
        </p>
        <div class="grid grid-cols-2 gap-2 mb-4">
          <DData
            v-for="code in backupCodes"
            :key="code"
            :value="code"
            class="justify-center"
            data-testid="backup-code"
          />
        </div>
        <div class="flex gap-2 mb-4">
          <button
            type="button"
            class="flex-1 px-3 py-2 border border-neutral-300 rounded-ctl text-sm hover:bg-neutral-50
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            @click="copyBackupCodes"
          >
            {{ $t('account.security.backup.copyAll') }}
          </button>
          <button
            type="button"
            class="flex-1 px-3 py-2 border border-neutral-300 rounded-ctl text-sm hover:bg-neutral-50
                   focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
            @click="downloadBackupCodes"
          >
            {{ $t('account.security.backup.download') }}
          </button>
        </div>
        <button
          type="button"
          class="w-full px-4 py-2 bg-brand-600 text-white rounded-ctl text-sm hover:bg-brand-700
                 focus-visible:outline-none focus-visible:ring-[3px] focus-visible:ring-ring"
          @click="dismissBackupCodes"
        >
          {{ $t('account.security.backup.saved') }}
        </button>
      </AppModal>

      <AppConfirmDialog
        :open="confirmOpen"
        :message="confirmMessage"
        danger
        @confirm="runConfirm"
        @cancel="confirmOpen = false"
      />
    </div>
  </div>
</template>

