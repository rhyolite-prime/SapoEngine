import { useDb, saveDb } from '../../../utils/db'

export default defineEventHandler((event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const i = db.invites.findIndex((inv) => inv.id === id && inv.status === 'pending')
  if (i < 0) throw createError({ statusCode: 404, statusMessage: 'Invite not found' })
  db.invites.splice(i, 1)
  saveDb(db)
  return { ok: true }
})
