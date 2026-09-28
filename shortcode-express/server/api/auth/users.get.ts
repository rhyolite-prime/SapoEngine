import { useDb } from '../../utils/db'

export default defineEventHandler(() => {
  const db = useDb()
  return db.users
})
