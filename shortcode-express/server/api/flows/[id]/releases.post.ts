import { useDb, saveDb, rid } from '../../../utils/db'
import type { Release } from '../../../../shared/types'

// Promote a successful build to the active release (Azure-style deployment).
export default defineEventHandler(async (event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const flow = db.flows.find((f) => f.id === id)
  if (!flow) throw createError({ statusCode: 404, statusMessage: 'Flow not found' })
  const body = await readBody<{ buildId: string; tag?: string; notes?: string }>(event)
  const build = flow.builds.find((b) => b.id === body.buildId)
  if (!build) throw createError({ statusCode: 400, statusMessage: 'Build not found for this flow' })
  if (build.status !== 'succeeded') throw createError({ statusCode: 400, statusMessage: 'Only successful builds can be released' })

  // bump the patch version when no explicit tag is given
  let tag = body.tag?.trim()
  if (!tag) {
    const latest = flow.releases.filter((r) => !r.rollbackOf).map((r) => r.tag)
    let major = 1, minor = 0, patch = -1
    for (const t of latest) {
      const m = /^v(\d+)\.(\d+)\.(\d+)$/.exec(t)
      if (m) {
        const [ma, mi, pa] = [parseInt(m[1]), parseInt(m[2]), parseInt(m[3])]
        if (ma > major || (ma === major && (mi > minor || (mi === minor && pa > patch)))) { major = ma; minor = mi; patch = pa }
      }
    }
    tag = `v${major}.${minor}.${patch < 0 ? 0 : patch + 1}`
  }

  for (const r of flow.releases) if (r.status === 'active') r.status = 'superseded'

  const release: Release = {
    id: rid('r'),
    tag,
    name: `Release ${tag}`,
    status: 'active',
    buildId: build.id,
    buildRunId: build.runId,
    notes: body.notes ?? '',
    releasedAt: new Date().toISOString(),
    releasedBy: 'ama@rhyoliteprime.com',
  }
  flow.releases.push(release)
  flow.updatedAt = new Date().toISOString()
  saveDb(db)
  return release
})
