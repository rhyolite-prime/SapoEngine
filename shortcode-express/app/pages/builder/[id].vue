<script setup lang="ts">
import { VueFlow, useVueFlow, type Connection, type Edge, type Node, type NodeMouseEvent } from '@vue-flow/core'
import { Background } from '@vue-flow/background'
import { Controls } from '@vue-flow/controls'
import { MiniMap } from '@vue-flow/minimap'
import { MarkerType } from '@vue-flow/core'
import {
  ArrowLeftIcon, DocumentTextIcon, PlayCircleIcon, RocketLaunchIcon,
  CloudArrowUpIcon, WrenchScrewdriverIcon, CheckIcon, ExclamationTriangleIcon,
  SparklesIcon, QuestionMarkCircleIcon, LightBulbIcon, XMarkIcon, ListBulletIcon,
} from '@heroicons/vue/24/outline'
import { chronologicalLayout } from '~/utils/flowExplain'
import type { Build, Flow, FlowNodeData, NodeKind, Release, ValidationIssue } from '~/../shared/types'
import type { SapoBlueprint } from '~/../shared/utils/sapo'
import { graphToBlueprint, paletteByKind, nodeRefId, validateBlueprint } from '~/../shared/utils/sapo'
import { blueprintToGraph } from '~/../shared/utils/sapo'

definePageMeta({ layout: 'builder' })

const route = useRoute()
const flowId = route.params.id as string

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const flow = ref<Flow | null>(null)
const flowName = ref('')
const meta = reactive({ name: '', version: '1.0', defaults: {} as Record<string, unknown> })
const nodes = ref<Node<import('~/composables/useBuilder').CanvasNodeData>[]>([])
const edges = ref<Edge[]>([])
const entryId = ref<string | null>(null)
const selectedId = ref<string | null>(null)
const dirty = ref(false)
const saving = ref(false)
const lastSavedAt = ref('')
const issues = ref<ValidationIssue[]>([])
const builds = ref<Build[]>([])
const releases = ref<Release[]>([])
const rightTab = ref<'node' | 'story' | 'build'>('node')
const showBlueprint = ref(false)
const showSimulator = ref(false)
const showShortcuts = ref(false)

// hover explain-card state
const hoverId = ref<string | null>(null)
const hoverX = ref(0)
const hoverY = ref(0)
let hoverTimer: ReturnType<typeof setTimeout> | undefined

// dismissible tips banner (per browser)
const tipsDismissed = ref(true)
onMounted(() => { tipsDismissed.value = localStorage.getItem('sce.builder.tips') === 'off' })

const { screenToFlowCoordinate, fitView } = useVueFlow()

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
const { data: loaded, error } = await useFetch<Flow>(`/api/flows/${flowId}`)
if (error.value || !loaded.value) {
  throw createError({ statusCode: 404, statusMessage: 'Flow not found', fatal: true })
}
onMounted(() => {
  const f = loaded.value!
  flow.value = f
  flowName.value = f.name
  meta.name = f.meta?.name ?? ''
  meta.version = f.meta?.version ?? '1.0'
  meta.defaults = { ...(f.meta?.defaults ?? {}) }
  builds.value = [...f.builds].sort((a, b) => b.number - a.number)
  releases.value = [...f.releases].sort((a, b) => b.releasedAt.localeCompare(a.releasedAt))
  const { nodes: cn, edges: ce } = canvasFromGraph(f.graph)
  nodes.value = cn
  edges.value = ce
  entryId.value = f.graph.entryId
  lastSavedAt.value = f.updatedAt
  if (cn.length) {
    selectedId.value = entryId.value ?? cn[0].id
    setTimeout(() => fitView({ padding: 0.2, duration: 300 }), 150)
  }
})

// ---------------------------------------------------------------------------
// Graph -> model helpers
// ---------------------------------------------------------------------------
function graphNow() {
  return graphFromCanvas(nodes.value, edges.value, entryId.value)
}

function toBlueprint(): SapoBlueprint {
  return graphToBlueprint(graphNow(), {
    name: meta.name || `sce.${flowName.value.toLowerCase().replace(/[^a-z0-9]+/g, '.')}`,
    description: flow.value?.description ?? '',
    version: meta.version,
    defaults: meta.defaults,
  })
}

function markDirty() { dirty.value = true }

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------
function addNode(kind: NodeKind, position: { x: number; y: number }) {
  const taken = new Set(nodes.value.map((n) => n.id))
  const node = newCanvasNode(kind, position, taken)
  nodes.value.push(node)
  if (nodes.value.length === 1) entryId.value = node.id
  selectedId.value = node.id
  rightTab.value = 'node'
  markDirty()
  return node.id
}

function updateConfig(id: string, patch: Partial<FlowNodeData>) {
  const n = nodes.value.find((x) => x.id === id)
  if (!n) return
  n.data = { ...n.data, config: { ...n.data.config, ...patch } }
  refreshEdgeLabels()
  markDirty()
}

function removeNode(id: string) {
  edges.value = edges.value.filter((e) => e.source !== id && e.target !== id)
  const i = nodes.value.findIndex((n) => n.id === id)
  if (i >= 0) nodes.value.splice(i, 1)
  if (entryId.value === id) entryId.value = nodes.value[0]?.id ?? null
  if (selectedId.value === id) selectedId.value = null
  markDirty()
}

function duplicateNode(id: string) {
  const src = nodes.value.find((n) => n.id === id)
  if (!src) return
  const taken = new Set(nodes.value.map((n) => n.id))
  const newId = nodeRefId(taken)
  const copy: typeof src = {
    id: newId,
    type: 'sapo',
    position: { x: src.position.x + 60, y: src.position.y + 90 },
    data: JSON.parse(JSON.stringify(src.data)),
  }
  nodes.value.push(copy)
  selectedId.value = newId
  markDirty()
}

function setEntry(id: string) {
  entryId.value = id
  markDirty()
}

function onConnect(conn: Connection) {
  const handle = conn.sourceHandle ?? 'next'
  const exists = edges.value.some((e) => e.source === conn.source && e.sourceHandle === handle && e.target === conn.target)
  if (exists) return
  const src = nodes.value.find((n) => n.id === conn.source)
  // structural nodes own their targets: prevent replacing an option/case/branch edge
  if (src && (handle.startsWith('option:') || handle.startsWith('case:') || handle.startsWith('branch:') || ['body', 'catch', 'finally', 'then', 'else', 'default'].includes(handle))) {
    edges.value = edges.value.filter((e) => !(e.source === conn.source && e.sourceHandle === handle))
  }
  const isErr = handle === 'error' || handle === 'catch'
  const isOpt = handle.startsWith('option:') || handle.startsWith('case:')
  const label = edgeLabelFor(src, handle)
  edges.value.push({
    id: uid('e'),
    source: conn.source,
    target: conn.target,
    sourceHandle: handle,
    targetHandle: null,
    label,
    style: isErr ? { stroke: '#f43f5e', strokeDasharray: '6 3' } : isOpt ? { stroke: '#80004d' } : { stroke: '#94a3b8' },
    labelStyle: { fill: '#475569', fontSize: '10px' },
    labelBgStyle: { fill: '#fff' },
    markerEnd: MarkerType.ArrowClosed,
  })
  markDirty()
}

function edgeLabelFor(src: Node<import('~/composables/useBuilder').CanvasNodeData> | undefined, handle: string): string | undefined {
  if (!src) return undefined
  const d = src.data.config
  if (handle.startsWith('option:')) {
    const optId = handle.split(':')[1]
    const i = (d.options ?? []).findIndex((o) => o.id === optId)
    const opt = (d.options ?? [])[i]
    return opt ? (opt.value || String(i + 1)) : undefined
  }
  if (handle.startsWith('case:')) {
    const c = (d.cases ?? []).find((x) => x.id === handle.split(':')[1])
    return c?.value
  }
  if (handle === 'then') return 'true'
  if (handle === 'else') return 'false'
  if (handle === 'default') return 'default'
  if (handle === 'body') return 'body'
  if (handle === 'catch') return 'catch'
  if (handle === 'finally') return 'finally'
  if (handle.startsWith('branch:')) return `b${parseInt(handle.split(':')[1], 10) + 1}`
  if (handle === 'error') return 'error'
  return undefined
}

function refreshEdgeLabels() {
  for (const e of edges.value) {
    const src = nodes.value.find((n) => n.id === e.source)
    e.label = edgeLabelFor(src, e.sourceHandle ?? 'next')
  }
}

function removeEdgeById(id: string) {
  edges.value = edges.value.filter((e) => e.id !== id)
  markDirty()
}

function importBlueprint(bp: SapoBlueprint) {
  const graph = blueprintToGraph(bp)
  const { nodes: cn, edges: ce } = canvasFromGraph(graph)
  nodes.value = cn
  edges.value = ce
  entryId.value = graph.entryId
  selectedId.value = graph.entryId
  meta.name = String(bp.name ?? meta.name)
  meta.version = String(bp.version ?? meta.version)
  meta.defaults = { ...(bp.defaults ?? {}) }
  dirty.value = true
  setTimeout(() => fitView({ padding: 0.2, duration: 300 }), 100)
}

// ---------------------------------------------------------------------------
// Persistence & pipeline
// ---------------------------------------------------------------------------
async function save() {
  saving.value = true
  try {
    await $fetch(`/api/flows/${flowId}`, {
      method: 'PUT',
      body: { name: flowName.value, graph: graphNow(), meta: { name: meta.name, version: meta.version, defaults: meta.defaults } },
    })
    dirty.value = false
    lastSavedAt.value = new Date().toISOString()
  } finally {
    saving.value = false
  }
}

function validate(): ValidationIssue[] {
  const v = validateBlueprint(toBlueprint(), graphNow())
  issues.value = [...v.errors, ...v.warnings]
  return v.errors
}

async function build(): Promise<Build> {
  await save()
  const buildResult = await $fetch<Build>(`/api/flows/${flowId}/builds`, { method: 'POST', body: {} })
  if (flow.value) flow.value.builds = [buildResult, ...(flow.value.builds ?? [])]
  issues.value = [...buildResult.errors, ...buildResult.warnings]
  rightTab.value = 'build'
  return buildResult
}

async function release(buildId: string, tag: string, notes: string): Promise<Release> {
  const rel = await $fetch<Release>(`/api/flows/${flowId}/releases`, { method: 'POST', body: { buildId, tag: tag || undefined, notes } })
  if (flow.value) flow.value.releases = [rel, ...(flow.value.releases ?? []).map((r) => (r.status === 'active' ? { ...r, status: 'superseded' as const } : r))]
  return rel
}

async function rollback(releaseId: string): Promise<Release> {
  const rel = await $fetch<Release>(`/api/flows/${flowId}/rollback`, { method: 'POST', body: { releaseId } })
  if (flow.value) flow.value.releases = [rel, ...(flow.value.releases ?? []).map((r) => (r.status === 'active' ? { ...r, status: 'rolled-back' as const } : r))]
  return rel
}

// Ctrl/Cmd+S
function onKey(e: KeyboardEvent) {
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 's') {
    e.preventDefault()
    save()
  }
}
if (import.meta.client) {
  onMounted(() => window.addEventListener('keydown', onKey))
  onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
}

// Guard against losing changes
const router = useRouter()
onBeforeRouteLeave(() => {
  if (dirty.value && !confirm('You have unsaved changes on the canvas. Leave anyway?')) return false
  return true
})

// ---------------------------------------------------------------------------
// Drag & drop from palette
// ---------------------------------------------------------------------------
const dragging = ref<NodeKind | null>(null)

function onDrop(event: DragEvent) {
  const kind = dragging.value ?? (event.dataTransfer?.getData('application/sapo-node') as NodeKind | undefined)
  dragging.value = null
  if (!kind || !paletteByKind[kind]) return
  const position = screenToFlowCoordinate({ x: event.clientX, y: event.clientY })
  addNode(kind, { x: position.x - 110, y: position.y - 30 })
}

function onDragOver(e: DragEvent) { e.preventDefault() }

function onNodeClick({ node }: NodeMouseEvent) {
  selectedId.value = node.id
  rightTab.value = 'node'
}

function onEdgeClick({ edge }: { edge: Edge }) {
  edge.selected = true
}

function onEdgesChange(changes: Array<{ type: string }>) {
  if (changes.some((c) => c.type === 'remove')) markDirty()
}

// --- hover explain card + focus highlighting --------------------------------
function onNodeMouseEnter({ node, event }: { node: { id: string }; event: MouseEvent }) {
  clearTimeout(hoverTimer)
  hoverX.value = event.clientX
  hoverY.value = event.clientY
  hoverTimer = setTimeout(() => {
    hoverId.value = node.id
    applyEdgeFocus(node.id)
  }, 260)
}
function onNodeMouseLeave() {
  clearTimeout(hoverTimer)
  if (!hoverId.value) return
  hoverId.value = null
  applyEdgeFocus(null)
}
function applyEdgeFocus(id: string | null) {
  // re-bind the array so Vue Flow picks up the class changes
  edges.value = edges.value.map((e) => {
    const hi = !!id && (e.source === id || e.target === id)
    return { ...e, class: !id ? '' : hi ? 'ef-hi' : 'ef-dim' }
  })
}

// --- chronological tidy layout ------------------------------------------------
function tidyLayout() {
  if (!nodes.value.length) return
  hoverId.value = null
  applyEdgeFocus(null)
  chronologicalLayout(nodes.value as never, edges.value, entryId.value)
  markDirty()
  nextTick(() => fitView({ padding: 0.15, duration: 500 }))
}

function focusStoryNode(id: string) {
  selectedId.value = id
  rightTab.value = 'node'
  nextTick(() => fitView({ nodes: [id], duration: 450, maxZoom: 1.25, padding: 4 }))
}

function dismissTips() {
  tipsDismissed.value = true
  localStorage.setItem('sce.builder.tips', 'off')
}

// --- keyboard shortcuts --------------------------------------------------------
function onKeydown(e: KeyboardEvent) {
  const t = e.target as HTMLElement
  if (t && ['INPUT', 'TEXTAREA', 'SELECT'].includes(t.tagName)) return
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 's') { e.preventDefault(); void save() }
  else if (e.key === '?') { showShortcuts.value = !showShortcuts.value }
  else if (e.key.toLowerCase() === 'f' && !e.metaKey && !e.ctrlKey) { fitView({ padding: 0.15, duration: 350 }) }
  else if (e.key === 'Escape') { showShortcuts.value = false }
}
onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => { window.removeEventListener('keydown', onKeydown); clearTimeout(hoverTimer) })

// Provide context to child components
provideBuilder({
  flowId,
  get flowName() { return flowName.value }, set flowName(v) { flowName.value = v; markDirty() },
  meta,
  nodes: nodes as never,
  edges,
  entryId,
  selectedId,
  hoverId,
  dirty,
  saving,
  lastSavedAt,
  issues,
  addNode,
  updateConfig,
  removeNode,
  duplicateNode,
  setEntry,
  onConnect,
  removeEdgeById,
  toGraph: graphNow,
  toBlueprint,
  validate,
  save,
  build,
  release,
  rollback,
  importBlueprint,
  flow,
})

const errorCount = computed(() => issues.value.filter((i) => i.level === 'error').length)
const activeRelease = computed(() => (flow.value?.releases ?? []).find((r) => r.status === 'active'))
const selectedNode = computed(() => nodes.value.find((n) => n.id === selectedId.value) ?? null)

function minimapColor(node: { data?: { kind?: string } }): string {
  const map: Record<string, string> = {
    menu: '#80004d', input: '#80004d', pin: '#80004d', display: '#80004d', await_event: '#80004d',
    http: '#0ea5e9', subflow: '#0ea5e9', event: '#0ea5e9',
    if: '#f59e0b', choice: '#f59e0b', try: '#f59e0b', script: '#f59e0b',
    assign: '#4bb543', transform: '#4bb543', query: '#4bb543',
    wait: '#64748b', loop: '#64748b', break: '#64748b', parallel: '#64748b', schedule: '#64748b',
    end_success: '#f43f5e', end_failure: '#f43f5e',
  }
  return map[node.data?.kind ?? ''] ?? '#94a3b8'
}
</script>

<template>
  <div
    class="flex h-screen flex-col bg-slate-200/60"
    @drop="onDrop" @dragover="onDragOver"
  >
    <!-- Toolbar -->
    <header class="z-30 flex h-14 shrink-0 items-center gap-3 border-b border-slate-200 bg-white px-4 shadow-sm">
      <NuxtLink to="/flows" class="rounded-lg p-2 text-slate-400 hover:bg-slate-100 hover:text-slate-700" title="Back to flows">
        <ArrowLeftIcon class="h-5 w-5" />
      </NuxtLink>
      <div class="h-6 w-px bg-slate-200" />
      <input
        v-model="flowName" @change="markDirty()"
        class="w-64 rounded-lg border border-transparent px-2 py-1.5 text-sm font-bold text-slate-800 hover:border-slate-200 focus:border-brand-400 focus:outline-none"
      />
      <span v-if="activeRelease" class="inline-flex items-center gap-1.5 rounded-full bg-success-50 px-2.5 py-1 text-[11px] font-bold text-success-700 ring-1 ring-success-200">
        <RocketLaunchIcon class="h-3.5 w-3.5" /> {{ activeRelease.tag }} live
      </span>
      <span v-else class="rounded-full bg-amber-50 px-2.5 py-1 text-[11px] font-bold text-amber-700 ring-1 ring-amber-200">draft</span>
      <span v-if="errorCount" class="inline-flex items-center gap-1 rounded-full bg-rose-50 px-2.5 py-1 text-[11px] font-bold text-rose-600 ring-1 ring-rose-200">
        <ExclamationTriangleIcon class="h-3.5 w-3.5" /> {{ errorCount }} error{{ errorCount > 1 ? 's' : '' }}
      </span>
      <span class="text-[11px] text-slate-400">
        {{ dirty ? '● unsaved changes' : `saved ${lastSavedAt ? new Date(lastSavedAt).toLocaleTimeString() : ''}` }}
      </span>

      <div class="ml-auto flex items-center gap-2">
        <button
          class="inline-flex items-center gap-1.5 rounded-lg border border-slate-200 px-3 py-1.5 text-xs font-semibold text-slate-700 hover:border-slate-300 hover:bg-slate-50"
          title="Re-arrange the canvas in execution order (chronological, left to right)"
          @click="tidyLayout()"
        >
          <SparklesIcon class="h-4 w-4 text-brand-600" /> Tidy
        </button>
        <button
          class="inline-flex items-center gap-1.5 rounded-lg border border-slate-200 px-3 py-1.5 text-xs font-semibold text-slate-700 hover:border-slate-300 hover:bg-slate-50"
          @click="showBlueprint = true"
        >
          <DocumentTextIcon class="h-4 w-4" /> Blueprint JSON
        </button>
        <button
          class="inline-flex items-center gap-1.5 rounded-lg border border-slate-200 px-3 py-1.5 text-xs font-semibold text-slate-700 hover:border-slate-300 hover:bg-slate-50"
          @click="showSimulator = !showSimulator"
        >
          <PlayCircleIcon class="h-4 w-4 text-brand-600" /> Simulate
        </button>
        <button
          class="inline-flex items-center gap-1.5 rounded-lg border border-slate-200 bg-white px-3 py-1.5 text-xs font-semibold text-slate-700 hover:border-slate-300"
          :class="saving ? 'opacity-60' : ''" @click="save()"
        >
          <CloudArrowUpIcon class="h-4 w-4" /> Save
        </button>
        <button
          class="inline-flex items-center gap-1.5 rounded-lg bg-slate-900 px-3.5 py-1.5 text-xs font-semibold text-white hover:bg-slate-700"
          @click="build()"
        >
          <WrenchScrewdriverIcon class="h-4 w-4" /> Build &amp; validate
        </button>
        <button
          class="rounded-lg border border-slate-200 p-2 text-slate-500 hover:border-slate-300 hover:bg-slate-50 hover:text-slate-700"
          title="Shortcuts & help (?)"
          @click="showShortcuts = !showShortcuts"
        >
          <QuestionMarkCircleIcon class="h-4 w-4" />
        </button>
        <button
          class="inline-flex items-center gap-1.5 rounded-lg bg-gradient-to-r from-brand-700 to-brand-500 px-3.5 py-1.5 text-xs font-semibold text-white shadow-sm shadow-brand-200 hover:from-brand-500 hover:to-brand-400"
          @click="rightTab = 'build'"
        >
          <RocketLaunchIcon class="h-4 w-4" /> Release
        </button>
      </div>
    </header>

    <div class="flex min-h-0 flex-1">
      <!-- Palette -->
      <BuilderPalette />

      <!-- Canvas -->
      <div class="relative min-w-0 flex-1">
        <!-- quick tips -->
        <div v-if="!tipsDismissed" class="absolute inset-x-3 top-3 z-20 flex items-start gap-2.5 rounded-xl border border-brand-200 bg-white/95 px-4 py-2.5 shadow-sm backdrop-blur">
          <LightBulbIcon class="mt-0.5 h-4 w-4 shrink-0 text-brand-600" />
          <p class="text-[11.5px] leading-relaxed text-slate-600">
            <b class="text-slate-800">Quick tips:</b>
            drag nodes in from the left — what you draw is exactly the Sapo blueprint ·
            <b>hover any node</b> to see what it does and where it leads ·
            hit <b>Tidy</b> to re-arrange in execution order ·
            build often, release when green.
            <button class="ml-1 font-semibold text-brand-700 hover:underline" @click="dismissTips()">Got it</button>
          </p>
          <button class="ml-auto shrink-0 rounded p-0.5 text-slate-400 hover:bg-slate-100 hover:text-slate-600" @click="dismissTips()">
            <XMarkIcon class="h-4 w-4" />
          </button>
        </div>

        <!-- shortcuts popover -->
        <div v-if="showShortcuts" class="absolute right-3 top-3 z-20 w-64 rounded-xl border border-slate-200 bg-white p-4 shadow-xl">
          <div class="mb-2 flex items-center justify-between">
            <h4 class="text-xs font-bold uppercase tracking-wide text-slate-500">Shortcuts</h4>
            <button class="rounded p-1 text-slate-400 hover:bg-slate-100" @click="showShortcuts = false"><XMarkIcon class="h-3.5 w-3.5" /></button>
          </div>
          <ul class="space-y-1.5 text-[11.5px] text-slate-600">
            <li class="flex justify-between"><span>Save the flow</span><kbd class="rounded border border-slate-200 bg-slate-50 px-1.5 font-mono text-[10px]">⌘/Ctrl + S</kbd></li>
            <li class="flex justify-between"><span>Fit to screen</span><kbd class="rounded border border-slate-200 bg-slate-50 px-1.5 font-mono text-[10px]">F</kbd></li>
            <li class="flex justify-between"><span>Delete selection</span><kbd class="rounded border border-slate-200 bg-slate-50 px-1.5 font-mono text-[10px]">Del / ⌫</kbd></li>
            <li class="flex justify-between"><span>Toggle this help</span><kbd class="rounded border border-slate-200 bg-slate-50 px-1.5 font-mono text-[10px]">?</kbd></li>
          </ul>
          <p class="mt-2.5 border-t border-slate-100 pt-2 text-[10.5px] leading-relaxed text-slate-400">
            Hover a node for “what happens here / next”. The Story tab walks the flow in execution order and flags unreachable nodes.
          </p>
        </div>

        <VueFlow
          v-model:nodes="nodes" v-model:edges="edges"
          :default-viewport="{ zoom: 0.85 }"
          :min-zoom="0.2" :max-zoom="2.5"
          :snap-to-grid="true" :snap-grid="[16, 16]"
          :delete-key-code="['Backspace', 'Delete']"
          fit-view-on-init
          @connect="onConnect" @node-click="onNodeClick" @edge-click="onEdgeClick"
          @edges-change="onEdgesChange"
          @node-drag-stop="markDirty" @nodes-change="markDirtyIfNeeded"
          @node-mouse-enter="onNodeMouseEnter" @node-mouse-leave="onNodeMouseLeave"
          @node-drag-start="onNodeMouseLeave" @node-double-click="({ node }: any) => focusStoryNode(node.id)"
          @pane-click="onNodeMouseLeave" @connect-start="onNodeMouseLeave"
        >
          <Background :gap="20" :size="1.4" pattern-color="#cbd5e1" />
          <Controls position="bottom-left" />
          <MiniMap position="bottom-right" :node-color="minimapColor" :mask-color="'rgba(241,245,249,0.75)'" />
          <template #node-sapo="props">
            <BuilderCanvasNode :id="props.id" :selected="props.selected" />
          </template>
        </VueFlow>

        <!-- hover explain card -->
        <BuilderNodeHoverCard v-if="hoverId" :node-id="hoverId" :x="hoverX" :y="hoverY" />

        <div v-if="!nodes.length" class="pointer-events-none absolute inset-0 flex flex-col items-center justify-center text-center">
          <div class="rounded-2xl border-2 border-dashed border-slate-300 bg-white/70 px-10 py-8">
            <div class="text-lg font-bold text-slate-500">Drag your first node here</div>
            <p class="mt-1 text-sm text-slate-400">Start with a <b>Menu</b>, add an <b>HTTP API</b>, end with <b>Terminate</b>.<br/>What you draw is exactly the Sapo DSL blueprint that ships.</p>
          </div>
        </div>
      </div>

      <!-- Right panel -->
      <div class="flex w-[360px] shrink-0 flex-col border-l border-slate-200 bg-white">
        <div class="flex shrink-0 border-b border-slate-200">
          <button
            class="flex-1 border-b-2 px-4 py-3 text-xs font-bold uppercase tracking-wide"
            :class="rightTab === 'node' ? 'border-brand-600 text-brand-700' : 'border-transparent text-slate-400 hover:text-slate-600'"
            @click="rightTab = 'node'"
          >{{ selectedNode ? 'Node settings' : 'Flow settings' }}</button>
          <button
            class="flex-1 border-b-2 px-4 py-3 text-xs font-bold uppercase tracking-wide"
            :class="rightTab === 'story' ? 'border-brand-600 text-brand-700' : 'border-transparent text-slate-400 hover:text-slate-600'"
            @click="rightTab = 'story'"
          ><ListBulletIcon class="mr-1 inline h-3.5 w-3.5 -translate-y-px" /> Story</button>
          <button
            class="flex-1 border-b-2 px-4 py-3 text-xs font-bold uppercase tracking-wide"
            :class="rightTab === 'build' ? 'border-brand-600 text-brand-700' : 'border-transparent text-slate-400 hover:text-slate-600'"
            @click="rightTab = 'build'"
          >Build &amp; release</button>
        </div>
        <div class="min-h-0 flex-1 overflow-y-auto">
          <BuilderInspector v-if="rightTab === 'node'" />
          <BuilderStoryPanel v-else-if="rightTab === 'story'" @focus="focusStoryNode" @tidy="tidyLayout" />
          <BuilderBuildPanel v-else />
        </div>
      </div>
    </div>

    <!-- Overlays -->
    <BuilderBlueprintDrawer v-if="showBlueprint" @close="showBlueprint = false" />
    <BuilderSimulator v-if="showSimulator" @close="showSimulator = false" />
  </div>
</template>

<script lang="ts">
function markDirtyIfNeeded() { /* handled via node-drag-stop to avoid loops */ }
export default { name: 'BuilderPage' }
</script>
