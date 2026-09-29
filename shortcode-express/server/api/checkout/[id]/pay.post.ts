// ---------------------------------------------------------------------------
// Pay a webcheckout session (simulated mobile-money / card rails with a
// realistic delay) and FULFIL it instantly:
//   · shortcode → provision the code, deploy the starter flow, release v1.0.0
//   · topup     → credit the session pack to the code
// Payments emit `payment.succeeded` (+ `shortcode.assigned` / `sessions.topped_up`)
// webhooks and drop a paid invoice into billing.
// ---------------------------------------------------------------------------
import { randomUUID } from 'node:crypto'
import { useDb, saveDb, rid, SHORTCODE_SETUP_FEES, SESSION_PACKS, PORT_FLAT_MONTHLY } from '../../../utils/db'
import { requireUser } from '../../../utils/auth'
import { emitWebhook } from '../../../utils/webhooks'
import { provisionStarterFlow } from '../../../utils/starter'
import { findShortcodeByCode } from '../../../utils/ussd'
import type { Invoice, ShortCode } from '../../../../shared/types'

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const id = getRouterParam(event, 'id')
  const body = await readBody<{
    method?: 'momo' | 'card'
    momoNetwork?: string
    phone?: string
    cardNumber?: string
  }>(event).catch(() => ({}))
  const db = useDb()

  const co = db.checkouts.find((c) => c.id === id)
  if (!co) throw createError({ statusCode: 404, statusMessage: 'Checkout not found' })
  if (co.userId !== user.id) throw createError({ statusCode: 403, statusMessage: 'This checkout belongs to another account' })
  if (co.status === 'paid') throw createError({ statusCode: 409, statusMessage: 'Already paid' })
  if (co.status !== 'pending') throw createError({ statusCode: 409, statusMessage: `Checkout is ${co.status}` })

  const method = body.method === 'card' ? 'card' : 'momo'

  // --- simulated rails -------------------------------------------------------
  //   · card ending 0000 → declined          · MoMo phone ending 000 → declined
  //   · everything else succeeds (after a realistic pause)
  await new Promise((r) => setTimeout(r, method === 'momo' ? 2600 : 1400))
  if (method === 'card') {
    const digits = (body.cardNumber ?? '').replace(/\D/g, '')
    if (digits.length < 12) {
      co.failureReason = 'invalid_card'
      throw createError({ statusCode: 402, statusMessage: 'Enter a full card number' })
    }
    if (digits.endsWith('0000')) {
      // declines leave the checkout PENDING so the customer can retry with
      // another card or payment method — nothing was captured.
      co.failureReason = 'card_declined'; saveDb(db)
      throw createError({ statusCode: 402, statusMessage: 'Card declined — try another card (test cards ending 0000 always decline)' })
    }
  } else {
    const phone = (body.phone ?? '').replace(/\D/g, '')
    if (phone.length < 9) {
      co.failureReason = 'invalid_phone'; saveDb(db)
      throw createError({ statusCode: 402, statusMessage: 'Enter the mobile money number' })
    }
    if (phone.endsWith('000')) {
      co.failureReason = 'momo_declined'; saveDb(db)
      throw createError({ statusCode: 402, statusMessage: 'MoMo prompt was rejected — try again (test numbers ending 000 always decline)' })
    }
  }

  const fresh = db.users.find((u) => u.id === user.id)!
  const now = new Date().toISOString()

  // --- fulfilment -------------------------------------------------------------
  const result: Record<string, unknown> = {}
  if (co.kind === 'shortcode') {
    const plan = db.plans.find((p) => p.id === String(co.meta.planId)) ?? db.plans[0]
    let code = String(co.meta.code ?? '')
    if (co.meta.mode === 'user') {
      const clash = findShortcodeByCode(code)
      if (clash) {
        co.status = 'failed'; co.failureReason = 'code_taken'; saveDb(db)
        throw createError({ statusCode: 409, statusMessage: `${code} was just taken — your payment was not charged. Pick another code.` })
      }
    } else {
      // system-pick: confirm still free at fulfilment, else take the next one
      if (findShortcodeByCode(code)) {
        code = nextSystemCode(db)
        co.meta.code = code
      }
    }
    const sc: ShortCode = {
      id: rid('sc'),
      code,
      label: String(co.meta.label ?? 'My USSD service'),
      network: String(co.meta.network ?? 'MTN'),
      status: 'active',
      plan: plan.id,
      sessionsUsed: 0,
      sessionsQuota: plan.sessionQuota,
      createdAt: now,
      ownerId: user.id,
      assignedBy: co.meta.mode === 'user' ? 'user' : 'system',
      setupFeePaid: SHORTCODE_SETUP_FEES[plan.id] ?? 250,
    }
    db.shortcodes.push(sc)
    const flow = provisionStarterFlow(db, fresh, sc) // flow + build + release v1.0.0 + binding
    result.shortcode = { id: sc.id, code: sc.code, plan: plan.id, quota: sc.sessionsQuota }
    result.flow = { id: flow.id, name: flow.name, release: flow.releases[0]?.tag }
    result.dialable = true

    void emitWebhook(db, fresh, 'shortcode.assigned', {
      shortcode: sc.code, label: sc.label, network: sc.network, plan: plan.id,
      sessions_quota: sc.sessionsQuota, assigned_by: sc.assignedBy,
      flow: { id: flow.id, name: flow.name, release: flow.releases[0]?.tag ?? null },
    })
  } else if (co.kind === 'port') {
    // Port-in: the code arrives with status 'porting' (unmetered flat rate).
    // We issue the provider interaction URL now — the customer shares it with
    // their donor provider, and the code goes live the moment they approve.
    let code = String(co.meta.code ?? '')
    if (findShortcodeByCode(code)) {
      co.status = 'failed'; co.failureReason = 'code_taken'; saveDb(db)
      throw createError({ statusCode: 409, statusMessage: `${code} is already active on ShortCodeExpress — your payment was not charged.` })
    }
    const token = randomUUID().replace(/-/g, '').slice(0, 24)
    const sc: ShortCode = {
      id: rid('sc'),
      code,
      label: String(co.meta.label ?? 'My USSD service'),
      network: String(co.meta.network ?? 'MTN'),
      status: 'porting',
      plan: 'starter',
      sessionsUsed: 0,
      sessionsQuota: 0, // ported codes are unmetered — see `flatMonthly`
      createdAt: now,
      ownerId: user.id,
      assignedBy: 'port',
      flatMonthly: PORT_FLAT_MONTHLY,
      port: { provider: String(co.meta.provider ?? 'your provider'), token, requestedAt: now },
    }
    db.shortcodes.push(sc)
    const flow = provisionStarterFlow(db, fresh, sc) // ready to dial the instant the port completes
    const origin = getRequestURL(event).origin
    result.shortcode = { id: sc.id, code: sc.code, status: sc.status, flatMonthly: sc.flatMonthly }
    result.port = { token, url: `${origin}/port/${token}`, provider: sc.port.provider }
    result.flow = { id: flow.id, name: flow.name, release: flow.releases[0]?.tag }
    result.dialable = false
  } else {
    const sc = db.shortcodes.find((s) => s.id === String(co.meta.shortcodeId))
    if (!sc) {
      co.status = 'failed'; co.failureReason = 'shortcode_missing'; saveDb(db)
      throw createError({ statusCode: 404, statusMessage: 'Short code disappeared — nothing was charged' })
    }
    const pack = SESSION_PACKS.find((p) => p.id === String(co.meta.packId))
    const sessions = Number(co.meta.sessions ?? pack?.sessions ?? 0)
    sc.sessionsQuota += sessions
    result.shortcode = { id: sc.id, code: sc.code, added: sessions, quota: sc.sessionsQuota }
    void emitWebhook(db, fresh, 'sessions.topped_up', {
      shortcode: sc.code, sessions_added: sessions, sessions_quota: sc.sessionsQuota,
      amount: co.total, currency: co.currency,
    })
  }

  // --- payment + invoice ------------------------------------------------------
  co.status = 'paid'
  co.paidAt = now
  co.paymentMethod = method
  co.result = result

  const month = now.slice(0, 7)
  const seq = db.invoices.length + 93
  const invoice: Invoice = {
    id: rid('in'),
    number: `SCE-${now.slice(0, 4)}-${String(seq).padStart(4, '0')}`,
    period: new Date(now).toLocaleString('en-GB', { month: 'long', year: 'numeric' }),
    issuedAt: now,
    dueAt: now,
    status: 'paid',
    lines: co.items.map((i) => ({ label: i.label, qty: i.qty, unit: i.unit, amount: i.amount })),
    total: co.total,
  }
  db.invoices.unshift(invoice)

  void emitWebhook(db, fresh, 'payment.succeeded', {
    checkout_id: co.id,
    kind: co.kind,
    method,
    amount: co.total,
    currency: co.currency,
    items: co.items.map((i) => i.label),
    invoice: invoice.number,
    result,
  })

  saveDb(db)
  return { checkout: co, invoice, result }
})

function nextSystemCode(db: ReturnType<typeof useDb>): string {
  for (let n = 10; n <= 999; n++) {
    const code = `*714*${n}#`
    if (!db.shortcodes.some((s) => s.code === code) && !db.checkouts.some((c) => c.status === 'pending' && c.meta.code === code)) return code
  }
  return `*71${Date.now() % 10000}#`
}
