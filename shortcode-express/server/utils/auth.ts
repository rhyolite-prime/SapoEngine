// ---------------------------------------------------------------------------
// Request auth: signed-in cookie OR Bearer API key (sk_live_…). The API key
// path is what external developers use against the USSD gateway endpoints.
// ---------------------------------------------------------------------------
import { useDb, saveDb } from './db'
import type { User } from '../../shared/types'

export function findUserByApiKey(db: ReturnType<typeof useDb>, key: string): User | null {
  for (const u of db.users) {
    const k = (u.apiKeys ?? []).find((ak) => ak.key === key && !ak.revoked)
    if (k) {
      k.lastUsedAt = new Date().toISOString()
      saveDb(db)
      return u
    }
  }
  return null
}

export function requireUser(event: Parameters<typeof getCookie>[0]): User {
  const db = useDb()
  const auth = getHeader(event, 'authorization')
  if (auth && auth.toLowerCase().startsWith('bearer ')) {
    const user = findUserByApiKey(db, auth.slice(7).trim())
    if (!user) throw createError({ statusCode: 401, statusMessage: 'Invalid API key' })
    return user
  }
  const uid = getCookie(event, 'sce_user')
  const user = db.users.find((u) => u.id === uid)
  if (!user) throw createError({ statusCode: 401, statusMessage: 'Sign in to continue' })
  return user
}

export function generateApiKey(): string {
  const bytes = new Uint8Array(24)
  crypto.getRandomValues(bytes)
  return `sk_live_${Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('')}`
}
