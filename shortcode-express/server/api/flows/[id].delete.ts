import { useDb, saveDb } from '../../utils/db'

export default defineEventHandler((event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const i = db.flows.findIndex((f) => f.id === id)
  if (i < 0) throw createError({ statusCode: 404, statusMessage: 'Flow not found' })
  db.flows.splice(i, 1)
  saveDb(db)
  return { ok: true }
})
