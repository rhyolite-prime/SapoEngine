import { useDb } from '../../utils/db'

export default defineEventHandler(() => {
  const db = useDb()
  const today = new Date().toISOString().slice(0, 10)
  const monthDays = db.stats.days.filter((d) => d.date.startsWith(today.slice(0, 7)))
  const usageByCode = new Map<string, { sessions: number; revenue: number }>()
  for (const d of monthDays) {
    const u = usageByCode.get(d.shortcodeId) ?? { sessions: 0, revenue: 0 }
    u.sessions += d.sessions
    u.revenue = Math.round((u.revenue + d.revenue) * 100) / 100
    usageByCode.set(d.shortcodeId, u)
  }
  const usage = db.shortcodes.map((sc) => {
    const plan = db.plans.find((p) => p.id === sc.plan)!
    const u = usageByCode.get(sc.id) ?? { sessions: 0, revenue: 0 }
    const overage = Math.max(0, sc.sessionsUsed - sc.sessionsQuota)
    return {
      shortcodeId: sc.id, code: sc.code, label: sc.label, plan: plan.id, planName: plan.name,
      priceMonthly: plan.priceMonthly, quota: sc.sessionsQuota, used: sc.sessionsUsed,
      monthSessions: u.sessions, overage, overageRate: plan.overagePerSession,
      overageCost: Math.round(overage * plan.overagePerSession * 100) / 100,
      projectedTotal: Math.round((plan.priceMonthly + overage * plan.overagePerSession) * 100) / 100,
      utilisation: sc.sessionsQuota ? Math.round((sc.sessionsUsed / sc.sessionsQuota) * 1000) / 10 : 0,
    }
  })
  const monthlyRecurring = usage.reduce((s, u) => s + u.priceMonthly, 0)
  const overageTotal = Math.round(usage.reduce((s, u) => s + u.overageCost, 0) * 100) / 100
  return {
    business: db.business,
    plans: db.plans,
    invoices: [...db.invoices].sort((a, b) => b.issuedAt.localeCompare(a.issuedAt)),
    usage,
    summary: {
      monthlyRecurring,
      overageTotal,
      projectedNextInvoice: Math.round((monthlyRecurring + overageTotal) * 100) / 100,
      currency: db.business.currency,
    },
  }
})
