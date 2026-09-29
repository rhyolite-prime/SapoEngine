// ---------------------------------------------------------------------------
// Settle a port request via the provider interaction URL (public, token-gated).
//   { action: 'approve' } → release the code: shortcode goes active, webhook
//                           `shortcode.ported` fires to the customer
//   { action: 'reject', reason? } → port refused, code suspended, customer
//                           is notified the same way
// ---------------------------------------------------------------------------
import { useDb, saveDb } from '../../utils/db'
import { emitWebhook } from '../../utils/webhooks'
import { findShortcodeByPortToken } from '../../utils/ussd'

export default defineEventHandler(async (event) => {
  const token = getRouterParam(event, 'token') ?? ''
  const body = await readBody<{ action?: 'approve' | 'reject'; reason?: string }>(event).catch(() => ({}))

  const sc = findShortcodeByPortToken(token)
  if (!sc) throw createError({ statusCode: 404, statusMessage: 'This porting link is invalid or was revoked' })
  const db = useDb()
  const port = sc.port!
  const now = new Date().toISOString()

  if (port.approvedAt) return { status: 'approved', message: `${sc.code} was already released to ShortCodeExpress`, at: port.approvedAt }
  if (port.rejectedAt) return { status: 'rejected', message: 'This port request was already declined', at: port.rejectedAt }

  const owner = db.users.find((u) => u.id === sc.ownerId) ?? null

  if (body.action === 'approve') {
    port.approvedAt = now
    sc.status = 'active'
    saveDb(db)
    if (owner) {
      void emitWebhook(db, owner, 'shortcode.ported', {
        shortcode: sc.code,
        label: sc.label,
        network: sc.network,
        provider: port.provider,
        approved_at: now,
        flat_monthly: sc.flatMonthly ?? null,
        message: `${port.provider} released ${sc.code} — it is now live on ShortCodeExpress`,
      })
    }
    return { status: 'approved', message: `${sc.code} released — routing moves to ShortCodeExpress`, at: now }
  }

  if (body.action === 'reject') {
    const reason = (body.reason ?? '').trim() || 'No reason given'
    port.rejectedAt = now
    port.rejectedReason = reason
    sc.status = 'suspended'
    saveDb(db)
    if (owner) {
      void emitWebhook(db, owner, 'shortcode.ported', {
        shortcode: sc.code,
        label: sc.label,
        network: sc.network,
        provider: port.provider,
        rejected_at: now,
        rejected_reason: reason,
        message: `${port.provider} declined the port of ${sc.code}: ${reason}`,
      })
    }
    return { status: 'rejected', message: 'Port request declined — the customer has been notified', at: now }
  }

  throw createError({ statusCode: 400, statusMessage: 'Pick an action: approve or reject' })
})
