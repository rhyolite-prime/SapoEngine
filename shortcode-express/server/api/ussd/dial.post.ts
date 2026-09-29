// ---------------------------------------------------------------------------
// LIVE USSD GATEWAY — dial a code and drive the session.
// Auth: session cookie (the dialer UI) or `Authorization: Bearer sk_live_…`
// (your integration). Quotas are enforced; every dial counts and fires
// webhooks.
// ---------------------------------------------------------------------------
import { requireUser } from '../../utils/auth'
import { findShortcodeByCode, startSession } from '../../utils/ussd'

const NETWORKS = ['MTN', 'Vodafone', 'AirtelTigo']

export default defineEventHandler(async (event) => {
  requireUser(event)
  const body = await readBody<{ code?: string; msisdn?: string; network?: string }>(event)
  if (!body.code) throw createError({ statusCode: 400, statusMessage: 'Dial a code, e.g. *714*10#' })

  const shortcode = findShortcodeByCode(body.code)
  if (!shortcode) throw createError({ statusCode: 404, statusMessage: `${body.code} is not a ShortCodeExpress code` })

  const msisdn = (body.msisdn ?? '').replace(/\D/g, '') || randomMsisdn()
  const network = NETWORKS.includes(body.network ?? '') ? body.network! : networkFor(msisdn)

  return await startSession(event, { shortcode, msisdn, network })
})

function randomMsisdn(): string {
  const prefix = ['024', '054', '055', '020', '027', '057', '026', '056'][Math.floor(Math.random() * 8)]
  return prefix + String(Math.floor(Math.random() * 1e7)).padStart(7, '0')
}

function networkFor(msisdn: string): string {
  const p = msisdn.slice(0, 3)
  if (['024', '054', '055', '059'].includes(p)) return 'MTN'
  if (['020', '050'].includes(p)) return 'Vodafone'
  return 'AirtelTigo'
}
