import { useDb, saveDb } from '../../utils/db'

export default defineEventHandler(async (event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const member = db.members.find((m) => m.id === id)
  if (!member) throw createError({ statusCode: 404, statusMessage: 'Member not found' })
  const body = await readBody<{ role?: 'owner' | 'editor' | 'viewer' }>(event)
  if (body.role) member.role = body.role
  saveDb(db)
  return member
})
