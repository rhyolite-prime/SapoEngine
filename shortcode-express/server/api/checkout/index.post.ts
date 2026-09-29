// Create a webcheckout session. kind: 'shortcode' (buy a code) | 'topup'.
// Codes are validated/reserved here — payment fulfils them instantly.
import { useDb, saveDb, rid, SHORTCODE_SETUP_FEES, SESSION_PACKS } from '../../utils/db'
import { requireUser } from '../../utils/auth'
import { findShortcodeByCode, normalizeCode } from '../../utils/ussd'
import type { CheckoutSession } from '../../../shared/types'

const CODE_RE = /^\*[0-9]{3,5}(\*[0-9]{1,3})?#$/

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const body = await readBody<{
    kind?: 'shortcode' | 'topup'
    // shortcode purchase
    mode?: 'system' | 'user'
    code?: string
    label?: string
    network?: string
    planId?: string
    // topup
    shortcodeId?: string
    packId?: string
  }>(event)
  const db = useDb()

  let items: CheckoutSession['items'] = []
  let meta: Record<string, unknown> = {}

  if (body.kind === 'shortcode') {
    const plan = db.plans.find((p) => p.id === (body.planId ?? 'starter'))
    if (!plan) throw createError({ statusCode: 400, statusMessage: 'Unknown plan' })
    const label = (body.label ?? '').trim()
    if (label.length < 2) throw createError({ statusCode: 400, statusMessage: 'Give your service a name (e.g. Kofi Airtime)' })
    const network = ['MTN', 'Vodafone', 'AirtelTigo'].includes(body.network ?? '') ? body.network! : 'MTN'

    let code = ''
    if (body.mode === 'user') {
      code = (body.code ?? '').replace(/\s+/g, '')
      if (!CODE_RE.test(code)) {
        throw createError({ statusCode: 400, statusMessage: 'Codes look like *714*42# or *565# — 3-5 digits, optional second group, then #' })
      }
      if (findShortcodeByCode(code)) {
        throw createError({ statusCode: 409, statusMessage: `${code} is already taken — try another` })
      }
    } else {
      // system picks: reserved at payment time, but pre-checked here
      code = suggestCode(db)
      if (!code) throw createError({ statusCode: 503, statusMessage: 'No codes left in the pool — contact support' })
    }

    const setup = SHORTCODE_SETUP_FEES[plan.id] ?? 250
    items = [
      { label: `${plan.name} plan — ${code} (first month)`, detail: `${plan.sessionQuota.toLocaleString()} sessions / month`, qty: 1, unit: 'month', amount: plan.priceMonthly },
      { label: 'Short code activation', detail: 'One-time provisioning fee', qty: 1, unit: 'one-time', amount: setup },
    ]
    meta = { mode: body.mode ?? 'system', code, label, network, planId: plan.id }
  } else if (body.kind === 'topup') {
    const pack = SESSION_PACKS.find((p) => p.id === body.packId)
    if (!pack) throw createError({ statusCode: 400, statusMessage: 'Pick a session pack' })
    const sc = db.shortcodes.find((s) => s.id === body.shortcodeId && s.ownerId === user.id)
      ?? db.shortcodes.find((s) => s.id === body.shortcodeId) // demo: any code is toppable by the team
    if (!sc) throw createError({ statusCode: 404, statusMessage: 'Short code not found' })
    items = [
      { label: `${pack.label} — ${sc.code}`, detail: `Pay-as-you-grow pack (GHS ${pack.perSession.toFixed(4)}/session)`, qty: 1, unit: 'pack', amount: pack.price },
    ]
    meta = { shortcodeId: sc.id, packId: pack.id, sessions: pack.sessions, code: sc.code }
  } else {
    throw createError({ statusCode: 400, statusMessage: 'Unknown checkout kind' })
  }

  // expire stale pending sessions
  const cutoff = Date.now() - 30 * 60_000
  for (const c of db.checkouts) if (c.status === 'pending' && Date.parse(c.createdAt) < cutoff) c.status = 'expired'

  const checkout: CheckoutSession = {
    id: rid('co'),
    kind: body.kind!,
    status: 'pending',
    userId: user.id,
    items,
    total: items.reduce((t, i) => t + i.amount, 0),
    currency: 'GHS',
    meta,
    createdAt: new Date().toISOString(),
  }
  db.checkouts.push(checkout)
  saveDb(db)
  return checkout
})

function suggestCode(db: ReturnType<typeof useDb>): string {
  const taken = new Set(db.shortcodes.map((s) => normalizeCode(s.code)))
  const pending = new Set(db.checkouts.filter((c) => c.status === 'pending' && c.kind === 'shortcode').map((c) => normalizeCode(String(c.meta.code ?? ''))))
  for (let n = 10; n <= 999; n++) {
    const k = normalizeCode(`*714*${n}#`)
    if (!taken.has(k) && !pending.has(k)) return `*714*${n}#`
  }
  for (let n = 1000; n <= 9999; n++) {
    const k = normalizeCode(`*71${n}#`)
    if (!taken.has(k) && !pending.has(k)) return `*71${n}#`
  }
  return ''
}
