import { useDb, saveDb } from '../../utils/db'

export default defineEventHandler((event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const i = db.shortcodes.findIndex((s) => s.id === id)
  if (i < 0) throw createError({ statusCode: 404, statusMessage: 'Short code not found' })
  db.shortcodes.splice(i, 1)
  saveDb(db)
  return { ok: true }
})
