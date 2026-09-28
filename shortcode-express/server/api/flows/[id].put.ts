import { useDb, saveDb } from '../../utils/db'
import type { FlowGraph } from '../../../shared/types'

export default defineEventHandler(async (event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const flow = db.flows.find((f) => f.id === id)
  if (!flow) throw createError({ statusCode: 404, statusMessage: 'Flow not found' })
  const body = await readBody<{ name?: string; description?: string; graph?: FlowGraph; shortcodeId?: string; meta?: { name?: string; version?: string; defaults?: Record<string, unknown> } }>(event)
  if (body.name !== undefined) flow.name = body.name
  if (body.description !== undefined) flow.description = body.description
  if (body.shortcodeId !== undefined) flow.shortcodeId = body.shortcodeId
  if (body.graph !== undefined) flow.graph = body.graph
  if (body.meta?.name !== undefined) flow.meta.name = body.meta.name
  if (body.meta?.version !== undefined) flow.meta.version = body.meta.version
  if (body.meta?.defaults !== undefined) flow.meta.defaults = body.meta.defaults
  flow.updatedAt = new Date().toISOString()
  saveDb(db)
  return flow
})

