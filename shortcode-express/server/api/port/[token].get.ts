// ---------------------------------------------------------------------------
// PROVIDER INTERACTION URL (public, token-gated) — the link a porting customer
// shares with their current (donor) short code provider. No auth: the 24-char
// token IS the secret. The donor provider uses it to inspect and settle the
// port request.
// ---------------------------------------------------------------------------
import { useDb } from '../../utils/db'
import { findShortcodeByPortToken } from '../../utils/ussd'

export default defineEventHandler((event) => {
  const token = getRouterParam(event, 'token') ?? ''
  const sc = findShortcodeByPortToken(token)
  if (!sc) throw createError({ statusCode: 404, statusMessage: 'This porting link is invalid or was revoked' })
  const db = useDb()
  const owner = db.users.find((u) => u.id === sc.ownerId) ?? null
  const port = sc.port!

  return {
    code: sc.code,
    label: sc.label,
    network: sc.network,
    donorProvider: port.provider,
    customer: { name: owner?.name ?? 'A ShortCodeExpress customer', company: owner?.company || null },
    flatMonthly: sc.flatMonthly ?? null,
    requestedAt: port.requestedAt,
    status: port.approvedAt ? 'approved' : port.rejectedAt ? 'rejected' : 'pending',
    approvedAt: port.approvedAt ?? null,
    rejectedAt: port.rejectedAt ?? null,
    rejectedReason: port.rejectedReason ?? null,
  }
})
