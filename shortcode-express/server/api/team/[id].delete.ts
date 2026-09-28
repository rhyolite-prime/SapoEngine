import { useDb, saveDb } from '../../utils/db'

export default defineEventHandler((event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const member = db.members.find((m) => m.id === id)
  if (member?.role === 'owner') throw createError({ statusCode: 400, statusMessage: 'The workspace owner cannot be removed' })
  const i = db.members.findIndex((m) => m.id === id)
  if (i < 0) throw createError({ statusCode: 404, statusMessage: 'Member not found' })
  db.members.splice(i, 1)
  saveDb(db)
  return { ok: true }
})
