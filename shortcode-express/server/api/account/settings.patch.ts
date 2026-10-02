// Workspace preferences from the Settings page.
import { useDb, saveDb } from '../../utils/db'
import { requireUser } from '../../utils/auth'

const NETWORKS = ['all', 'MTN', 'Vodafone', 'AirtelTigo']

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const body = await readBody<{ defaultNetwork?: string }>(event)
  const db = useDb()
  const fresh = db.users.find((u) => u.id === user.id)
  if (!fresh) throw createError({ statusCode: 404, statusMessage: 'Account not found' })

  if (body.defaultNetwork !== undefined) {
    if (!NETWORKS.includes(body.defaultNetwork)) {
      throw createError({ statusCode: 400, statusMessage: 'Pick All Networks, MTN, Vodafone or AirtelTigo' })
    }
    fresh.settings = { ...(fresh.settings ?? {}), defaultNetwork: body.defaultNetwork }
  }
  saveDb(db)
  return { settings: fresh.settings ?? {} }
})
