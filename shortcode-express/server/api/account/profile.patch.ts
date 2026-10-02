// Update the local workspace profile (display fields only — the email and
// password live on the Rhyolite Prime ERP account).
import { useDb, saveDb } from '../../utils/db'
import { requireUser } from '../../utils/auth'

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const body = await readBody<{ name?: string; title?: string; company?: string }>(event)
  const db = useDb()
  const fresh = db.users.find((u) => u.id === user.id)
  if (!fresh) throw createError({ statusCode: 404, statusMessage: 'Account not found' })

  if (body.name !== undefined) {
    const name = body.name.trim()
    if (name.length < 2) throw createError({ statusCode: 400, statusMessage: 'Name needs at least 2 characters' })
    fresh.name = name
    const member = db.members.find((m) => m.email === fresh.email)
    if (member) member.name = name
  }
  if (body.title !== undefined) fresh.title = body.title.trim() || 'Developer'
  if (body.company !== undefined) fresh.company = body.company.trim() || undefined
  saveDb(db)
  return fresh
})
