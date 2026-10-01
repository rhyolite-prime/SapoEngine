import type { BaseApiResponse, BusinessAuthModel } from '~/models'
import type { User } from '~/../shared/types'

/**
 * Auth spans two worlds:
 *
 *  1. The Rhyolite Prime ERP (ABP) — the source of truth for identity.
 *     A successful sign-in/sign-up stores the ERP tokens in the
 *     `rhyolite-identity` cookie (permissions go to IndexedDB) so
 *     httpClient can Bearer-authenticate direct ERP service calls.
 *  2. The local workspace session (`sce_user` cookie) — what the app's own
 *     APIs (flows, short codes, billing, the USSD gateway) authenticate with.
 *
 * The ERP-backed login/signup endpoints set both; signOut clears both.
 */
export const useAuth = () => {
  const router = useRouter()

  const businessIdentityCookie = useCookie('rhyolite-identity', {
    maxAge: 60 * 60 * 24,
  })

  const config = useRuntimeConfig()

  // --- local workspace session ------------------------------------------------
  const user = useState<User | null>('sce-user', () => null)
  const sessionCookie = useCookie('sce_user')

  async function refresh() {
    if (!sessionCookie.value) { user.value = null; return }
    user.value = await $fetch<User>('/api/auth/me')
  }

  /** demo/offline member picker — no ERP round-trip */
  async function login(email: string) {
    user.value = await $fetch<User>('/api/auth/login', { method: 'POST', body: { email } })
  }

  // --- ERP identity persistence (cookie + IndexedDB permissions) ---------------
  const savePermissionsToIndexedDB = (permissions: string[]): Promise<void> => {
    return new Promise((resolve, reject) => {
      if (typeof window === 'undefined') {
        resolve()
        return
      }
      const request = indexedDB.open('ShortCodeExpressAuthDB', 1)
      request.onupgradeneeded = (event: any) => {
        const db = event.target.result
        if (!db.objectStoreNames.contains('permissions')) {
          db.createObjectStore('permissions')
        }
      }
      request.onsuccess = (event: any) => {
        const db = event.target.result
        const transaction = db.transaction('permissions', 'readwrite')
        const store = transaction.objectStore('permissions')
        store.put(permissions, 'user_permissions')
        transaction.oncomplete = () => resolve()
        transaction.onerror = (e) => reject(e)
      }
      request.onerror = (event) => reject(event)
    })
  }

  const clearPermissionsFromIndexedDB = (): Promise<void> => {
    return new Promise((resolve, reject) => {
      if (typeof window === 'undefined') {
        resolve()
        return
      }
      const request = indexedDB.open('ShortCodeExpressAuthDB', 1)
      request.onsuccess = (event: any) => {
        const db = event.target.result
        if (!db.objectStoreNames.contains('permissions')) {
          resolve()
          return
        }
        const transaction = db.transaction('permissions', 'readwrite')
        const store = transaction.objectStore('permissions')
        store.delete('user_permissions')
        transaction.oncomplete = () => resolve()
        transaction.onerror = (e) => reject(e)
      }
      request.onerror = (event) => reject(event)
    })
  }

  /** Persist an ERP-issued identity (called after a successful ERP sign-in or sign-up) */
  const saveBusinessIdentity = async (model: BusinessAuthModel) => {
    const permissions = model.permissions || []
    await savePermissionsToIndexedDB(permissions)
    businessIdentityCookie.value = JSON.stringify({ ...model, permissions: [] })
  }

  // --- direct ERP sign-in (browser → /erp proxy → ERP) ---------------------------
  const getBusinessAuth = async (payload: Object) => {
    const response = await $fetch<BaseApiResponse<BusinessAuthModel>>(
      'tokenauth/authenticate',
      {
        method: 'POST',
        body: payload,
        baseURL: config.public.proxyApiAuthBaseURL,
      },
    )

    if (response.success && response.result) {
      let today = new Date()

      response.result.expiresOn = today
        .setDate(today.getDate() + 1)
        .toString()

      await saveBusinessIdentity(response.result)

      await router.push('/')
    }
    return response
  }

  // --- sign out: clear the ERP identity AND the local workspace session -----------
  const signOut = async () => {
    businessIdentityCookie.value = null
    await clearPermissionsFromIndexedDB().catch(() => {})
    try { await $fetch('/api/auth/logout', { method: 'POST' }) } catch {}
    sessionCookie.value = null
    user.value = null
    window.location.href = '/login'
  }

  return {
    // ERP identity
    getBusinessAuth,
    saveBusinessIdentity,
    // local workspace session
    user,
    refresh,
    login,
    logout: signOut,
    signOut,
  }
}
