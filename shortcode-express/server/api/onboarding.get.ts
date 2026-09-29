// One call the onboarding wizard uses to know exactly where a developer is on
// the "live in 5 minutes" path.
import { useDb } from '../utils/db'
import { requireUser } from '../utils/auth'
import { activeBlueprintFor } from '../utils/ussd'

export default defineEventHandler((event) => {
  const user = requireUser(event)
  const db = useDb()
  const mine = db.shortcodes.filter((s) => s.ownerId === user.id)
  const withFlow = mine.find((s) => s.flowId)
  const deployed = mine.find((s) => s.flowId && activeBlueprintFor(s) !== null)
  const firstCode = deployed ?? withFlow ?? mine[0] ?? null

  return {
    user: { id: user.id, name: user.name, company: user.company ?? '', createdAt: user.createdAt },
    hasKey: (user.apiKeys ?? []).some((k) => !k.revoked),
    webhookConfigured: !!user.webhook?.url,
    shortcode: firstCode
      ? {
          id: firstCode.id, code: firstCode.code, label: firstCode.label, network: firstCode.network,
          status: firstCode.status, plan: firstCode.plan, assignedBy: firstCode.assignedBy ?? 'system',
          sessionsUsed: firstCode.sessionsUsed, sessionsQuota: firstCode.sessionsQuota,
          flowId: firstCode.flowId ?? null,
          flowName: db.flows.find((f) => f.id === firstCode.flowId)?.name ?? null,
          hasRelease: !!db.flows.find((f) => f.id === firstCode.flowId)?.releases.some((r) => r.status === 'active'),
          flatMonthly: firstCode.flatMonthly ?? null,
          port: firstCode.port
            ? {
                provider: firstCode.port.provider,
                interactionUrl: `${getRequestURL(event).origin}/port/${firstCode.port.token}`,
                requestedAt: firstCode.port.requestedAt,
                approvedAt: firstCode.port.approvedAt ?? null,
                rejectedAt: firstCode.port.rejectedAt ?? null,
                rejectedReason: firstCode.port.rejectedReason ?? null,
              }
            : null,
        }
      : null,
  }
})
