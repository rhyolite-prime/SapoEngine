import { useDb, saveDb } from '../../utils/db'

export default defineEventHandler(async (event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const sc = db.shortcodes.find((s) => s.id === id)
  if (!sc) throw createError({ statusCode: 404, statusMessage: 'Short code not found' })
  const body = await readBody<{ plan?: string; status?: string; label?: string; flowId?: string; sessionsQuota?: number; sessionsUsed?: number }>(event)
  // ported codes are flat-rated: plan changes and manual quota edits don't apply
  if (sc.port && (body.plan || body.sessionsQuota !== undefined)) {
    throw createError({ statusCode: 409, statusMessage: `${sc.code} is flat-rated (GHS ${sc.flatMonthly ?? 105}/mo, unlimited sessions) — plans and quotas don't apply to ported codes` })
  }
  // a mid-port code must not be manually activated — the donor provider approves the port
  if (sc.status === 'porting' && body.status === 'active' && !sc.port?.approvedAt) {
    throw createError({ statusCode: 409, statusMessage: `${sc.code} is mid-port — ${sc.port?.provider ?? 'the donor provider'} must approve the port first` })
  }
  if (body.plan) {
    const plan = db.plans.find((p) => p.id === body.plan)
    if (plan) { sc.plan = plan.id; sc.sessionsQuota = plan.sessionQuota }
  }
  if (body.status && ['active', 'provisioning', 'suspended'].includes(body.status)) sc.status = body.status as typeof sc.status
  if (body.label !== undefined) sc.label = body.label
  if (body.flowId !== undefined) sc.flowId = body.flowId
  if (body.sessionsQuota !== undefined) sc.sessionsQuota = body.sessionsQuota
  // demo affordance: simulate quota consumption to try the 402 top-up path
  if (body.sessionsUsed !== undefined) sc.sessionsUsed = body.sessionsUsed
  saveDb(db)
  return sc
})
