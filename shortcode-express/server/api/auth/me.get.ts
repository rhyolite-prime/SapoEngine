import { useDb } from '../../utils/db'

export default defineEventHandler((event) => {
  const db = useDb()
  const id = getCookie(event, 'sce_user')
  if (!id) return null
  return db.users.find((u) => u.id === id) ?? null
})
