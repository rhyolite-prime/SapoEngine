// ---------------------------------------------------------------------------
// Flow explainability — plain-language "what happens here / what happens next"
// for hover cards, plus the chronological story walk and auto-layout used to
// keep dense canvases readable.
// ---------------------------------------------------------------------------
import type { Edge, Node } from '@vue-flow/core'
import type { FlowNodeData, NodeKind } from '~/../shared/types'
import { paletteByKind } from '~/../shared/utils/sapo'

type AnyNode = Node<{ kind: NodeKind; config: FlowNodeData }>

function cut(s: string, n = 64): string {
  const t = s.replace(/\s+/g, ' ').trim()
  return t.length > n ? t.slice(0, n - 1) + '…' : t
}

/** What happens ON this node, in subscriber/engine terms. */
export function explainNode(kind: NodeKind, d: FlowNodeData): string {
  switch (kind) {
    case 'menu': {
      const opts = (d.options ?? []).map((o) => o.label || o.value).join(', ')
      return `Shows a numbered menu${opts ? ` (${cut(opts, 70)})` : ''} and waits for the subscriber's reply. The reply is saved to \`${d.inputVariable || 'input'}\` and routed to the matching option.`
    }
    case 'input':
      return `Asks “${cut(d.message ?? '…', 80)}” and waits. The reply is saved to \`${d.inputVariable || 'input'}\`${d.validationMode === 'custom' && d.customValidation ? ' and must pass your validation rule' : ''}.`
    case 'pin':
      return `Asks for a PIN (masked on the handset). The reply is saved to \`${d.inputVariable || 'input'}\`.`
    case 'display':
      return `Shows “${cut(d.message ?? '…', 80)}” and continues immediately — no reply needed.`
    case 'await_event':
      return `Parks the session until the “${d.eventName || 'event'}” event arrives${d.triggerCondition ? ` (when: ${cut(d.triggerCondition, 60)})` : ''}.`
    case 'http': {
      const outs = (d.outputs ?? []).map((o) => o.key).filter(Boolean)
      return `Calls ${String(d.verb ?? 'post').toUpperCase()} ${cut(d.url ?? '…', 60)} while the subscriber waits${outs.length ? `. Response is mapped to: ${outs.join(', ')}` : ''}${d.retryEnabled ? ` · retries up to ${d.retry?.max_attempts ?? 2}× on failure` : ''}.`
    }
    case 'subflow':
      return `Runs the “${d.workflow || 'subflow'}” blueprint as a child flow${d.inputsJson ? ' with your inputs' : ''}, then continues here.`
    case 'event':
      return `Publishes the “${d.eventName || 'event'}” event on the bus${d.payloadJson ? ' with a payload' : ''}.`
    case 'if':
      return `Checks \`${cut(d.condition ?? '…', 70)}\` — takes the then-branch when true, the else-branch when false.`
    case 'choice':
      return `Evaluates \`${cut(d.expression ?? '…', 70)}\` and jumps to the first matching case.`
    case 'try':
      return `Runs its body. If any node inside fails, control jumps to the catch block${d.errorVariable ? ` with the error saved to \`${d.errorVariable}\`` : ''}; the finally block always runs after.`
    case 'loop': {
      const mode = d.loopMode === 'count' ? `Repeats the body ${d.count ?? 'N'} times`
        : d.loopMode === 'while' ? `Repeats the body while \`${cut(d.while ?? '…', 50)}\` holds`
        : `Repeats the body for every item in \`${cut(d.collection ?? '…', 50)}\` (as \`${d.iterator || 'item'}\`)`
      return `${mode}${d.onItemError === 'continue' ? ' · a failing item is skipped and the loop continues' : ''}.`
    }
    case 'parallel':
      return `Runs all branches at the same time and merges the results (${d.mergePolicy ?? 'wait_all'}).`
    case 'script':
      return `Runs your script${d.bindingsJson ? ' with the bound variables' : ''} and continues.`
    case 'break':
      return d.breakAction === 'continue' ? 'Skips to the next loop iteration.' : 'Exits the enclosing loop immediately.'
    case 'assign': {
      const keys = (d.assignments ?? []).map((a) => a.key).filter(Boolean)
      return keys.length ? `Sets ${keys.map((k) => `\`${k}\``).join(', ')} on the session context.` : 'Sets variables on the session context (none configured yet).'
    }
    case 'transform':
      return `Transforms \`${d.inputExpr || 'input'}\` with ${d.operation ?? 'the operation'} → \`${d.outputKey || 'result'}\`.`
    case 'query':
      return `Queries ${d.source || 'the source'}${d.where ? ` where ${cut(d.where, 50)}` : ''} → \`${d.outputKey || 'rows'}\`.`
    case 'wait':
      return `Pauses the flow for ${d.duration || 'a while'}.`
    case 'schedule':
      return `Schedules the rest of the flow (${d.cron ? `cron: ${d.cron}` : d.until ? `until ${d.until}` : 'on a timer'}).`
    case 'end_success':
      return `Ends the session successfully${d.endMessage ? ` with “${cut(d.endMessage, 80)}”` : ''}.`
    case 'end_failure':
      return `Ends the session as FAILED${d.endMessage ? ` with “${cut(d.endMessage, 80)}”` : ''}.`
    default:
      return paletteByKind[kind]?.hint ?? 'Sapo DSL node.'
  }
}

// --- outgoing connections with human-readable labels ------------------------

export interface NextStep {
  label: string       // "If reply is '1' (Buy airtime)"
  targetId: string
  targetLabel: string
  targetKind: NodeKind
}

const HANDLE_ORDER = ['option:', 'case:', 'then', 'else', 'body', 'catch', 'finally', 'branch:', 'next', 'error']

export function nextSteps(nodeId: string, nodes: AnyNode[], edges: Edge[]): NextStep[] {
  const node = nodes.find((n) => n.id === nodeId)
  if (!node) return []
  const d = node.data.config as FlowNodeData
  const labelOf = (handle: string): string => {
    if (handle === 'next') return 'Always continues to'
    if (handle === 'error') return 'If this node fails →'
    if (handle === 'then') return 'If the condition is true (then)'
    if (handle === 'else') return 'If the condition is false (else)'
    if (handle === 'body') return node.data.kind === 'try' ? 'Try body (runs first)' : 'Loop body (repeated)'
    if (handle === 'catch') return 'On error — catch block'
    if (handle === 'finally') return 'Always after — finally block'
    if (handle.startsWith('option:')) {
      const opt = (d.options ?? []).find((o) => `option:${o.id}` === handle)
      return opt ? `If the subscriber replies “${opt.value || opt.label}” (${cut(opt.label, 32)})` : 'Menu option'
    }
    if (handle.startsWith('case:')) {
      const c = (d.cases ?? []).find((x) => `case:${x.id}` === handle)
      return c ? `Case “${c.value || c.label}”` : 'Case'
    }
    if (handle.startsWith('branch:')) return `Parallel branch ${parseInt(handle.split(':')[1], 10) + 1}`
    return handle
  }
  return edges
    .filter((e) => e.source === nodeId)
    .sort((a, b) => rank(a.sourceHandle) - rank(b.sourceHandle))
    .map((e) => {
      const t = nodes.find((n) => n.id === e.target)
      return {
        label: labelOf(e.sourceHandle ?? 'next'),
        targetId: e.target,
        targetLabel: (t?.data.config.label as string) || t?.id || e.target,
        targetKind: t?.data.kind ?? 'assign',
      }
    })
}

function rank(handle: string | undefined): number {
  const h = handle ?? 'next'
  for (let i = 0; i < HANDLE_ORDER.length; i++) {
    if (HANDLE_ORDER[i].endsWith(':')) { if (h.startsWith(HANDLE_ORDER[i])) return i } else if (h === HANDLE_ORDER[i]) return i
  }
  return HANDLE_ORDER.length
}

export function isTerminal(kind: NodeKind): boolean {
  return kind === 'end_success' || kind === 'end_failure'
}

// --- chronological story -----------------------------------------------------

export interface StoryEntry {
  n: number
  depth: number
  via: string            // how we got here ("start", "if reply “1”", …)
  id: string
  label: string
  kind: NodeKind
}

export function flowStory(nodes: AnyNode[], edges: Edge[], entryId: string | null): { entries: StoryEntry[]; unreachable: AnyNode[] } {
  const entries: StoryEntry[] = []
  const visited = new Set<string>()
  let counter = 0
  const labelOf = (id: string) => {
    const n = nodes.find((x) => x.id === id)
    return (n?.data.config.label as string) || id
  }
  const kindOf = (id: string) => nodes.find((x) => x.id === id)?.data.kind ?? 'assign'

  const visit = (id: string, via: string, depth: number) => {
    if (visited.has(id)) return
    visited.add(id)
    entries.push({ n: ++counter, depth, via, id, label: labelOf(id), kind: kindOf(id) })
    for (const s of nextSteps(id, nodes, edges)) {
      const viaText = s.label.startsWith('If the subscriber') ? `reply “${s.label.match(/“([^”]*)”/)?.[1] ?? ''}”` : s.label
      visit(s.targetId, viaText, Math.min(depth + 1, 8))
    }
  }

  if (entryId && nodes.some((n) => n.id === entryId)) visit(entryId, 'session starts here', 0)
  const unreachable = nodes.filter((n) => !visited.has(n.id))
  return { entries, unreachable }
}

// --- chronological auto-layout -----------------------------------------------

/**
 * Layered left→right arrangement: every node sits one column after the nodes
 * that lead into it (shortest path from the entry), so the canvas reads in
 * execution order. Unreachable nodes are parked in a bottom row.
 */
export function chronologicalLayout(nodes: AnyNode[], edges: Edge[], entryId: string | null): void {
  if (!nodes.length) return
  const root = entryId && nodes.some((n) => n.id === entryId) ? entryId : nodes[0].id

  // BFS depth = earliest column
  const depth = new Map<string, number>([[root, 0]])
  const queue = [root]
  const order = [root]
  while (queue.length) {
    const cur = queue.shift()!
    const cd = depth.get(cur) ?? 0
    for (const e of edges.filter((x) => x.source === cur)) {
      if (!depth.has(e.target)) { depth.set(e.target, cd + 1); queue.push(e.target); order.push(e.target) }
    }
  }

  const cols = new Map<number, string[]>()
  for (const id of order) {
    const d = depth.get(id)!
    if (!cols.has(d)) cols.set(d, [])
    cols.get(d)!.push(id)
  }

  const X0 = 60, DX = 300, Y0 = 40, DY = 168
  for (const [d, ids] of [...cols.entries()].sort((a, b) => a[0] - b[0])) {
    ids.forEach((id, i) => {
      const n = nodes.find((x) => x.id === id)
      if (n) n.position = { x: X0 + d * DX, y: Y0 + i * DY }
    })
  }

  // unreachable: bottom-left row, compact
  const orphans = nodes.filter((n) => !depth.has(n.id))
  orphans.forEach((n, i) => {
    const col = Math.floor(i / 6), row = i % 6
    n.position = { x: X0 + col * 240, y: Y0 + (cols.size ? [...cols.values()].reduce((m, c) => Math.max(m, c.length), 0) * DY + 80 : 0) + row * 150 }
  })
}
