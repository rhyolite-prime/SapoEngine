import { useDb } from '../../utils/db'

export default defineEventHandler(() => {
  const db = useDb()
  const buildCount = (f: { builds: unknown[] }) => f.builds.length
  return db.flows.map((f) => {
    const active = f.releases.find((r) => r.status === 'active')
    const lastBuild = f.builds[f.builds.length - 1]
    const sc = db.shortcodes.find((s) => s.id === f.shortcodeId)
    return {
      id: f.id,
      name: f.name,
      description: f.description,
      shortcodeId: f.shortcodeId,
      shortcode: sc?.code ?? null,
      nodeCount: f.graph.nodes.length,
      updatedAt: f.updatedAt,
      activeRelease: active ? { tag: active.tag, releasedAt: active.releasedAt } : null,
      buildCount: buildCount(f),
      lastBuildStatus: lastBuild?.status ?? null,
      lastBuildRunId: lastBuild?.runId ?? null,
    }
  })
})
