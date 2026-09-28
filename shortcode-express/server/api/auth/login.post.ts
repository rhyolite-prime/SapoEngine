import { useDb } from '../../utils/db'

export default defineEventHandler(async (event) => {
  const body = await readBody<{ email?: string }>(event)
  const db = useDb()
  const user = db.users.find((u) => u.email === body.email) ?? db.users[0]
  setCookie(event, 'sce_user', user.id, { path: '/', maxAge: 60 * 60 * 24 * 30, httpOnly: false, sameSite: 'lax' })
  return user
})
