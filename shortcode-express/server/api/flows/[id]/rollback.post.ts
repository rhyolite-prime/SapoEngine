import { useDb, saveDb, rid } from '../../../utils/db'
import type { Release } from '../../../../shared/types'

// Azure-style rollback: re-deploy a previous release. The current active
// release is marked rolled-back and a new deployment entry is appended so the
// history timeline stays auditable.
export default defineEventHandler(async (event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const flow = db.flows.find((f) => f.id === id)
  if (!flow) throw createError({ statusCode: 404, statusMessage: 'Flow not found' })
  const body = await readBody<{ releaseId: string; notes?: string }>(event)
  const target = flow.releases.find((r) => r.id === body.releaseId)
  if (!target) throw createError({ statusCode: 400, statusMessage: 'Release not found' })
  if (target.status === 'active') throw createError({ statusCode: 400, statusMessage: 'That release is already active' })

  const active = flow.releases.find((r) => r.status === 'active')
  if (active) active.status = 'rolled-back'

  const rollback: Release = {
    id: rid('r'),
    tag: target.tag,
    name: `Rollback to ${target.tag}`,
    status: 'active',
    buildId: target.buildId,
    buildRunId: target.buildRunId,
    notes: body.notes || `Rolled back to ${target.tag} (${target.buildRunId}).`,
    releasedAt: new Date().toISOString(),
    releasedBy: 'ama@rhyoliteprime.com',
    rollbackOf: target.id,
  }
  flow.releases.push(rollback)
  flow.updatedAt = new Date().toISOString()
  saveDb(db)
  return rollback
})
