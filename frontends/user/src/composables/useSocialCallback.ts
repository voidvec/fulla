import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useAuthStore } from '../stores/auth'
import { getAccessToken, setTokens, tryRestoreSession } from '../services/http'
import { userService } from '../services/userService'
import { normalizeError, type NormalizedError } from '../services/errorAdapter'
import { getErrorMessage } from '../services/messages'
import { takeSocialCallback, takeSocialFlow, type SocialProvider } from '../services/socialFlowContext'
import { exchangeSocialLogin, SocialFlowError } from '../services/socialLogin'

export function useSocialCallback(provider: SocialProvider) {
  const { t } = useI18n()
  const router = useRouter()
  const auth = useAuthStore()
  const error = ref<NormalizedError | string | null>(null)
  const errorText = computed(() => {
    const value = error.value
    return !value ? '' : typeof value === 'string' ? value : getErrorMessage(value.code)
  })
  onMounted(async () => {
    try {
      const callback = takeSocialCallback()
      const states = callback.searchParams.getAll('state')
      const codes = callback.searchParams.getAll('code')
      if (states.length !== 1 || codes.length > 1) throw new SocialFlowError('invalid')
      const state = states[0]!
      const flow = takeSocialFlow(state, provider, callback.href)
      if (!flow) throw new SocialFlowError('invalid')
      if (callback.searchParams.has('error')) throw new SocialFlowError('denied')
      const code = codes[0]
      if (!code) throw new SocialFlowError('invalid')
      if (flow.purpose === 'link') {
        if (!getAccessToken() && !await tryRestoreSession()) {
          error.value = t('oauth.linkSignInFirst', { provider })
          return
        }
        await userService.linkSocialAccount(provider, code, state)
        await router.replace('/security')
        return
      }
      const tokens = await exchangeSocialLogin(provider, code, state)
      if (!tokens.access_token) throw new SocialFlowError('invalid')
      setTokens(tokens.access_token, tokens.refresh_token)
      auth.markAuthenticated()
      await auth.fetchUser()
      await router.replace('/')
    } catch (failure) {
      error.value = failure instanceof SocialFlowError
        ? t(failure.reason === 'cookies' ? 'oauth.cookiesRequired' :
            failure.reason === 'denied' ? 'oauth.providerDenied' : 'oauth.invalidFlow')
        : normalizeError(failure)
    }
  })
  return { errorText }
}
