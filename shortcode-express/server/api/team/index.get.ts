import { useDb } from '../../utils/db'

export default defineEventHandler(() => {
  const db = useDb()
  return { members: db.members, invites: db.invites, business: db.business }
})
