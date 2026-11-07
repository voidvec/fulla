import axios from 'axios'
import { rememberSocialFlow, type SocialProvider } from './socialFlowContext'

// No bearer interceptor, automatic refresh or retry on these one-time POSTs.
const socialHttp = axios.create({
  baseURL: import.meta.env.VITE_API_BASE_URL || '',
  timeout: 15000,
  withCredentials: true,
  headers: { 'Content-Type': 'application/json' },
})

export class SocialFlowError extends Error {
  constructor(public reason: 'invalid' | 'cookies' | 'denied') {
    super('Social transaction could not be completed')
  }
}

export async function beginSocialLogin(provider: SocialProvider): Promise<string> {
  const path = `/api/auth/providers/${provider}/authorize`
  let response = await socialHttp.post(path, {})
  if (response.status === 204) response = await socialHttp.post(path, {})
  if (response.status === 204) throw new SocialFlowError('cookies')
  const { state, authorize_url: authorizeUrl, redirect_uri: redirectUri } = response.data
  if (response.status !== 200 || typeof state !== 'string' || typeof authorizeUrl !== 'string' ||
      typeof redirectUri !== 'string' || new URL(authorizeUrl).protocol !== 'https:') {
    throw new SocialFlowError('invalid')
  }
  rememberSocialFlow(state, { purpose: 'login', provider, redirectUri })
  return authorizeUrl
}

export async function exchangeSocialLogin(provider: SocialProvider, code: string, state: string) {
  const response = await socialHttp.post<{ access_token: string; refresh_token?: string }>(
    `/api/${provider}/login`, { code, state })
  return response.data
}
