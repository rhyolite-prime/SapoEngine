import { useDb } from '../../utils/db'
import { requireUser } from '../../utils/auth'

export default defineEventHandler((event) => {
  const user = requireUser(event)
  const db = useDb()
  return {
    endpoint: user.webhook ?? null,
    deliveries: db.webhookDeliveries.filter((d) => d.userId === user.id).slice(0, 50),
  }
})
