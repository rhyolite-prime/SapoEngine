import { useDb } from '../../utils/db'

export default defineEventHandler(() => {
  const db = useDb()
  return db.shortcodes.map((sc) => {
    const plan = db.plans.find((p) => p.id === sc.plan)
    const flow = db.flows.find((f) => f.id === sc.flowId)
    return { ...sc, planDetails: plan, flowName: flow?.name ?? null }
  })
})
