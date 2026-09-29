import { useDb, saveDb, rid } from '../../utils/db'
import { requireUser, generateApiKey } from '../../utils/auth'

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const body = await readBody<{ name?: string }>(event).catch(() => ({ name: '' }))
  const db = useDb()
  const fresh = db.users.find((u) => u.id === user.id)!
  const key = {
    id: rid('key'),
    name: (body.name ?? '').trim() || `Key ${(fresh.apiKeys ?? []).length + 1}`,
    key: generateApiKey(),
    createdAt: new Date().toISOString(),
  }
  fresh.apiKeys = [...(fresh.apiKeys ?? []), key]
  saveDb(db)
  return key
})
