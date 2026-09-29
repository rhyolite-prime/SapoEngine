// ---------------------------------------------------------------------------
// Outbound webhooks: signed (Stripe-style `t=<ts>,v1=<hmac>`), delivered with
// a short timeout, and every attempt is recorded so developers can inspect
// payloads, signatures and response codes in the dashboard.
// ---------------------------------------------------------------------------
import { createHmac, randomUUID } from 'node:crypto'
import { useDb, saveDb, rid } from './db'
import type { User, WebhookDelivery, WebhookEventName } from '../../shared/types'

function sign(secret: string, timestamp: number, body: string): string {
  return `t=${timestamp},v1=${createHmac('sha256', secret).update(`${timestamp}.${body}`).digest('hex')}`
}

export interface EmitOptions {
  /** deliver even if the endpoint lists a narrow event set (used by test.pings) */
  force?: boolean
  /** extra URL override (test receiver) */
}

export async function emitWebhook(
  db: ReturnType<typeof useDb>,
  user: User,
  event: WebhookEventName,
  data: Record<string, unknown>,
  opts: EmitOptions = {},
): Promise<WebhookDelivery | null> {
  const endpoint = user.webhook
  if (!endpoint || !endpoint.active || !endpoint.url) return null
  if (!opts.force && !(endpoint.events ?? []).includes(event)) return null

  const eventId = `evt_${randomUUID().slice(0, 12)}`
  const payload = { id: eventId, event, createdAt: new Date().toISOString(), data }
  const body = JSON.stringify(payload)
  const timestamp = Math.floor(Date.now() / 1000)
  const signature = sign(endpoint.secret, timestamp, body)

  const delivery: WebhookDelivery = {
    id: rid('whd'),
    userId: user.id,
    eventId,
    event,
    url: endpoint.url,
    payload,
    signature,
    status: 'failed',
    createdAt: new Date().toISOString(),
  }

  try {
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), 4000)
    const res = await fetch(endpoint.url, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'user-agent': 'ShortCodeExpress-Webhooks/1.0',
        'x-sapexp-event': event,
        'x-sapexp-signature': signature,
      },
      body,
      signal: controller.signal,
    })
    clearTimeout(timer)
    delivery.status = res.ok ? 'delivered' : 'failed'
    delivery.responseStatus = res.status
    if (!res.ok) delivery.error = `HTTP ${res.status}`
  } catch (e) {
    delivery.error = (e as Error).name === 'AbortError' ? 'timed out after 4s' : (e as Error).message
  }

  db.webhookDeliveries.unshift(delivery)
  if (db.webhookDeliveries.length > 200) db.webhookDeliveries.length = 200
  saveDb(db)
  return delivery
}

export { sign as signWebhookPayload }
