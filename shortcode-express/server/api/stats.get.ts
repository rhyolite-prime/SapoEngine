import { useDb } from '../utils/db'

// Dashboard aggregates: sessions, revenue, completion rates, network split,
// hourly load and quota utilisation — everything the ECharts views consume.
export default defineEventHandler(() => {
  const db = useDb()
  const today = new Date().toISOString().slice(0, 10)
  const byDate = new Map<string, { sessions: number; completed: number; failed: number; revenue: number; duration: number; count: number }>()
  const byCode = new Map<string, { sessions: number; completed: number; failed: number; revenue: number }>()
  const dailyByCode = new Map<string, Record<string, number>>()

  for (const d of db.stats.days) {
    const agg = byDate.get(d.date) ?? { sessions: 0, completed: 0, failed: 0, revenue: 0, duration: 0, count: 0 }
    agg.sessions += d.sessions
    agg.completed += d.completed
    agg.failed += d.failed
    agg.revenue = Math.round((agg.revenue + d.revenue) * 100) / 100
    agg.duration += d.avgDurationSec
    agg.count++
    byDate.set(d.date, agg)

    const code = byCode.get(d.shortcodeId) ?? { sessions: 0, completed: 0, failed: 0, revenue: 0 }
    code.sessions += d.sessions
    code.completed += d.completed
    code.failed += d.failed
    code.revenue = Math.round((code.revenue + d.revenue) * 100) / 100
    byCode.set(d.shortcodeId, code)

    const series = dailyByCode.get(d.shortcodeId) ?? {}
    series[d.date] = d.sessions
    dailyByCode.set(d.shortcodeId, series)
  }

  const dates = [...byDate.keys()].sort()
  const totals = dates.reduce((acc, d) => {
    const a = byDate.get(d)!
    acc.sessions += a.sessions
    acc.completed += a.completed
    acc.failed += a.failed
    acc.revenue = Math.round((acc.revenue + a.revenue) * 100) / 100
    return acc
  }, { sessions: 0, completed: 0, failed: 0, revenue: 0 })

  const yesterday = dates.length > 1 ? byDate.get(dates[dates.length - 2])! : null
  const last7 = dates.slice(-7)
  const prev7 = dates.slice(-14, -7)
  const sum = (ds: string[]) => ds.reduce((s, d) => s + (byDate.get(d)?.sessions ?? 0), 0)
  const wow = prev7.length ? Math.round(((sum(last7) - sum(prev7)) / Math.max(1, sum(prev7))) * 1000) / 10 : 0

  const scAgg = db.shortcodes.map((sc) => {
    const s = byCode.get(sc.id) ?? { sessions: 0, completed: 0, failed: 0, revenue: 0 }
    const flow = db.flows.find((f) => f.id === sc.flowId)
    return {
      id: sc.id, code: sc.code, label: sc.label, plan: sc.plan, status: sc.status,
      network: sc.network, sessions: s.sessions, completed: s.completed, failed: s.failed,
      revenue: s.revenue, sessionsUsed: sc.sessionsUsed, sessionsQuota: sc.sessionsQuota,
      completionRate: s.sessions ? Math.round((s.completed / s.sessions) * 1000) / 10 : 0,
      flowName: flow?.name ?? null,
      series: Object.entries(dailyByCode.get(sc.id) ?? {}).sort(([a], [b]) => a.localeCompare(b)).map(([, v]) => v),
    }
  })

  return {
    currency: db.business.currency,
    today: { sessions: byDate.get(today)?.sessions ?? 0, hourly: db.stats.hourlyToday },
    kpis: {
      sessions30d: totals.sessions,
      sessionsToday: byDate.get(today)?.sessions ?? 0,
      sessionsTodayDelta: yesterday ? Math.round((((byDate.get(today)?.sessions ?? 0) - yesterday.sessions) / Math.max(1, yesterday.sessions)) * 1000) / 10 : 0,
      completed30d: totals.completed,
      failed30d: totals.failed,
      completionRate: totals.sessions ? Math.round((totals.completed / totals.sessions) * 1000) / 10 : 0,
      revenue30d: totals.revenue,
      revenueMtd: Math.round(dates.filter((d) => d.startsWith(today.slice(0, 7))).reduce((s, d) => s + (byDate.get(d)?.revenue ?? 0), 0) * 100) / 100,
      wowChange: wow,
      activeShortcodes: db.shortcodes.filter((s) => s.status === 'active').length,
      totalShortcodes: db.shortcodes.length,
      avgDurationSec: byDate.size ? Math.round((dates.reduce((s, d) => s + (byDate.get(d)?.duration ?? 0), 0) / byDate.size) * 10) / 10 : 0,
    },
    trend: {
      dates,
      sessions: dates.map((d) => byDate.get(d)!.sessions),
      completed: dates.map((d) => byDate.get(d)!.completed),
      failed: dates.map((d) => byDate.get(d)!.failed),
      revenue: dates.map((d) => byDate.get(d)!.revenue),
    },
    shortcodes: scAgg,
    networks: db.stats.networks,
    topFlows: db.stats.topFlows.map((tf) => {
      const flow = db.flows.find((f) => f.id === tf.flowId)
      return { flowId: tf.flowId, name: flow?.name ?? tf.flowId, sessions: tf.sessions, completion: tf.completion }
    }),
  }
})
