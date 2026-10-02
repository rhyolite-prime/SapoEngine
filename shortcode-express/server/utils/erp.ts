// ---------------------------------------------------------------------------
// Rhyolite Prime ERP (ABP) identity client. Signup + sign-in are proxied
// through the app's server so the ERP base URL stays configurable and the
// browser never fights CORS. All requests time out quickly so a slow ERP
// degrades into a clear 503 instead of hanging the checkout of a login.
// ---------------------------------------------------------------------------
export const ERP_BASE = (process.env.ERP_API_BASE ?? 'https://erp-api.rhyoliteprime.com').replace(/\/$/, '')

export const ERP_PATHS = {
  signup: '/api/services/app/Tenant/PrivateCreateSignup',
  login: '/api/tokenauth/authenticate',
  changePassword: '/api/services/app/Account/ChangePassword',
}

/** ABP wraps everything: { result, success, error: { message }, __abp } */
export interface ErpAuthResult {
  accessToken?: string | null
  encryptedAccessToken?: string | null
  expireInSeconds?: number
  userId?: number
  permissions?: string[]
  requiresTwoFactorAuthenticaton?: boolean // ABP Zero's historic spelling
  requiresTwoFactorAuthentication?: boolean
  [k: string]: unknown
}

export interface ErpResponse {
  ok: boolean
  /** network-level failure (ERP unreachable) — map to 503 */
  unreachable: boolean
  status: number
  result?: ErpAuthResult
  error?: string
}

export async function erpRequest(path: string, body: Record<string, unknown>, bearerToken?: string): Promise<ErpResponse> {
  let res: Response
  try {
    res = await fetch(ERP_BASE + path, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        ...(bearerToken ? { authorization: `Bearer ${bearerToken}` } : {}),
      },
      body: JSON.stringify(body),
      signal: AbortSignal.timeout(10_000),
    })
  } catch {
    return { ok: false, unreachable: true, status: 0, error: 'ERP unreachable' }
  }

  let payload: { result?: ErpAuthResult; success?: boolean; error?: { message?: string; details?: string } | null } = {}
  try { payload = await res.json() } catch { /* non-JSON error page */ }

  const errMessage = payload.error?.message
    ?? (typeof payload.error === 'object' && payload.error && 'details' in (payload.error as object) ? undefined : undefined)
    ?? (res.ok ? undefined : `ERP error (HTTP ${res.status})`)

  const result = payload.result
  const twoFactorRequired =
    !!result?.requiresTwoFactorAuthenticaton || !!result?.requiresTwoFactorAuthentication

  return {
    ok: res.ok && payload.success !== false && !twoFactorRequired && !!result?.accessToken,
    unreachable: false,
    status: res.status,
    result,
    error: errMessage,
  }
}

/** Does an ABP error message mean "we need your authenticator code"? */
export function mentionsTwoFactor(message?: string): boolean {
  if (!message) return false
  const m = message.toLowerCase()
  return m.includes('two factor') || m.includes('two-factor') || m.includes('verification code') || m.includes('authenticator')
}

/** Stable local identity for an ERP user that has never signed in here before */
export function syntheticEmail(userName: string, tenant: string): string {
  const u = userName.trim().toLowerCase().replace(/[^a-z0-9@._-]/g, '') || 'user'
  if (/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(u)) return u
  const t = tenant.trim().toLowerCase().replace(/[^a-z0-9]/g, '') || 'erp'
  return `${u}@${t}.erp.local`
}
