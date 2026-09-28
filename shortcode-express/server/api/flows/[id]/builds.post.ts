import { useDb, saveDb, rid, formatRunDate } from '../../../utils/db'
import { graphToBlueprint, validateBlueprint } from '../../../../shared/utils/sapo'
import type { Build } from '../../../../shared/types'

// Azure-pipeline style build: serialize the canvas to a Sapo blueprint and
// validate it exactly like sapo::parser::BlueprintValidator would.
export default defineEventHandler(async (event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const flow = db.flows.find((f) => f.id === id)
  if (!flow) throw createError({ statusCode: 404, statusMessage: 'Flow not found' })
  const body = await readBody<{ notes?: string }>(event).catch(() => ({ notes: '' }))

  const started = Date.now()
  const blueprint = graphToBlueprint(flow.graph, {
    name: flow.meta?.name || `sce.${flow.name.toLowerCase().replace(/[^a-z0-9]+/g, '.')}`,
    description: flow.description,
    version: flow.meta?.version || '1.0',
    defaults: flow.meta?.defaults,
  })
  const validation = validateBlueprint(blueprint, flow.graph)

  const number = flow.builds.length + 1
  const build: Build = {
    id: rid('b'),
    number,
    runId: `${formatRunDate(new Date())}.${number}`,
    status: validation.ok ? 'succeeded' : 'failed',
    createdAt: new Date().toISOString(),
    durationMs: Math.max(180, Date.now() - started + Math.floor(Math.random() * 600)),
    triggeredBy: 'ama@rhyoliteprime.com',
    blueprint,
    errors: validation.errors,
    warnings: validation.warnings,
    notes: body.notes ?? '',
  }
  flow.builds.push(build)
  flow.updatedAt = new Date().toISOString()
  saveDb(db)
  return build
})
