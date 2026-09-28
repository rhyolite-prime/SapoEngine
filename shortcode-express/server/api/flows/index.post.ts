import { useDb, saveDb, rid } from '../../utils/db'
import type { Flow, SapoBlueprint } from '../../../shared/types'
import { blueprintToGraph } from '../../../shared/utils/sapo'

export default defineEventHandler(async (event) => {
  const body = await readBody<{ name?: string; description?: string; shortcodeId?: string; blueprint?: SapoBlueprint }>(event)
  const db = useDb()
  const now = new Date().toISOString()
  const flow: Flow = {
    id: rid('fl'),
    name: body.name?.trim() || 'Untitled USSD service',
    description: body.description ?? '',
    shortcodeId: body.shortcodeId,
    createdAt: now,
    updatedAt: now,
    graph: body.blueprint ? blueprintToGraph(body.blueprint) : { entryId: null, nodes: [], edges: [] },
    meta: {
      name: String(body.blueprint?.name ?? `sce.${(body.name ?? 'workflow').toLowerCase().replace(/[^a-z0-9]+/g, '.')}`),
      version: String(body.blueprint?.version ?? '1.0'),
      defaults: (body.blueprint?.defaults as Record<string, unknown>) ?? {},
    },
    builds: [],
    releases: [],
  }
  db.flows.push(flow)
  saveDb(db)
  return flow
})
