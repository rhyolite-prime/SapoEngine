import type { BusinessAuthModel } from '~/models'

/**
 * The ERP-issued business identity, persisted by useAuth in the
 * `rhyolite-identity` cookie (permissions live in IndexedDB, not the cookie).
 * httpClient reads the access token from here to Bearer-authenticate
 * direct ERP service calls.
 */
export const useBusinessAuthIdentity = () => {
  const cookie = useCookie('rhyolite-identity', { maxAge: 60 * 60 * 24 })
  return computed<BusinessAuthModel | null>(() => {
    if (!cookie.value) return null
    try {
      return typeof cookie.value === 'string' ? (JSON.parse(cookie.value) as BusinessAuthModel) : (cookie.value as BusinessAuthModel)
    } catch {
      return null
    }
  })
}
