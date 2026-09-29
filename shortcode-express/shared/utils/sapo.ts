// ---------------------------------------------------------------------------
// Sapo blueprint bridge — converts the ShortCodeExpress canvas graph into the
// exact Sapo DSL workflow blueprint JSON that runs on the Sapo VM
// (schemas/workflow.schema.json / sapo::parser::WorkflowParser), and back.
// ---------------------------------------------------------------------------
import type {
  ChoiceCase, FlowEdge, FlowGraph, FlowNode, KVPair, MenuOption, NodeKind,
  SapoBlueprint, SapoNode, ValidationIssue, ValidationResult,
} from '../types'

// --- Palette -----------------------------------------------------------------

export interface PaletteDef {
  kind: NodeKind
  name: string
  sapoType: string
  group: 'USSD interaction' | 'Integrations' | 'Logic' | 'Data' | 'Flow control' | 'End'
  icon: string
  color: string // tailwind classes for node chrome
  hint: string
  defaults: () => Record<string, unknown>
  outputs: (node: FlowNode) => { handle: string; label: string; kind: 'main' | 'alt' | 'error' }[]
}

export function uid(prefix = 'n'): string {
  return `${prefix}_${Math.random().toString(36).slice(2, 8)}`
}

export function slugId(name: string): string {
  const s = name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '')
  return s || 'node'
}

export function uniqueId(base: string, taken: Set<string>): string {
  let id = slugId(base)
  if (!taken.has(id)) { taken.add(id); return id }
  let i = 2
  while (taken.has(`${id}_${i}`)) i++
  id = `${id}_${i}`
  taken.add(id)
  return id
}

/**
 * System-assigned node reference: a 6-digit number, unique within the flow.
 * Shown to the user read-only — labels are the human-facing name.
 */
export function nodeRefId(taken: Set<string>): string {
  let id = String(100000 + Math.floor(Math.random() * 900000))
  while (taken.has(id)) id = String(100000 + Math.floor(Math.random() * 900000))
  taken.add(id)
  return id
}

function kv(pairs?: KVPair[]): Record<string, string> {
  const out: Record<string, string> = {}
  for (const p of pairs ?? []) if (p.key.trim()) out[p.key.trim()] = p.value
  return out
}

function parseJsonOr(raw: string | undefined, fallback: unknown): unknown {
  if (!raw || !raw.trim()) return fallback
  try { return JSON.parse(raw) } catch { return fallback }
}

export function menuAutoMessage(options: MenuOption[]): string {
  return options.map((o, i) => `${o.value || i + 1}. ${o.label}`).join('\n')
}

export function autoValidation(options: MenuOption[]): string {
  const values = options.map((o, i) => (o.value || String(i + 1)).replace(/'/g, ''))
  return `matches(input, '^[${values.join('')}]$')`
}

// --- Palette definitions ------------------------------------------------------

export const PALETTE: PaletteDef[] = [
  {
    kind: 'menu', name: 'Menu', sapoType: 'action', group: 'USSD interaction', icon: 'list', color: 'violet',
    hint: 'Numbered USSD menu with option routing (action + next_tasks)',
    defaults: () => ({
      label: 'Main menu', inputVariable: 'menu_choice', autoMessage: true, message: '',
      options: [
        { id: uid('o'), label: 'Option one', value: '1' },
        { id: uid('o'), label: 'Option two', value: '2' },
      ],
      validationMode: 'off', timeout: '2m',
    }),
    outputs: (node) => [
      ...(node.data.options ?? []).map((o, i) => ({ handle: `option:${o.id}`, label: o.value || String(i + 1), kind: 'alt' as const })),
      { handle: 'next', label: 'no match', kind: 'main' as const },
    ],
  },
  {
    kind: 'input', name: 'Ask input', sapoType: 'action', group: 'USSD interaction', icon: 'keyboard', color: 'violet',
    hint: 'Prompt the caller to type a reply (action)',
    defaults: () => ({ label: 'Ask for input', inputVariable: 'user_input', message: 'Enter your name:', validationMode: 'off', customValidation: '', timeout: '2m' }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'pin', name: 'Ask PIN', sapoType: 'action', group: 'USSD interaction', icon: 'lock', color: 'violet',
    hint: 'Secure PIN prompt with validation (action)',
    defaults: () => ({ label: 'Verify PIN', inputVariable: 'pin', message: 'Enter your 4-digit PIN', validationMode: 'custom', customValidation: "matches(input, '^[0-9]{4}$')", timeout: '2m' }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'display', name: 'Show message', sapoType: 'action', group: 'USSD interaction', icon: 'chat', color: 'violet',
    hint: 'Display a message and continue (no input)',
    defaults: () => ({ label: 'Show message', message: 'Please wait...' }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }],
  },
  {
    kind: 'http', name: 'HTTP API', sapoType: 'command', group: 'Integrations', icon: 'globe', color: 'sky',
    hint: 'Call a REST API (command http.get/post/…)',
    defaults: () => ({
      label: 'Call API', verb: 'post', url: 'https://api.example.com/v1/resource',
      headers: [], queryParams: [], bodyJson: '', retryEnabled: true,
      retry: { max_attempts: 3, backoff_ms: 250, multiplier: 2, jitter: 0.2 },
      outputs: [{ id: uid('m'), key: 'api_response', value: '$.body' }],
    }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'subflow', name: 'Sub-workflow', sapoType: 'subflow', group: 'Integrations', icon: 'cube', color: 'sky',
    hint: 'Run another registered blueprint (subflow)',
    defaults: () => ({ label: 'Run sub-flow', workflow: 'examples.hello_world', inputsJson: '', returnJson: '' }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'event', name: 'Emit event', sapoType: 'event', group: 'Integrations', icon: 'bolt', color: 'sky',
    hint: 'Publish an event on the bus (event)',
    defaults: () => ({ label: 'Publish event', eventName: 'payments.initiated', payloadJson: '' }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }],
  },
  {
    kind: 'if', name: 'Condition', sapoType: 'if', group: 'Logic', icon: 'branch', color: 'amber',
    hint: 'Branch on a SEL predicate (if / then / else)',
    defaults: () => ({ label: 'Check condition', condition: 'amount > 0' }),
    outputs: () => [{ handle: 'then', label: 'then', kind: 'main' }, { handle: 'else', label: 'else', kind: 'alt' }],
  },
  {
    kind: 'choice', name: 'Router', sapoType: 'choice', group: 'Logic', icon: 'shuffle', color: 'amber',
    hint: 'Route on a value (choice cases + default)',
    defaults: () => ({
      label: 'Route on value', expression: '$menu_choice',
      cases: [
        { id: uid('c'), label: 'First', value: '1' },
        { id: uid('c'), label: 'Second', value: '2' },
      ],
    }),
    outputs: (node) => [
      ...(node.data.cases ?? []).map((c) => ({ handle: `case:${c.id}`, label: c.value, kind: 'alt' as const })),
      { handle: 'default', label: 'default', kind: 'main' as const },
    ],
  },
  {
    kind: 'try', name: 'Try / Catch', sapoType: 'try', group: 'Logic', icon: 'shield', color: 'amber',
    hint: 'Run a guarded body with an error handler (try)',
    defaults: () => ({ label: 'Guard the call', errorVariable: 'error' }),
    outputs: () => [
      { handle: 'body', label: 'body', kind: 'main' },
      { handle: 'catch', label: 'catch', kind: 'error' },
      { handle: 'next', label: 'after', kind: 'alt' },
    ],
  },
  {
    kind: 'script', name: 'Script', sapoType: 'script', group: 'Logic', icon: 'code', color: 'amber',
    hint: 'Inline SEL program (script)',
    defaults: () => ({ label: 'Compute values', scriptCode: 'total = to_number(price) * to_number(qty)', bindingsJson: '' }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'assign', name: 'Set variables', sapoType: 'noop', group: 'Data', icon: 'variable', color: 'emerald',
    hint: 'Write context variables (noop assign)',
    defaults: () => ({ label: 'Set variables', assignments: [{ id: uid('a'), key: 'currency', value: 'GHS' }] }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }],
  },
  {
    kind: 'transform', name: 'Transform', sapoType: 'transform', group: 'Data', icon: 'filter', color: 'emerald',
    hint: 'Map/filter/project a collection (transform)',
    defaults: () => ({ label: 'Transform data', operation: 'map', inputExpr: '$rows', mappingJson: '', where: '', outputKey: 'result', asArray: false }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'query', name: 'Query data', sapoType: 'query', group: 'Data', icon: 'database', color: 'emerald',
    hint: 'Read from a data source (query)',
    defaults: () => ({ label: 'Query data source', source: 'profiles', filter: '', queryStatement: '', paramsJson: '', limit: 0, offset: 0, outputKey: 'rows' }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'wait', name: 'Wait', sapoType: 'wait', group: 'Flow control', icon: 'clock', color: 'slate',
    hint: 'Pause the session (wait)',
    defaults: () => ({ label: 'Wait before retry', duration: '10m', until: '', pollInterval: '' }),
    outputs: () => [{ handle: 'next', label: 'next', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'loop', name: 'Loop', sapoType: 'loop', group: 'Flow control', icon: 'repeat', color: 'slate',
    hint: 'Iterate a collection or count (loop)',
    defaults: () => ({ label: 'Loop over items', loopMode: 'collection', collection: '$items', count: '', while: '', iterator: 'item', indexVar: 'index', maxIterations: 1000, onItemError: 'fail' }),
    outputs: () => [{ handle: 'body', label: 'body', kind: 'main' }, { handle: 'next', label: 'after', kind: 'alt' }],
  },
  {
    kind: 'break', name: 'Break / Continue', sapoType: 'break', group: 'Flow control', icon: 'exit', color: 'slate',
    hint: 'Control loop execution (break/continue)',
    defaults: () => ({ label: 'Exit loop', breakAction: 'break' }),
    outputs: () => [],
  },
  {
    kind: 'parallel', name: 'Parallel', sapoType: 'parallel', group: 'Flow control', icon: 'columns', color: 'slate',
    hint: 'Run branches concurrently (parallel)',
    defaults: () => ({ label: 'Fan out', mergePolicy: 'wait_all', failFast: false }),
    outputs: (node) => [0, 1].map((i) => ({ handle: `branch:${i}`, label: `branch ${i + 1}`, kind: (i === 0 ? 'main' : 'alt') as 'main' | 'alt' })),
  },
  {
    kind: 'schedule', name: 'Schedule', sapoType: 'schedule', group: 'Flow control', icon: 'calendar', color: 'slate',
    hint: 'Cron-triggered workflow (schedule)',
    defaults: () => ({ label: 'Nightly job', cron: '0 2 * * *', timezone: 'UTC', workflow: '', jobId: '' }),
    outputs: () => [],
  },
  {
    kind: 'await_event', name: 'Await event', sapoType: 'action', group: 'Flow control', icon: 'radio', color: 'violet',
    hint: 'Park the session until an event arrives (action on_event)',
    defaults: () => ({ label: 'Wait for callback', eventName: 'payments.confirmed', triggerCondition: '', timeout: '30m' }),
    outputs: () => [{ handle: 'next', label: 'handler', kind: 'main' }, { handle: 'error', label: 'error', kind: 'error' }],
  },
  {
    kind: 'end_success', name: 'End — success', sapoType: 'terminate', group: 'End', icon: 'check', color: 'rose',
    hint: 'Terminate the session as completed (terminate)',
    defaults: () => ({ label: 'Done', endMessage: 'Thank you for using our service.', endOutputs: [] }),
    outputs: () => [],
  },
  {
    kind: 'end_failure', name: 'End — failure', sapoType: 'terminate', group: 'End', icon: 'x', color: 'rose',
    hint: 'Terminate the session as failed (terminate)',
    defaults: () => ({ label: 'Failed', endMessage: 'Sorry, something went wrong.', endOutputs: [] }),
    outputs: () => [],
  },
]

export const paletteByKind: Record<string, PaletteDef> = Object.fromEntries(PALETTE.map((p) => [p.kind, p]))

// --- Graph -> Blueprint --------------------------------------------------------

function targetsOf(edges: FlowEdge[], source: string, handle: string): string[] {
  return edges.filter((e) => e.source === source && e.sourceHandle === handle).map((e) => e.target)
}

function firstTarget(edges: FlowEdge[], source: string, handle: string): string | undefined {
  return targetsOf(edges, source, handle)[0]
}

function edgesFrom(edges: FlowEdge[], source: string, handlePrefix: string): FlowEdge[] {
  return edges.filter((e) => e.source === source && e.sourceHandle.startsWith(handlePrefix))
}

/** Serialize the canvas graph into an exact Sapo DSL blueprint. */
export function graphToBlueprint(graph: FlowGraph, meta: { name: string; description: string; version: string; defaults?: Record<string, unknown> }): SapoBlueprint {
  const nodesById = new Map(graph.nodes.map((n) => [n.id, n]))
  const entry = graph.entryId && nodesById.has(graph.entryId) ? graph.entryId : graph.nodes[0]?.id
  const ordered = [...graph.nodes].sort((a, b) => (a.id === entry ? -1 : b.id === entry ? 1 : 0))

  // --- Frame extraction -------------------------------------------------
  // The Sapo VM runs loop/try/parallel bodies as frames of node ids. A body
  // node without `next` steps to its sibling; an explicit `next` pointing
  // outside the frame escapes it (goto-out semantics). We therefore collect
  // each structural node's body chain from the canvas and let members omit
  // `next` when it merely continues the chain.
  const structuralKinds = new Set(['loop', 'try', 'parallel', 'if', 'choice', 'menu'])
  const structuralHandlePrefixes = ['body:', 'catch:', 'finally:', 'branch:', 'option:', 'case:', 'then', 'else', 'default']
  const hasIncomingStructuralEdge = new Set<string>()
  for (const e of graph.edges) {
    if (structuralHandlePrefixes.some((p) => e.sourceHandle.startsWith(p))) hasIncomingStructuralEdge.add(e.target)
  }
  const isStructuralTarget = (id: string) =>
    structuralKinds.has(nodesById.get(id)?.kind ?? '') || hasIncomingStructuralEdge.has(id)

  const chains = new Map<string, { body: string[]; catch: string[]; finally: string[]; branches: string[][] }>()
  const memberOf = new Map<string, { owner: string; chain: string[] }>()

  const extractChain = (start: string | undefined, stops: Set<string>): string[] => {
    const chain: string[] = []
    const seen = new Set<string>()
    let cur = start
    while (cur && cur !== undefined && !stops.has(cur) && !seen.has(cur) && !isStructuralTarget(cur) && nodesById.has(cur)) {
      chain.push(cur)
      seen.add(cur)
      cur = firstTarget(graph.edges, cur, 'next')
    }
    for (const id of chain) if (!memberOf.has(id)) memberOf.set(id, { owner: '', chain })
    return chain
  }

  const register = (owner: string, chain: string[]) => {
    for (const id of chain) memberOf.set(id, { owner, chain })
  }

  for (const node of graph.nodes) {
    if (node.kind === 'loop' || node.kind === 'try' || node.kind === 'parallel' || node.kind === 'if') {
      const after = firstTarget(graph.edges, node.id, 'next')
      const stops = new Set<string>([node.id, ...(after ? [after] : [])])
      const entry4: { body: string[]; catch: string[]; finally: string[]; branches: string[][] } = { body: [], catch: [], finally: [], branches: [] }
      if (node.kind === 'loop') {
        entry4.body = extractChain(firstTarget(graph.edges, node.id, 'body'), stops)
        register(node.id, entry4.body)
      } else if (node.kind === 'try') {
        entry4.body = extractChain(firstTarget(graph.edges, node.id, 'body'), stops)
        entry4.catch = extractChain(firstTarget(graph.edges, node.id, 'catch'), stops)
        entry4.finally = extractChain(firstTarget(graph.edges, node.id, 'finally'), stops)
        register(node.id, entry4.body)
        register(node.id, entry4.catch)
        register(node.id, entry4.finally)
      } else if (node.kind === 'if') {
        // With an `after` target the then/else branches become body frames
        // that resume at `next` once they complete — matching the engine.
        entry4.body = extractChain(firstTarget(graph.edges, node.id, 'then'), stops)
        entry4.catch = extractChain(firstTarget(graph.edges, node.id, 'else'), stops)
        register(node.id, entry4.body)
        register(node.id, entry4.catch)
      } else {
        const branchEdges = edgesFrom(graph.edges, node.id, 'branch:')
        const idxs = [...new Set(branchEdges.map((e) => parseInt(e.sourceHandle.split(':')[1] ?? '0', 10) || 0))].sort((a, b) => a - b)
        entry4.branches = idxs.map((i) => extractChain(firstTarget(graph.edges, node.id, `branch:${i}`), stops))
        for (const b of entry4.branches) register(node.id, b)
      }
      chains.set(node.id, entry4)
    }
  }

  // `next` handling: omit when it just continues a body chain (implicit
  // sibling stepping); keep it when it jumps within a body or escapes it.
  const nextFor = (node: FlowNode): string | undefined => {
    const t = firstTarget(graph.edges, node.id, 'next')
    if (!t) return undefined
    const m = memberOf.get(node.id)
    if (m) {
      const i = m.chain.indexOf(node.id)
      if (i >= 0 && i + 1 < m.chain.length && m.chain[i + 1] === t) return undefined // implicit sibling
      if (t === m.owner) return undefined // jumping back to the structural owner: the engine iterates itself
    }
    return t
  }

  const bp: SapoBlueprint = {
    name: meta.name || 'sapo.workflow',
    version: meta.version || '1.0',
    description: meta.description || '',
    nodes: [],
  }
  if (meta.defaults && Object.keys(meta.defaults).length) bp.defaults = meta.defaults

  for (const node of ordered) {
    const sapo = serializeNode(node, graph.edges, nextFor(node), chains.get(node.id))
    if (sapo) bp.nodes.push(sapo)
  }
  return bp
}

function cleanUndefined(obj: Record<string, unknown>) {
  for (const k of Object.keys(obj)) {
    if (obj[k] === undefined) delete obj[k]
    else if (obj[k] && typeof obj[k] === 'object' && !Array.isArray(obj[k])) cleanUndefined(obj[k] as Record<string, unknown>)
  }
  return obj
}

function serializeNode(node: FlowNode, edges: FlowEdge[], next: string | undefined, chains?: { body: string[]; catch: string[]; finally: string[]; branches: string[][] }): SapoNode | null {
  const d = node.data ?? ({} as FlowNode['data'])
  const id = node.id
  const onError = firstTarget(edges, id, 'error')
  const label = d.label?.trim() || undefined
  const base: SapoNode = { id, type: '', ...(label ? { label } : {}) }

  switch (node.kind) {
    case 'menu': {
      const options = d.options ?? []
      const message = d.autoMessage !== false ? menuAutoMessage(options) : (d.message ?? '')
      const prompt: SapoNode = {
        message,
        interaction_type: 'menu',
        ...(d.timeout ? { timeout: d.timeout } : {}),
      }
      if (d.validationMode === 'custom' && d.customValidation?.trim()) prompt.input_validation = d.customValidation.trim()
      else if (d.validationMode === 'auto' && options.length) prompt.input_validation = autoValidation(options)

      const nextTasks = (d.options ?? [])
        .map((opt, i) => {
          const target = firstTarget(edges, id, `option:${opt.id}`)
          if (!target) return null
          const value = (opt.value || String(i + 1)).replace(/'/g, '')
          return { task_id: target, execute_condition: `${d.inputVariable || 'input'} == '${value}'` }
        })
        .filter(Boolean)

      return cleanUndefined({
        ...base,
        type: 'action',
        capability: '',
        await_input: true,
        input_variable: d.inputVariable || 'input',
        prompt_config: prompt,
        ...(nextTasks.length ? { next_tasks: nextTasks } : {}),
        ...(next ? { next } : {}),
        ...(onError ? { on_error: onError } : {}),
      })
    }
    case 'input':
    case 'pin': {
      const prompt: SapoNode = {
        message: d.message ?? '',
        interaction_type: node.kind === 'pin' ? 'pin' : 'input',
        ...(d.timeout ? { timeout: d.timeout } : {}),
      }
      if (d.validationMode === 'custom' && d.customValidation?.trim()) prompt.input_validation = d.customValidation.trim()
      return cleanUndefined({
        ...base, type: 'action', capability: '', await_input: true,
        input_variable: d.inputVariable || 'input', prompt_config: prompt,
        ...(next ? { next } : {}), ...(onError ? { on_error: onError } : {}),
      })
    }
    case 'display':
      return cleanUndefined({
        ...base, type: 'action', capability: '', await_input: false,
        prompt_config: { message: d.message ?? '', interaction_type: 'display' },
        ...(next ? { next } : {}),
      })
    case 'await_event': {
      const handler = next
      return cleanUndefined({
        ...base, type: 'action', capability: '',
        on_event: {
          event_name: d.eventName || 'external.event',
          ...(d.triggerCondition?.trim() ? { trigger_condition: d.triggerCondition.trim() } : {}),
          ...(handler ? { handler } : {}),
        },
        ...(d.timeout ? { prompt_config: { message: `Waiting for ${d.eventName}`, timeout: d.timeout } } : {}),
        ...(onError ? { on_error: onError } : {}),
      })
    }
    case 'http': {
      const verb = (d.verb || 'post').toUpperCase()
      const http: SapoNode = { url: d.url ?? '', method: verb }
      const headers = kv(d.headers)
      if (Object.keys(headers).length) http.headers = headers
      const query = kv(d.queryParams)
      if (Object.keys(query).length) http.query = query
      const body = parseJsonOr(d.bodyJson, undefined as unknown as Record<string, unknown>)
      if (body !== undefined) http.body = body as SapoNode
      if (d.timeout) http.timeout = d.timeout
      const outputs = kv(d.outputs)
      const out: SapoNode = { type: 'command', command: `http.${(d.verb || 'post').toLowerCase()}`, http_request: http }
      if (Object.keys(outputs).length) out.output = outputs
      return cleanUndefined({
        ...base, ...out,
        ...(d.retryEnabled && d.retry ? { retry: d.retry } : {}),
        ...(next ? { next } : {}), ...(onError ? { on_error: onError } : {}),
      })
    }
    case 'subflow': {
      const inputs = parseJsonOr(d.inputsJson, {}) as Record<string, unknown>
      const ret = parseJsonOr(d.returnJson, {}) as Record<string, unknown>
      return cleanUndefined({
        ...base, type: 'subflow', workflow: d.workflow || 'examples.hello_world',
        ...(Object.keys(inputs).length ? { inputs } : {}),
        ...(Object.keys(ret).length ? { return: ret } : {}),
        ...(next ? { next } : {}), ...(onError ? { on_error: onError } : {}),
      })
    }
    case 'event': {
      const payload = parseJsonOr(d.payloadJson, {}) as Record<string, unknown>
      return cleanUndefined({
        ...base, type: 'event', event: d.eventName || 'domain.event',
        ...(Object.keys(payload).length ? { payload } : {}),
        ...(next ? { next } : {}),
      })
    }
    case 'if': {
      const c = chains ?? { body: [], catch: [], finally: [], branches: [] }
      const thenT = firstTarget(edges, id, 'then')
      const elseT = firstTarget(edges, id, 'else')
      // With an after-branch target, then/else become body id arrays whose
      // frame resumes at `next`; otherwise they are plain jump refs.
      const emitBranch = (target: string | undefined, chain: string[]): string | string[] | undefined => {
        if (next) return chain.length ? chain : (target ? [target] : undefined)
        return target
      }
      const thenOut = emitBranch(thenT, c.body)
      const elseOut = emitBranch(elseT, c.catch)
      return cleanUndefined({
        ...base, type: 'if', condition: d.condition || 'true',
        ...(thenOut !== undefined ? { then: thenOut } : {}), ...(elseOut !== undefined ? { else: elseOut } : {}),
        ...(next ? { next } : {}),
      })
    }
    case 'choice': {
      const cases: Record<string, string> = {}
      for (const c of d.cases ?? []) {
        const t = firstTarget(edges, id, `case:${c.id}`)
        if (t) cases[c.value] = t
      }
      const def = firstTarget(edges, id, 'default')
      return cleanUndefined({
        ...base, type: 'choice', expression: d.expression || '$input',
        ...(Object.keys(cases).length ? { cases } : {}),
        ...(def ? { default: def } : {}),
      })
    }
    case 'try': {
      const c = chains ?? { body: [], catch: [], finally: [], branches: [] }
      const bodyT = firstTarget(edges, id, 'body')
      const catchT = firstTarget(edges, id, 'catch')
      const finallyT = firstTarget(edges, id, 'finally')
      if (!bodyT && !c.body.length) return null
      const catchObj: SapoNode = { as: d.errorVariable || 'error' }
      if (c.catch.length) catchObj.body = c.catch
      else if (catchT) catchObj.target = catchT
      return cleanUndefined({
        ...base, type: 'try',
        body: c.body.length ? c.body : [bodyT],
        ...(catchT || c.catch.length ? { catch: catchObj } : {}),
        ...(c.finally.length || finallyT ? { finally: c.finally.length ? c.finally : [finallyT] } : {}),
        ...(next ? { next } : {}),
      })
    }
    case 'loop': {
      const c = chains ?? { body: [], catch: [], finally: [], branches: [] }
      const bodyT = firstTarget(edges, id, 'body')
      if (!bodyT && !c.body.length) return null
      const spec: SapoNode = {
        type: 'loop',
        iterator: d.iterator || 'item',
        index: d.indexVar || 'index',
        max_iterations: d.maxIterations ?? 1000,
        on_item_error: d.onItemError || 'fail',
        body: c.body.length ? c.body : [bodyT!],
      }
      if (d.loopMode === 'collection' && d.collection) spec.collection = d.collection
      else if (d.loopMode === 'count' && d.count) spec.count = d.count
      else if (d.loopMode === 'while' && d.while) spec.while = d.while
      else spec.collection = '$items'
      return cleanUndefined({ ...base, ...spec, ...(next ? { next } : {}) })
    }
    case 'parallel': {
      const c = chains ?? { body: [], catch: [], finally: [], branches: [] }
      const childTasks = (c.branches.length ? c.branches : [[id + '_branch']]) as string[][]
      return cleanUndefined({
        ...base, type: 'parallel',
        child_tasks: childTasks,
        merge_policy: d.mergePolicy || 'wait_all',
        ...(d.failFast !== undefined ? { fail_fast: d.failFast } : {}),
        ...(next ? { next } : {}),
      })
    }
    case 'script': {
      const bindings = parseJsonOr(d.bindingsJson, {}) as Record<string, unknown>
      return cleanUndefined({
        ...base, type: 'script', code: d.scriptCode || '',
        ...(Object.keys(bindings).length ? { bindings } : {}),
        ...(next ? { next } : {}), ...(onError ? { on_error: onError } : {}),
      })
    }
    case 'break':
      return cleanUndefined({ ...base, type: d.breakAction === 'continue' ? 'continue' : 'break' })
    case 'assign': {
      const assign: Record<string, unknown> = {}
      for (const a of d.assignments ?? []) if (a.key.trim()) assign[a.key.trim()] = parseValue(a.value)
      return cleanUndefined({ ...base, type: 'noop', ...(Object.keys(assign).length ? { assign } : {}), ...(next ? { next } : {}) })
    }
    case 'transform':
      return cleanUndefined({
        ...base, type: 'transform', operation: d.operation || 'assign',
        ...(d.inputExpr ? { input: d.inputExpr } : {}),
        output: d.outputKey || 'result',
        ...((d.mappingJson ?? '').trim() ? { mapping: parseJsonOr(d.mappingJson, {}) } : {}),
        ...(d.where?.trim() ? { where: d.where.trim() } : {}),
        ...(d.asArray ? { as_array: true } : {}),
        ...(next ? { next } : {}), ...(onError ? { on_error: onError } : {}),
      })
    case 'query':
      return cleanUndefined({
        ...base, type: 'query', source: d.source || 'data', output: d.outputKey || 'rows',
        ...(d.filter?.trim() ? { filter: d.filter.trim() } : {}),
        ...(d.queryStatement?.trim() ? { query: d.queryStatement.trim() } : {}),
        ...((d.paramsJson ?? '').trim() ? { parameters: parseJsonOr(d.paramsJson, {}) } : {}),
        ...(d.limit ? { limit: d.limit } : {}), ...(d.offset ? { offset: d.offset } : {}),
        ...(next ? { next } : {}), ...(onError ? { on_error: onError } : {}),
      })
    case 'wait':
      return cleanUndefined({
        ...base, type: 'wait', duration: d.duration || '10m',
        ...(d.until?.trim() ? { until: d.until.trim() } : {}),
        ...(d.pollInterval?.trim() ? { poll_interval: d.pollInterval.trim() } : {}),
        ...(next ? { next } : {}), ...(onError ? { on_error: onError } : {}),
      })
    case 'schedule':
      return cleanUndefined({
        ...base, type: 'schedule', cron: d.cron || '0 2 * * *',
        ...(d.timezone ? { timezone: d.timezone } : {}),
        ...(d.workflow ? { workflow: d.workflow } : { body: next ?? id + '_job' }),
        ...(d.jobId ? { job_id: d.jobId } : {}),
      })
    case 'end_success':
    case 'end_failure': {
      const output: Record<string, unknown> = {}
      for (const o of d.endOutputs ?? []) if (o.key.trim()) output[o.key.trim()] = parseValue(o.value)
      return cleanUndefined({
        ...base, type: 'terminate', status: node.kind === 'end_success' ? 'success' : 'failure',
        ...(d.endMessage ? { message: d.endMessage } : {}),
        ...(Object.keys(output).length ? { output } : {}),
      })
    }
    default:
      return null
  }
}

/** Values in assign/output can be templates, numbers, booleans, null or JSON. */
export function parseValue(raw: string): unknown {
  const s = (raw ?? '').trim()
  if (s === '') return ''
  if (s === 'true') return true
  if (s === 'false') return false
  if (s === 'null') return null
  if (/^-?\d+(\.\d+)?$/.test(s)) return parseFloat(s)
  if (s.startsWith('{') || s.startsWith('[')) {
    try { return JSON.parse(s) } catch { return raw }
  }
  return raw
}

// --- Blueprint -> Graph (import / round-trip) ----------------------------------

/**
 * The parser accepts inline node objects inside loop/try/parallel/then/else
 * bodies (they get "hoisted" to the top level with the body holding their
 * ids). The canvas is flat, so we hoist inline bodies the same way before
 * importing: every inline object becomes a top-level node and the body keeps
 * only its id.
 */
export function flattenBodies(bp: SapoBlueprint): SapoBlueprint {
  const out: SapoNode[] = []
  const queue: SapoNode[] = [...(bp.nodes ?? [])]
  const replaceInline = (list: unknown): string[] => {
    if (!Array.isArray(list)) return []
    const ids: string[] = []
    for (const item of list) {
      if (typeof item === 'string' && item) ids.push(item)
      else if (item && typeof item === 'object' && !Array.isArray(item)) {
        queue.push(item as SapoNode)
        const cid = String((item as SapoNode).id ?? '')
        if (cid) ids.push(cid)
      }
    }
    return ids
  }
  const seen = new Set<string>()
  while (queue.length) {
    const n = queue.shift()!
    const nid = String(n.id ?? '')
    if (seen.has(nid)) continue
    seen.add(nid)
    for (const key of ['body', 'then', 'else', 'finally', 'handler'] as const) {
      if (Array.isArray(n[key])) (n as Record<string, unknown>)[key] = replaceInline(n[key])
    }
    const c = n.catch
    if (Array.isArray(c)) (n as Record<string, unknown>).catch = { body: replaceInline(c) }
    else if (c && typeof c === 'object' && Array.isArray((c as Record<string, unknown>).body)) {
      ;(c as Record<string, unknown>).body = replaceInline((c as Record<string, unknown>).body)
    }
    if (Array.isArray(n.child_tasks)) {
      ;(n as Record<string, unknown>).child_tasks = (n.child_tasks as unknown[]).map((branch) =>
        Array.isArray(branch) ? replaceInline(branch) : branch,
      )
    }
    out.push(n)
  }
  return { ...bp, nodes: out }
}

interface PendingTarget { fromId: string; handle: string; target: string }

export function blueprintToGraph(bpIn: SapoBlueprint): FlowGraph {
  const bp = flattenBodies(bpIn)
  const nodes: FlowNode[] = []
  const edges: FlowEdge[] = []
  const pending: PendingTarget[] = []
  const taken = new Set<string>()
  const pos = (i: number) => ({ x: 80 + (i % 4) * 300, y: 60 + Math.floor(i / 4) * 190 })
  let idx = 0
  let entryId: string | null = null

  const push = (id: string, kind: NodeKind, data: Record<string, unknown>) => {
    const node: FlowNode = { id, kind, position: pos(idx++), data: data as FlowNode['data'] }
    nodes.push(node)
    taken.add(id)
    return node
  }

  const link = (fromId: string, handle: string, target: unknown) => {
    if (typeof target === 'string' && target) pending.push({ fromId, handle, target })
  }

  /** Structural bodies (loop/try/parallel) are id chains: link the owner to
   *  the first member and stitch the rest together with implicit next edges. */
  const chainLinks = (fromId: string, handle: string, ids: Array<string | SapoNode>) => {
    const list = (Array.isArray(ids) ? ids : []).map((x) => (typeof x === 'string' ? x : (x as SapoNode)?.id)).filter(Boolean) as string[]
    if (!list.length) return
    link(fromId, handle, list[0])
    for (let i = 1; i < list.length; i++) pending.push({ fromId: list[i - 1], handle: 'next', target: list[i] })
  }

  for (const raw of bp.nodes ?? []) {
    const t = String(raw.type ?? 'noop')
    const id = uniqueId(String(raw.id ?? slugId(String(raw.label ?? t))), taken)
    // keep the original id for reference mapping
    const origId = String(raw.id ?? id)
    const label = typeof raw.label === 'string' ? raw.label : ''

    switch (t) {
      case 'action': {
        const prompt = (raw.prompt_config ?? {}) as Record<string, unknown>
        const interaction = String(prompt.interaction_type ?? 'input')
        const nextTasks = (raw.next_tasks ?? []) as Array<Record<string, string>>
        if (interaction === 'menu') {
          const data: Record<string, unknown> = {
            label: label || 'Menu',
            inputVariable: String(raw.input_variable ?? 'input'),
            autoMessage: nextTasks.length > 0,
            message: String(prompt.message ?? ''),
            options: [],
            validationMode: prompt.input_validation ? 'custom' : 'off',
            customValidation: String(prompt.input_validation ?? ''),
            timeout: prompt.timeout ? String(prompt.timeout) : '2m',
          }
          const node = push(id, 'menu', data)
          const options = (data.options as MenuOption[])
          for (const [i, nt] of nextTasks.entries()) {
            const optId = uid('o')
            const cond = String(nt.execute_condition ?? '')
            const m = /==\s*'([^']*)'/.exec(cond)
            const value = m ? m[1] : String(i + 1)
            options.push({ id: optId, label: `${value}. …`, value })
            link(node.id, `option:${optId}`, nt.task_id ?? nt)
          }
          // re-derive labels from the static message when possible
          const msgLines = String(prompt.message ?? '').split('\n')
          for (const [i, opt] of options.entries()) {
            const line = msgLines.find((l) => l.trim().startsWith(`${opt.value}.`))
            opt.label = line ? line.trim().replace(/^\d+\.\s*/, '') : `${opt.value}. …`
          }
          link(node.id, 'next', raw.next)
          link(node.id, 'error', raw.on_error)
        } else if (interaction === 'display') {
          push(id, 'display', { label: label || 'Show message', message: String(prompt.message ?? '') })
          link(id, 'next', raw.next)
        } else if (raw.on_event && typeof raw.on_event === 'object') {
          const ev = raw.on_event as Record<string, string>
          push(id, 'await_event', { label: label || 'Await event', eventName: ev.event_name, triggerCondition: ev.trigger_condition ?? '', timeout: prompt.timeout ? String(prompt.timeout) : '30m' })
          link(id, 'next', ev.handler ?? raw.next)
          link(id, 'error', raw.on_error)
        } else {
          push(id, interaction === 'pin' ? 'pin' : 'input', {
            label: label || (interaction === 'pin' ? 'Ask PIN' : 'Ask input'),
            inputVariable: String(raw.input_variable ?? 'input'),
            message: String(prompt.message ?? ''),
            validationMode: prompt.input_validation ? 'custom' : 'off',
            customValidation: String(prompt.input_validation ?? ''),
            timeout: prompt.timeout ? String(prompt.timeout) : '2m',
          })
          link(id, 'next', raw.next)
          link(id, 'error', raw.on_error)
        }
        break
      }
      case 'command': {
        const http = (raw.http_request ?? {}) as Record<string, unknown>
        const verb = String(raw.command ?? 'http.post').replace('http.', '')
        const outputs = (raw.output ?? {}) as Record<string, string>
        push(id, 'http', {
          label: label || 'Call API',
          verb: (['get', 'post', 'put', 'patch', 'delete'].includes(verb) ? verb : 'post') as string,
          url: String(http.url ?? ''),
          headers: Object.entries((http.headers ?? {}) as Record<string, string>).map(([k, v]) => ({ id: uid('h'), key: k, value: v })),
          queryParams: Object.entries((http.query ?? {}) as Record<string, string>).map(([k, v]) => ({ id: uid('q'), key: k, value: v })),
          bodyJson: http.body ? JSON.stringify(http.body, null, 2) : '',
          retryEnabled: !!raw.retry,
          retry: (raw.retry ?? { max_attempts: 3, backoff_ms: 250, multiplier: 2, jitter: 0.2 }) as Record<string, number>,
          outputs: Object.entries(outputs).map(([k, v]) => ({ id: uid('m'), key: k, value: String(v) })),
          timeout: http.timeout ? String(http.timeout) : '',
        })
        link(id, 'next', raw.next)
        link(id, 'error', raw.on_error)
        break
      }
      case 'if': {
        push(id, 'if', { label: label || 'Condition', condition: String(raw.condition ?? 'true') })
        const thenV = raw.then
        if (typeof thenV === 'string') link(id, 'then', thenV)
        else if (Array.isArray(thenV) && thenV.length) chainLinks(id, 'then', thenV as string[])
        const elseV = raw.else
        if (typeof elseV === 'string') link(id, 'else', elseV)
        else if (Array.isArray(elseV) && elseV.length) chainLinks(id, 'else', elseV as string[])
        link(id, 'next', raw.next)
        break
      }
      case 'choice': {
        const cases = (raw.cases ?? {}) as Record<string, string>
        const cs: ChoiceCase[] = Object.entries(cases).map(([value, target]) => ({ id: uid('c'), label: value, value }))
        push(id, 'choice', { label: label || 'Router', expression: String(raw.expression ?? '$input'), cases: cs })
        for (const c of cs) link(id, `case:${c.id}`, cases[c.value])
        link(id, 'default', raw.default)
        break
      }
      case 'try': {
        const catchBlock = raw.catch as Record<string, unknown> | undefined
        const catchIds = Array.isArray(catchBlock?.body)
          ? (catchBlock!.body as string[])
          : catchBlock?.target ? [String(catchBlock.target)] : []
        const finallyBody = (raw.finally ?? []) as string[]
        push(id, 'try', { label: label || 'Try / Catch', errorVariable: String(catchBlock?.as ?? 'error') })
        chainLinks(id, 'body', (raw.body ?? []) as string[])
        chainLinks(id, 'catch', catchIds)
        chainLinks(id, 'finally', finallyBody)
        link(id, 'next', raw.next)
        break
      }
      case 'loop': {
        push(id, 'loop', {
          label: label || 'Loop',
          loopMode: raw.collection ? 'collection' : raw.count ? 'count' : 'while',
          collection: String(raw.collection ?? ''), count: raw.count !== undefined ? String(raw.count) : '', while: String(raw.while ?? ''),
          iterator: String(raw.iterator ?? 'item'), indexVar: String(raw.index ?? 'index'),
          maxIterations: Number(raw.max_iterations ?? 1000), onItemError: (raw.on_item_error as 'fail' | 'continue') ?? 'fail',
        })
        chainLinks(id, 'body', (raw.body ?? []) as string[])
        link(id, 'next', raw.next)
        break
      }
      case 'parallel': {
        push(id, 'parallel', { label: label || 'Parallel', mergePolicy: (raw.merge_policy as string) ?? 'wait_all', failFast: !!raw.fail_fast })
        const childTasks = (raw.child_tasks ?? []) as Array<Array<string | SapoNode>>
        childTasks.forEach((branch, bi) => chainLinks(id, `branch:${bi}`, branch))
        link(id, 'next', raw.next)
        break
      }
      case 'script': {
        push(id, 'script', {
          label: label || 'Script', scriptCode: String(raw.code ?? ''),
          bindingsJson: raw.bindings ? JSON.stringify(raw.bindings, null, 2) : '',
        })
        link(id, 'next', raw.next)
        link(id, 'error', raw.on_error)
        break
      }
      case 'noop': {
        const assign = (raw.assign ?? {}) as Record<string, unknown>
        push(id, 'assign', {
          label: label || 'Set variables',
          assignments: Object.entries(assign).map(([k, v]) => ({ id: uid('a'), key: k, value: typeof v === 'string' ? v : JSON.stringify(v) })),
        })
        link(id, 'next', raw.next)
        break
      }
      case 'transform': {
        push(id, 'transform', {
          label: label || 'Transform', operation: String(raw.operation ?? 'assign'), inputExpr: String(raw.input ?? ''),
          mappingJson: raw.mapping ? JSON.stringify(raw.mapping, null, 2) : '', where: String(raw.where ?? ''),
          outputKey: String(raw.output ?? 'result'), asArray: !!raw.as_array,
        })
        link(id, 'next', raw.next)
        link(id, 'error', raw.on_error)
        break
      }
      case 'query': {
        push(id, 'query', {
          label: label || 'Query data', source: String(raw.source ?? 'data'), filter: typeof raw.filter === 'string' ? raw.filter : '',
          queryStatement: String(raw.query ?? ''), paramsJson: raw.parameters ? JSON.stringify(raw.parameters, null, 2) : '',
          limit: Number(raw.limit ?? 0), offset: Number(raw.offset ?? 0), outputKey: String(raw.output ?? 'rows'),
        })
        link(id, 'next', raw.next)
        link(id, 'error', raw.on_error)
        break
      }
      case 'wait': {
        push(id, 'wait', { label: label || 'Wait', duration: String(raw.duration ?? '10m'), until: String(raw.until ?? ''), pollInterval: raw.poll_interval ? String(raw.poll_interval) : '' })
        link(id, 'next', raw.next)
        link(id, 'error', raw.on_error)
        break
      }
      case 'schedule': {
        push(id, 'schedule', { label: label || 'Schedule', cron: String(raw.cron ?? '0 2 * * *'), timezone: String(raw.timezone ?? 'UTC'), workflow: String(raw.workflow ?? ''), jobId: String(raw.job_id ?? '') })
        break
      }
      case 'event': {
        push(id, 'event', { label: label || 'Emit event', eventName: String(raw.event ?? raw.name ?? 'domain.event'), payloadJson: raw.payload ? JSON.stringify(raw.payload, null, 2) : '' })
        link(id, 'next', raw.next)
        break
      }
      case 'terminate': {
        const output = (raw.output ?? {}) as Record<string, unknown>
        push(id, raw.status === 'failure' ? 'end_failure' : 'end_success', {
          label: label || (raw.status === 'failure' ? 'Failed' : 'Done'),
          endMessage: String(raw.message ?? ''),
          endOutputs: Object.entries(output).map(([k, v]) => ({ id: uid('o'), key: k, value: typeof v === 'string' ? v : JSON.stringify(v) })),
        })
        break
      }
      case 'break':
      case 'continue':
        push(id, 'break', { label: label || 'Loop control', breakAction: t })
        break
      default: {
        // subflow / condition / switch / loop_control etc. -> best effort
        push(id, 'subflow', { label: label || t, workflow: String(raw.workflow ?? ''), inputsJson: raw.inputs ? JSON.stringify(raw.inputs, null, 2) : '', returnJson: raw.return ? JSON.stringify(raw.return, null, 2) : '' })
        link(id, 'next', raw.next)
        link(id, 'error', raw.on_error)
        break
      }
    }
    if (entryId === null) entryId = id
    // map original ids for later link resolution
    ;(nodes[nodes.length - 1] as FlowNode & { _orig?: string })._orig = origId
  }

  // resolve pending links through original-id map
  const byOrig = new Map<string, string>()
  for (const n of nodes as (FlowNode & { _orig?: string })[]) if (n._orig) byOrig.set(n._orig, n.id)
  const seenEdges = new Set<string>()
  for (const p of pending) {
    const targetId = byOrig.get(p.target) ?? (taken.has(p.target) ? p.target : null)
    if (!targetId) continue
    const key = `${p.fromId}|${p.handle}|${targetId}`
    if (seenEdges.has(key)) continue
    seenEdges.add(key)
    edges.push({ id: uid('e'), source: p.fromId, sourceHandle: p.handle, target: targetId })
  }
  // clean temp key
  for (const n of nodes as (FlowNode & { _orig?: string })[]) delete n._orig

  return { entryId: nodes[0]?.id ?? null, nodes, edges }
}

// --- Validation (mirrors sapo::parser::BlueprintValidator) ----------------------

const TERMINAL_OK = new Set(['terminate', 'break', 'continue', 'schedule', 'event', 'parallel'])

export function validateBlueprint(bp: SapoBlueprint, graph?: FlowGraph): ValidationResult {
  const errors: ValidationIssue[] = []
  const warnings: ValidationIssue[] = []
  const nodeIds = new Set<string>()
  const nodes = bp.nodes ?? []

  if (!nodes.length) errors.push({ level: 'error', message: 'A blueprint needs at least one node.' })
  if (!bp.name || !bp.name.trim()) warnings.push({ level: 'warning', message: "Blueprint has no 'name' — the engine will default to sapo.workflow." })

  for (const n of nodes) {
    const id = String(n.id ?? '')
    const type = String(n.type ?? '')
    if (!id) errors.push({ level: 'error', nodeId: id, message: `A ${type || 'node'} is missing its id.` })
    else if (nodeIds.has(id)) errors.push({ level: 'error', nodeId: id, message: `Duplicate node id '${id}' — ids must be unique across the blueprint (hoisted children included).` })
    else nodeIds.add(id)

    switch (type) {
      case 'action': {
        const prompt = n.prompt_config as Record<string, unknown> | undefined
        const nextTasks = (n.next_tasks ?? []) as Array<Record<string, string>>
        const hasPrompt = prompt && typeof prompt.message === 'string' && (prompt.message as string).length > 0
        const inert = !n.capability && !prompt && !nextTasks.length && !n.on_event
        if (inert) errors.push({ level: 'error', nodeId: id, message: `Action '${id}' is inert: it declares no capability, prompt, event or next_tasks.` })
        if (prompt && !hasPrompt && !nextTasks.length) errors.push({ level: 'error', nodeId: id, message: `Action '${id}' has a prompt_config without a message.` })
        if (n.await_input !== false && !n.input_variable && prompt) warnings.push({ level: 'warning', nodeId: id, message: `Action '${id}' awaits input but has no input_variable — the reply lands in the prompt's output key.` })
        break
      }
      case 'command': {
        const http = n.http_request as Record<string, unknown> | undefined
        if (!http || !http.url) errors.push({ level: 'error', nodeId: id, message: `Command '${id}' is missing http_request.url.` })
        if (!String(n.command ?? '').startsWith('http.')) errors.push({ level: 'error', nodeId: id, message: `Command '${id}' must use a http.* command (got '${n.command}').` })
        break
      }
      case 'if':
        if (!n.condition) errors.push({ level: 'error', nodeId: id, message: `If node '${id}' needs a 'condition' predicate.` })
        if (!n.then && !n.else) errors.push({ level: 'error', nodeId: id, message: `If node '${id}' needs a 'then' (or 'else') target.` })
        break
      case 'choice':
        if (!n.expression) errors.push({ level: 'error', nodeId: id, message: `Choice node '${id}' needs an 'expression'.` })
        if (!n.cases || !Object.keys(n.cases as object).length) errors.push({ level: 'error', nodeId: id, message: `Choice node '${id}' needs at least one case.` })
        if (!n.default) warnings.push({ level: 'warning', nodeId: id, message: `Choice node '${id}' has no default — unmatched values will end the session.` })
        break
      case 'try': {
        const body = (n.body ?? []) as unknown[]
        if (!body.length) errors.push({ level: 'error', nodeId: id, message: `Try node '${id}' needs a non-empty 'body'.` })
        const hasHandler = n.catch || n.finally
        if (!hasHandler) errors.push({ level: 'error', nodeId: id, message: `Try node '${id}' needs at least one of 'catch' or 'finally'.` })
        break
      }
      case 'loop': {
        const body = (n.body ?? []) as unknown[]
        if (!body.length) errors.push({ level: 'error', nodeId: id, message: `Loop node '${id}' needs a non-empty 'body'.` })
        if (!n.collection && !n.count && !n.while) errors.push({ level: 'error', nodeId: id, message: `Loop node '${id}' needs 'collection', 'count' or 'while'.` })
        break
      }
      case 'wait':
        if (!n.duration) errors.push({ level: 'error', nodeId: id, message: `Wait node '${id}' needs a 'duration'.` })
        else if (!/^\d+(\.\d+)?(ms|s|m|h|d)?$/.test(String(n.duration))) warnings.push({ level: 'warning', nodeId: id, message: `Wait node '${id}': duration '${n.duration}' should look like 30s, 5m or 1200ms.` })
        break
      case 'schedule':
        if (!n.cron) errors.push({ level: 'error', nodeId: id, message: `Schedule node '${id}' requires 'cron'.` })
        if (!n.workflow && !n.body) errors.push({ level: 'error', nodeId: id, message: `Schedule node '${id}' needs a 'body' target or a 'workflow' id.` })
        break
      case 'script':
        if (!n.code) errors.push({ level: 'error', nodeId: id, message: `Script node '${id}' needs 'code'.` })
        break
      case 'query':
        if (!n.source) errors.push({ level: 'error', nodeId: id, message: `Query node '${id}' requires a 'source' data-source id.` })
        if (!n.output) errors.push({ level: 'error', nodeId: id, message: `Query node '${id}' requires an 'output' key.` })
        break
      case 'transform':
        if (!n.output) errors.push({ level: 'error', nodeId: id, message: `Transform node '${id}' requires an 'output' key.` })
        if (['filter', 'map', 'reduce'].includes(String(n.operation)) && !n.input) errors.push({ level: 'error', nodeId: id, message: `Transform '${id}' with operation '${n.operation}' requires an 'input'.` })
        break
      case 'subflow':
        if (!n.workflow) errors.push({ level: 'error', nodeId: id, message: `Subflow node '${id}' needs a 'workflow' id.` })
        break
      case 'terminate':
        if (!['success', 'failure'].includes(String(n.status))) errors.push({ level: 'error', nodeId: id, message: `Terminate '${id}' needs status 'success' or 'failure'.` })
        break
    }
  }

  // jump target existence
  const targetFields = ['next', 'on_error', 'then', 'else', 'default', 'handler']
  for (const n of nodes) {
    const id = String(n.id ?? '')
    for (const f of targetFields) {
      const t = n[f]
      if (typeof t === 'string' && t && !nodeIds.has(t)) errors.push({ level: 'error', nodeId: id, message: `Node '${id}' jumps to '${t}' (${f}), which does not exist.` })
    }
    const cases = n.cases as Record<string, string> | undefined
    if (cases) for (const [v, t] of Object.entries(cases)) if (t && !nodeIds.has(t)) errors.push({ level: 'error', nodeId: id, message: `Choice '${id}' case '${v}' targets missing node '${t}'.` })
    const nextTasks = (n.next_tasks ?? []) as Array<Record<string, string>>
    for (const nt of nextTasks) if (nt.task_id && !nodeIds.has(nt.task_id)) errors.push({ level: 'error', nodeId: id, message: `Action '${id}' next_tasks targets missing node '${nt.task_id}'.` })
    const bodyLists = [n.body, n.finally, n.then, n.else, (n.catch as Record<string, unknown> | undefined)?.body]
    for (const list of bodyLists) {
      if (Array.isArray(list)) for (const t of list as unknown[]) {
        const tid = typeof t === 'string' ? t : String((t as SapoNode)?.id ?? '')
        if (tid && !nodeIds.has(tid)) errors.push({ level: 'error', nodeId: id, message: `Node '${id}' body references missing node '${tid}'.` })
      }
    }
    const childTasks = (n.child_tasks ?? []) as unknown[][]
    for (const branch of childTasks ?? []) {
      if (Array.isArray(branch)) for (const t of branch as unknown[]) {
        const tid = typeof t === 'string' ? t : String((t as SapoNode)?.id ?? '')
        if (tid && !nodeIds.has(tid)) errors.push({ level: 'error', nodeId: id, message: `Parallel '${id}' branch references missing node '${tid}'.` })
      }
    }
  }

  // reachability from the entry node (first node)
  if (nodes.length) {
    const adj = new Map<string, string[]>()
    const addEdge = (from: string, t: unknown) => { if (typeof t === 'string' && t) { if (!adj.has(from)) adj.set(from, []); adj.get(from)!.push(t) } }
    for (const n of nodes) {
      const id = String(n.id ?? '')
      for (const f of ['next', 'on_error', 'then', 'else', 'default', 'handler']) addEdge(id, n[f])
      const cases = n.cases as Record<string, string> | undefined
      if (cases) for (const t of Object.values(cases)) addEdge(id, t)
      for (const nt of (n.next_tasks ?? []) as Array<Record<string, string>>) addEdge(id, nt.task_id)
      for (const list of [n.body, n.finally, n.then, n.else, (n.catch as Record<string, unknown> | undefined)?.body, ...((n.child_tasks ?? []) as unknown[][])]) {
        if (Array.isArray(list)) for (const t of list) addEdge(id, typeof t === 'string' ? t : (t as SapoNode)?.id)
      }
    }
    const entry = String(nodes[0].id ?? '')
    const seen = new Set<string>([entry])
    const queue = [entry]
    while (queue.length) {
      const cur = queue.shift()!
      for (const t of adj.get(cur) ?? []) if (!seen.has(t)) { seen.add(t); queue.push(t) }
    }
    for (const n of nodes) {
      const id = String(n.id ?? '')
      if (!seen.has(id) && id !== entry) warnings.push({ level: 'warning', nodeId: id, message: `Node '${id}' is unreachable from the entry node '${entry}'.` })
    }

    // every reachable path should reach a terminate (no dangling exits)
    const hasTerminal = nodes.some((n) => n.type === 'terminate')
    if (!hasTerminal) warnings.push({ level: 'warning', message: 'No terminate node — sessions will end implicitly on nodes without next.' })
    // nodes listed inside a body frame step to their sibling implicitly
    const bodyMembers = new Set<string>()
    for (const n of nodes) {
      for (const list of [n.body, n.finally, (n.catch as Record<string, unknown> | undefined)?.body, n.then, n.else, ...((n.child_tasks ?? []) as unknown[][])]) {
        if (Array.isArray(list)) for (const t of list) {
          const tid = typeof t === 'string' ? t : String((t as SapoNode)?.id ?? '')
          if (tid) bodyMembers.add(tid)
        }
      }
    }
    for (const n of nodes) {
      const id = String(n.id ?? '')
      const type = String(n.type)
      if (TERMINAL_OK.has(type)) continue
      if (bodyMembers.has(id)) continue // implicit sibling stepping inside a frame
      const hasNext = typeof n.next === 'string' && !!n.next
      const hasBranch = ['if', 'choice', 'try', 'loop', 'parallel', 'schedule'].includes(type)
        || (Array.isArray((n as SapoNode).next_tasks) && ((n.next_tasks as unknown[]).length > 0))
      if (!hasNext && !hasBranch && type !== 'terminate') {
        warnings.push({ level: 'warning', nodeId: id, message: `Node '${id}' has no outgoing 'next' — the session ends here.` })
      }
    }
  }

  if (graph) {
    const kinds = new Map(graph.nodes.map((n) => [n.id, n.kind]))
    for (const n of graph.nodes) {
      if (['menu', 'input', 'pin'].includes(n.kind) && !(n.data.inputVariable ?? '').trim()) {
        errors.push({ level: 'error', nodeId: n.id, message: `${n.kind === 'menu' ? 'Menu' : 'Prompt'} '${n.id}' needs an input variable name.` })
      }
      if (n.kind === 'http' && !(n.data.url ?? '').trim()) {
        errors.push({ level: 'error', nodeId: n.id, message: `HTTP node '${n.id}' needs a URL.` })
      }
    }
    if (graph.nodes.length && !graph.entryId) warnings.push({ level: 'warning', message: 'No entry node set — the first node on the canvas will be used.' })
    void kinds
  }

  return { ok: errors.length === 0, errors, warnings }
}
