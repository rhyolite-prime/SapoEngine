import { useDb, saveDb } from '../../utils/db'
import { requireUser } from '../../utils/auth'
import { randomBytes } from 'node:crypto'
import type { WebhookEventName } from '../../../shared/types'

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const body = await readBody<{ url?: string; events?: WebhookEventName[]; active?: boolean }>(event)
  const db = useDb()
  const fresh = db.users.find((u) => u.id === user.id)!

  let url = (body.url ?? '').trim()
  if (url && !/^https?:\/\//i.test(url)) {
    throw createError({ statusCode: 400, statusMessage: 'Webhook URL must start with http:// or https://' })
  }
  if (url) {
    try { new URL(url) } catch { throw createError({ statusCode: 400, statusMessage: 'That URL does not parse' }) }
  }

  fresh.webhook = {
    url,
    secret: fresh.webhook?.secret ?? `whsec_${randomBytes(16).toString('hex')}`,
    events: body.events ?? fresh.webhook?.events ?? [],
    active: body.active ?? true,
  }
  saveDb(db)
  return fresh.webhook
})
