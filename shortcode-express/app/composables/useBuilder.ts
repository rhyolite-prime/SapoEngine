import type { Edge, Node, Connection } from '@vue-flow/core'
import type { Build, Flow, FlowEdge, FlowGraph, FlowNodeData, NodeKind, Release, ValidationIssue } from '~/../shared/types'
import { graphToBlueprint, paletteByKind, uid, nodeRefId, validateBlueprint, type SapoBlueprint } from '~/../shared/utils/sapo'

export interface CanvasNodeData extends Record<string, unknown> {
  kind: NodeKind
  config: FlowNodeData
}

export interface BuilderCtx {
  flowId: string
  flowName: string
  meta: { name: string; version: string; defaults: Record<string, unknown> }
  nodes: Ref<Node<CanvasNodeData>[]>
  edges: Ref<Edge[]>
  entryId: Ref<string | null>
  selectedId: Ref<string | null>
  dirty: Ref<boolean>
  saving: Ref<boolean>
  lastSavedAt: Ref<string>
  issues: Ref<ValidationIssue[]>
  // actions
  addNode: (kind: NodeKind, position: { x: number; y: number }) => string
  updateConfig: (id: string, patch: Partial<FlowNodeData>) => void
  renameNode: (id: string, newId: string) => boolean
  removeNode: (id: string) => void
  duplicateNode: (id: string) => void
  setEntry: (id: string) => void
  onConnect: (conn: Connection) => void
  removeEdgeById: (id: string) => void
  toGraph: () => FlowGraph
  toBlueprint: () => SapoBlueprint
  validate: () => ValidationIssue[]
  save: () => Promise<void>
  build: () => Promise<Build>
  release: (buildId: string, tag: string, notes: string) => Promise<Release>
  rollback: (releaseId: string) => Promise<Release>
  importBlueprint: (bp: SapoBlueprint) => void
  flow: Ref<Flow | null>
}

export const BUILDER_KEY: InjectionKey<BuilderCtx> = Symbol('builder')

export function provideBuilder(ctx: BuilderCtx) {
  provide(BUILDER_KEY, ctx)
}

export function useBuilder(): BuilderCtx {
  const ctx = inject(BUILDER_KEY)
  if (!ctx) throw new Error('Builder context not available')
  return ctx
}

// --- helpers shared by builder components -------------------------------------

export function graphFromCanvas(nodes: Node<CanvasNodeData>[], edges: Edge[], entryId: string | null): FlowGraph {
  const graphNodes = nodes.map((n) => ({
    id: n.id,
    kind: n.data.kind,
    position: { x: Math.round(n.position.x), y: Math.round(n.position.y) },
    data: n.data.config,
  }))
  const graphEdges: FlowEdge[] = edges.map((e) => ({
    id: e.id,
    source: e.source,
    target: e.target,
    sourceHandle: (e.sourceHandle ?? 'next') as string,
  }))
  return { entryId, nodes: graphNodes, edges: graphEdges }
}

export function canvasFromGraph(graph: FlowGraph): { nodes: Node<CanvasNodeData>[]; edges: Edge[] } {
  const nodes: Node<CanvasNodeData>[] = graph.nodes.map((n) => ({
    id: n.id,
    type: 'sapo',
    position: { ...n.position },
    data: { kind: n.kind, config: n.data },
  }))
  const edges: Edge[] = graph.edges.map((e) => {
    const isErr = e.sourceHandle === 'error' || e.sourceHandle === 'catch'
    const isOpt = e.sourceHandle.startsWith('option:') || e.sourceHandle.startsWith('case:')
    return {
      id: e.id,
      source: e.source,
      target: e.target,
      sourceHandle: e.sourceHandle,
      targetHandle: null,
      label: edgeLabel(graph, e),
      animated: false,
      style: isErr ? { stroke: '#f43f5e', strokeDasharray: '6 3' } : isOpt ? { stroke: '#7c3aed' } : { stroke: '#94a3b8' },
      labelStyle: { fill: '#475569', fontSize: '10px' },
      labelBgStyle: { fill: '#fff' },
    }
  })
  return { nodes, edges }
}

export function edgeLabel(graph: FlowGraph, e: FlowEdge): string | undefined {
  const src = graph.nodes.find((n) => n.id === e.source)
  if (!src) return undefined
  if (e.sourceHandle.startsWith('option:')) {
    const optId = e.sourceHandle.split(':')[1]
    const opt = (src.data.options ?? []).find((o) => o.id === optId)
    const i = (src.data.options ?? []).findIndex((o) => o.id === optId)
    return opt ? (opt.value || String(i + 1)) : undefined
  }
  if (e.sourceHandle.startsWith('case:')) {
    const caseId = e.sourceHandle.split(':')[1]
    const c = (src.data.cases ?? []).find((c2) => c2.id === caseId)
    return c?.value
  }
  if (e.sourceHandle === 'then') return 'true'
  if (e.sourceHandle === 'else') return 'false'
  if (e.sourceHandle === 'default') return 'default'
  if (e.sourceHandle === 'body') return 'body'
  if (e.sourceHandle === 'catch') return 'catch'
  if (e.sourceHandle === 'finally') return 'finally'
  if (e.sourceHandle.startsWith('branch:')) return `b${parseInt(e.sourceHandle.split(':')[1], 10) + 1}`
  if (e.sourceHandle === 'error') return 'error'
  return undefined
}

export function newCanvasNode(kind: NodeKind, position: { x: number; y: number }, taken: Set<string>): Node<CanvasNodeData> {
  const def = paletteByKind[kind]
  const id = nodeRefId(taken)
  return {
    id,
    type: 'sapo',
    position,
    data: { kind, config: def.defaults() as FlowNodeData },
  }
}

export { uid, graphToBlueprint, validateBlueprint }
