import { useDb, saveDb } from '../../utils/db'
import { requireUser } from '../../utils/auth'

export default defineEventHandler((event) => {
  const user = requireUser(event)
  const id = getRouterParam(event, 'id')
  const db = useDb()
  const fresh = db.users.find((u) => u.id === user.id)!
  const key = (fresh.apiKeys ?? []).find((k) => k.id === id && !k.revoked)
  if (!key) throw createError({ statusCode: 404, statusMessage: 'Key not found' })
  key.revoked = true
  saveDb(db)
  return { ok: true }
})
