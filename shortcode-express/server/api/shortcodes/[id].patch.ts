import { useDb, saveDb } from '../../utils/db'

export default defineEventHandler(async (event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const sc = db.shortcodes.find((s) => s.id === id)
  if (!sc) throw createError({ statusCode: 404, statusMessage: 'Short code not found' })
  const body = await readBody<{ plan?: string; status?: string; label?: string; flowId?: string; sessionsQuota?: number }>(event)
  if (body.plan) {
    const plan = db.plans.find((p) => p.id === body.plan)
    if (plan) { sc.plan = plan.id; sc.sessionsQuota = plan.sessionQuota }
  }
  if (body.status && ['active', 'provisioning', 'suspended'].includes(body.status)) sc.status = body.status as typeof sc.status
  if (body.label !== undefined) sc.label = body.label
  if (body.flowId !== undefined) sc.flowId = body.flowId
  if (body.sessionsQuota !== undefined) sc.sessionsQuota = body.sessionsQuota
  saveDb(db)
  return sc
})
