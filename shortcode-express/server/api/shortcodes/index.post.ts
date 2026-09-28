import { useDb, saveDb, rid } from '../../utils/db'

export default defineEventHandler(async (event) => {
  const db = useDb()
  const body = await readBody<{ code?: string; label?: string; network?: string; plan?: string; flowId?: string }>(event)
  const code = (body.code ?? '').trim()
  if (!/^\*\d[\d*]*#$/.test(code)) throw createError({ statusCode: 400, statusMessage: 'Short code must look like *713*9#' })
  if (db.shortcodes.some((s) => s.code === code)) throw createError({ statusCode: 400, statusMessage: `${code} is already provisioned` })
  const plan = db.plans.find((p) => p.id === (body.plan ?? 'starter'))
  const sc = {
    id: rid('sc'),
    code,
    label: body.label?.trim() || code,
    network: body.network || 'MTN',
    status: 'provisioning',
    plan: (plan?.id ?? 'starter') as 'starter' | 'growth' | 'scale',
    sessionsUsed: 0,
    sessionsQuota: plan?.sessionQuota ?? 10000,
    createdAt: new Date().toISOString(),
    flowId: body.flowId,
  }
  db.shortcodes.push(sc)
  saveDb(db)
  return sc
})
