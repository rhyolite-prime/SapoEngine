import { useDb } from '../../../../utils/db'
import { requireUser } from '../../../../utils/auth'
import type { WebhookEventName } from '../../../../../shared/types'

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const id = getRouterParam(event, 'id')
  const db = useDb()
  const original = db.webhookDeliveries.find((d) => d.id === id)
  if (!original || original.userId !== user.id) throw createError({ statusCode: 404, statusMessage: 'Delivery not found' })

  // re-POST the exact stored payload to the recorded URL
  let status = 'failed'
  let responseStatus: number | undefined
  let error: string | undefined
  try {
    const res = await fetch(original.url, {
      method: 'POST',
      headers: { 'content-type': 'application/json', 'x-sapexp-event': original.event, 'x-sapexp-signature': original.signature },
      body: JSON.stringify(original.payload),
    })
    status = res.ok ? 'delivered' : 'failed'
    responseStatus = res.status
    if (!res.ok) error = `HTTP ${res.status}`
  } catch (e) {
    error = (e as Error).message
  }
  const re = { ...original, id: `${original.id}_r${Date.now() % 100000}`, status, responseStatus, error, createdAt: new Date().toISOString() }
  db.webhookDeliveries.unshift(re)
  return re
})
