// ---------------------------------------------------------------------------
// Sapo mini-VM — an in-browser interpreter for Sapo DSL blueprints that
// mirrors sapo::runtime::Interpreter: jump-based execution, body frames for
// loop/try/parallel, prompt suspension & resume, next_tasks routing, error
// capture with on_error / try-catch, and terminate signals.
//
// It powers the USSD phone simulator so engineers can test exactly what they
// composed, before the blueprint ever reaches the C++ VM.
// ---------------------------------------------------------------------------
import type { SapoBlueprint, SapoNode } from '../types'
import { evalPredicate, evalTemplate, resolveTemplates, toText, type SELValue } from './sel.ts'

export interface VmTraceEntry {
  nodeId: string
  type: string
  label: string
  detail: string
  kind: 'ok' | 'prompt' | 'api' | 'jump' | 'error' | 'info' | 'end'
}

export interface VmPrompt {
  message: string
  interactionType: 'display' | 'input' | 'menu' | 'pin' | 'event'
  nodeId: string
}

export interface VmOptions {
  liveHttp?: boolean
  httpCall?: (req: { method: string; url: string; headers: Record<string, string>; query: Record<string, string>; body?: unknown; timeout?: string }) => Promise<{ status: number; body: unknown }>
  msisdn?: string
  network?: string
}

interface Frame {
  kind: 'loop' | 'try' | 'body' | 'branch'
  nodeId: string
  ids: string[]
  position: number
  resumeTarget: string
  state: Record<string, SELValue>
}

type Signal =
  | { t: 'continue' }
  | { t: 'jump'; target: string }
  | { t: 'suspend'; reason: 'input' | 'event' }
  | { t: 'terminate'; status: 'success' | 'failure'; payload: Record<string, unknown>; message?: string }
  | { t: 'loopbreak' }
  | { t: 'loopcontinue' }

class VmError extends Error {
  code: string
  nodeId: string
  data?: unknown
  constructor(code: string, message: string, nodeId: string, data?: unknown) {
    super(message)
    this.code = code
    this.nodeId = nodeId
    this.data = data
  }
}

export class SapoSimulator {
  private bp: SapoBlueprint
  private opts: VmOptions
  status: 'idle' | 'running' | 'awaiting_input' | 'awaiting_event' | 'completed' | 'failed' = 'idle'
  vars: Record<string, SELValue> = {}
  trace: VmTraceEntry[] = []
  prompt: VmPrompt | null = null
  currentNodeId: string | null = null
  error: { code: string; message: string; node: string } | null = null
  output: Record<string, unknown> | null = null
  terminateStatus: 'success' | 'failure' | null = null
  screen: string[] = [] // USSD screen history

  private nodes = new Map<string, SapoNode & { next?: string }>()
  private entryId: string | null = null
  private frames: Frame[] = []
  private pendingError: { code: string; message: string; node: string; data?: unknown } | null = null
  private resumeNodeId = ''
  private visits = 0
  private budget = 600

  constructor(bp: SapoBlueprint, opts: VmOptions = {}) {
    this.bp = bp
    this.opts = opts
    for (const n of bp.nodes ?? []) this.nodes.set(String(n.id), n as SapoNode & { next?: string })
    this.entryId = (bp.nodes ?? [])[0] ? String((bp.nodes as SapoNode[])[0].id) : null
    if (opts.msisdn) this.vars.msisdn = opts.msisdn
    if (opts.network) this.vars.network = opts.network
    for (const [k, v] of Object.entries(bp.defaults ?? {})) this.vars[k] = v as SELValue
    this.vars.session_id = `sim-${Math.random().toString(36).slice(2, 8)}`
    this.vars.now = new Date().toISOString()
  }

  // --- public API -----------------------------------------------------------

  async dial(): Promise<void> {
    this.reset()
    this.status = 'running'
    this.log('info', this.entryId ?? '', 'entry', `Session started on ${this.bp.name ?? 'workflow'}`)
    await this.drive(this.entryId ?? '')
  }

  async submit(input: string): Promise<void> {
    if (this.status !== 'awaiting_input' && this.status !== 'awaiting_event') return
    const node = this.currentNodeId ? this.nodes.get(this.currentNodeId) : null
    const interaction = this.prompt?.interactionType
    this.status = 'running'
    if (interaction === 'event') {
      this.vars.event = input
      this.vars.input = input
      this.resumeNodeId = this.currentNodeId ?? ''
      this.log('ok', 'action', this.currentNodeId ?? '', `Event received: ${input}`)
      await this.drive(this.currentNodeId ?? '')
      return
    }
    // input prompt resume
    const inputVar = node ? String(node.input_variable ?? (node.prompt_config as Record<string, unknown>)?.output ?? 'input') : 'input'
    this.vars.input = input
    this.vars[inputVar] = input
    this.log('ok', String(node?.type ?? 'action'), this.currentNodeId ?? '', `Reply "${input}" → ${inputVar}`)
    this.resumeNodeId = this.currentNodeId ?? ''
    await this.drive(this.currentNodeId ?? '')
  }

  reset(): void {
    this.status = 'idle'
    this.vars = {}
    this.trace = []
    this.prompt = null
    this.currentNodeId = null
    this.error = null
    this.output = null
    this.terminateStatus = null
    this.screen = []
    this.frames = []
    this.pendingError = null
    this.resumeNodeId = ''
    this.visits = 0
    if (this.opts.msisdn) this.vars.msisdn = this.opts.msisdn
    if (this.opts.network) this.vars.network = this.opts.network
    for (const [k, v] of Object.entries(this.bp.defaults ?? {})) this.vars[k] = v as SELValue
    this.vars.session_id = `sim-${Math.random().toString(36).slice(2, 8)}`
  }

  // --- core loop --------------------------------------------------------------

  private async drive(from: string): Promise<void> {
    let cursor: string = from
    while (this.visits < this.budget) {
      if (this.pendingError) {
        const err = this.pendingError
        this.pendingError = null
        if (!this.handleError(err)) {
          this.status = 'failed'
          this.error = { code: err.code, message: err.message, node: err.node }
          this.log('error', err.node, err.node, `${err.code}: ${err.message}`)
          this.screen.push(`✖ ${err.message}`)
          return
        }
        cursor = this.currentNodeId ?? ''
        continue
      }
      if (!cursor) {
        this.status = 'completed'
        this.log('end', '', 'session', 'Session ended (no next node)')
        this.screen.push('— session ended —')
        return
      }
      const node = this.nodes.get(cursor)
      if (!node) {
        this.status = 'failed'
        this.error = { code: 'ROUTING_ERROR', message: `control reached unknown node id '${cursor}'`, node: cursor }
        return
      }
      this.currentNodeId = cursor
      this.visits++
      let signal: Signal
      try {
        signal = await this.execute(node)
      } catch (e) {
        if (e instanceof VmError) {
          this.pendingError = { code: e.code, message: e.message, node: e.nodeId, data: e.data }
          continue
        }
        throw e
      }
      cursor = this.route(node, signal)
      if (cursor === '__suspend__') return
      if (cursor === '__terminated__') return
      if (cursor === '__propagate__') { cursor = ''; continue } // error keeps travelling after finally
    }
    this.status = 'failed'
    this.error = { code: 'BUDGET', message: 'Simulation exceeded the node-visit budget', node: '' }
  }

  private route(node: SapoNode & { next?: string }, signal: Signal): string {
    switch (signal.t) {
      case 'jump': {
        this.resolveJump(signal.target)
        return signal.target
      }
      case 'suspend':
        return '__suspend__'
      case 'terminate': {
        this.status = 'completed'
        this.terminateStatus = signal.status
        this.output = signal.payload
        const msg = signal.message ?? Object.entries(signal.payload).map(([k, v]) => `${k}: ${typeof v === 'object' ? JSON.stringify(v) : String(v)}`).join(' | ')
        this.screen.push(signal.message ? signal.message : msg || `Session ${signal.status}`)
        this.log('end', 'terminate', String(node.id), `Terminated (${signal.status})${signal.message ? ': ' + signal.message : ''}`)
        return '__terminated__'
      }
      case 'loopbreak': {
        for (let d = this.frames.length; d > 0; d--) {
          if (this.frames[d - 1].kind === 'loop') {
            const frame = this.frames[d - 1]
            this.frames.length = d - 1
            this.log('info', 'break', String(node.id), `Break out of loop '${frame.nodeId}'`)
            return this.advanceAfterNode(frame.nodeId)
          }
        }
        return this.advanceAfterNode(String(node.id))
      }
      case 'loopcontinue': {
        for (let d = this.frames.length; d > 0; d--) {
          if (this.frames[d - 1].kind === 'loop') {
            this.frames.length = d
            return this.startNextIteration()
          }
        }
        return this.advanceAfterNode(String(node.id))
      }
      case 'continue':
      default:
        return this.advanceAfterNode(String(node.id))
    }
  }

  // --- frames (mirrors Interpreter::resolveJump / advanceAfterNode) ------------

  private resolveJump(target: string): void {
    if (!target) { this.currentNodeId = ''; return }
    for (let d = this.frames.length; d > 0; d--) {
      const frame = this.frames[d - 1]
      const index = frame.ids.indexOf(target)
      if (index < 0) continue
      this.frames.length = d
      frame.position = index
      if (frame.kind === 'loop') this.bindIteration(frame)
      this.currentNodeId = target
      return
    }
    this.frames = []
    this.currentNodeId = target
  }

  private advanceAfterNode(current: string): string {
    for (;;) {
      if (!this.frames.length) {
        const node = this.nodes.get(current)
        const next = node?.next as string | undefined
        if (next) { this.currentNodeId = next; return next }
        this.currentNodeId = ''
        return ''
      }
      const frame = this.frames[this.frames.length - 1]
      const index = frame.ids.indexOf(current)

      if (index >= 0) {
        const stepping = this.nodes.get(current)
        const explicitNext = stepping?.next as string | undefined
        if (explicitNext && frame.ids.indexOf(explicitNext) < 0) {
          this.resolveJump(explicitNext)
          return this.currentNodeId ?? ''
        }
      }

      if (index < 0) {
        const owner = frame.nodeId
        if (frame.kind === 'try') {
          const advanced = this.advanceTryPhase()
          if (advanced !== null) return advanced
          current = owner
          continue
        }
        if (frame.kind === 'loop') return this.startNextIteration()
        if (frame.kind === 'branch') return ''
        const resume = frame.resumeTarget
        this.frames.pop()
        if (resume) { this.currentNodeId = resume; return resume }
        current = owner
        continue
      }

      if (index + 1 < frame.ids.length) {
        frame.position = index + 1
        const next = frame.ids[index + 1]
        this.currentNodeId = next
        return next
      }
      if (frame.kind === 'loop') return this.startNextIteration()
      if (frame.kind === 'try') {
        const advanced = this.advanceTryPhase()
        if (advanced !== null) return advanced
        current = frame.nodeId
        continue
      }
      if (frame.kind === 'branch') return ''
      const resume = frame.resumeTarget
      const owner = frame.nodeId
      this.frames.pop()
      if (resume) { this.currentNodeId = resume; return resume }
      current = owner
    }
  }

  private startNextIteration(): string {
    for (let d = this.frames.length; d > 0; d--) {
      if (this.frames[d - 1].kind !== 'loop') continue
      const frame = this.frames[d - 1]
      this.frames.length = d
      const total = Number(frame.state.total ?? 0)
      let idx = Number(frame.state.index ?? 0) + 1
      const maxIter = Number(frame.state.max_iterations ?? 1000)
      const isWhile = frame.state.mode === 'while'
      const guardOk = isWhile
        ? evalPredicate(String(frame.state.while ?? 'true'), this.vars)
        : idx < total
      if (!guardOk || idx >= maxIter) {
        this.frames.pop()
        this.log('info', 'loop', frame.nodeId, `Loop finished after ${idx} iteration${idx === 1 ? '' : 's'}`)
        if (frame.resumeTarget) { this.currentNodeId = frame.resumeTarget; return frame.resumeTarget }
        this.currentNodeId = ''
        return ''
      }
      frame.state.index = idx
      this.bindIteration(frame)
      const first = frame.ids[0]
      if (first) { this.currentNodeId = first; return first }
      this.frames.pop()
      this.currentNodeId = ''
      return ''
    }
    this.currentNodeId = ''
    return ''
  }

  private advanceTryPhase(): string | null {
    const frame = this.frames[this.frames.length - 1]
    if (!frame || frame.kind !== 'try') return null
    const phase = String(frame.state.phase ?? 'body')
    const finallyIds = (frame.state.finally_ids as string[] | undefined) ?? []
    // normal completion: body -> finally -> close; catch -> finally -> close
    if ((phase === 'body' || phase === 'catch') && finallyIds.length) {
      frame.state.phase = 'finally'
      frame.ids = finallyIds
      frame.position = 0
      this.currentNodeId = finallyIds[0]
      return finallyIds[0]
    }
    this.frames.pop()
    // an error that ran through finally keeps travelling outwards
    if (frame.state.propagate !== undefined) {
      const pending = frame.state.propagate as { code: string; message: string; node: string; data?: unknown }
      this.pendingError = pending
      this.currentNodeId = ''
      return '__propagate__'
    }
    if (frame.resumeTarget) { this.currentNodeId = frame.resumeTarget; return frame.resumeTarget }
    return null
  }

  private bindIteration(frame: Frame): void {
    const idx = Number(frame.state.index ?? 0)
    const items = (frame.state.items as SELValue[] | undefined) ?? []
    const iterator = String(frame.state.iterator ?? 'item')
    if (iterator && items[idx] !== undefined) this.vars[iterator] = items[idx]
    const indexVar = String(frame.state.index_variable ?? 'index')
    if (indexVar) this.vars[indexVar] = idx
    this.vars.iteration = idx
  }

  private handleError(err: { code: string; message: string; node: string; data?: unknown }): boolean {
    const failing = this.nodes.get(err.node)
    const onError = failing?.on_error as string | undefined
    if (onError && this.nodes.has(onError)) {
      this.vars.$error = { code: err.code, message: err.message, node: err.node, data: (err.data ?? null) as SELValue } as SELValue
      this.log('error', String(failing?.type ?? ''), err.node, `${err.code} → on_error '${onError}'`)
      this.resolveJump(onError)
      return true
    }
    // unwind try frames
    while (this.frames.length) {
      const frame = this.frames[this.frames.length - 1]
      if (frame.kind === 'try') {
        const errVar = String(frame.state.error_variable ?? 'error')
        this.vars[errVar] = { code: err.code, message: err.message, node: err.node, data: (err.data ?? null) as SELValue } as SELValue
        const catchIds = (frame.state.catch_ids as string[] | undefined) ?? []
        if (catchIds.length) {
          this.frames.length = this.frames.length // keep the frame; move to catch phase
          frame.state.phase = 'catch'
          this.log('error', 'try', frame.nodeId, `${err.code} caught → ${errVar}`)
          const first = catchIds[0]
          this.resolveJump(first)
          return true
        }
        const finallyIds = (frame.state.finally_ids as string[] | undefined) ?? []
        if (finallyIds.length) {
          frame.state.phase = 'finally'
          this.resolveJump(finallyIds[0])
          return true
        }
      }
      this.frames.pop()
    }
    return false
  }

  // --- node execution -----------------------------------------------------------

  private async execute(node: SapoNode & { next?: string }): Promise<Signal> {
    const id = String(node.id)
    const type = String(node.type)
    const label = String(node.label ?? id)
    switch (type) {
      case 'action': return this.execAction(node, id, label)
      case 'noop': {
        const assign = (node.assign ?? {}) as Record<string, SELValue>
        for (const [k, v] of Object.entries(assign)) this.vars[k] = resolveTemplates(v, this.vars)
        this.log('ok', type, id, `Set ${Object.keys(assign).join(', ') || '(nothing)'}`)
        return { t: 'continue' }
      }
      case 'command': return this.execCommand(node, id, label)
      case 'if': {
        const cond = String(node.condition ?? 'true')
        const taken = evalPredicate(cond, this.vars)
        this.log('jump', type, id, `${cond} → ${taken}`)
        const branch = (taken ? node.then : node.else) as unknown
        if (typeof branch === 'string' && branch) return { t: 'jump', target: branch }
        if (Array.isArray(branch) && branch.length) {
          // body-array branch: a frame that resumes at `next` when done
          const ids = this.bodyIds(branch)
          this.frames.push({ kind: 'body', nodeId: id, ids, position: 0, resumeTarget: (node.next as string | undefined) ?? '', state: {} })
          return { t: 'jump', target: ids[0] }
        }
        return { t: 'continue' }
      }
      case 'choice': {
        const value = toText(evalTemplate(String(node.expression ?? ''), this.vars))
        const cases = (node.cases ?? {}) as Record<string, string>
        const target = cases[value] ?? (node.default as string | undefined)
        this.log('jump', type, id, `'${value}' → ${target ?? '(no match)'}`)
        return target ? { t: 'jump', target } : { t: 'continue' }
      }
      case 'try': {
        const body = this.bodyIds(node.body)
        const catchBlock = node.catch as Record<string, unknown> | undefined
        const catchIds = Array.isArray(catchBlock?.body)
          ? (catchBlock!.body as string[])
          : catchBlock?.target ? [String(catchBlock.target)] : []
        const finallyIds = this.bodyIds(node.finally)
        this.frames.push({
          kind: 'try', nodeId: id, ids: body, position: 0,
          resumeTarget: (node.next as string | undefined) ?? '',
          state: { phase: 'body', catch_ids: catchIds, finally_ids: finallyIds, error_variable: String(catchBlock?.as ?? 'error') },
        })
        this.log('info', type, id, `Guard ${body.length} body node(s)`)
        return body.length ? { t: 'jump', target: body[0] } : { t: 'continue' }
      }
      case 'loop': {
        let items: SELValue[] = []
        let whileMode = false
        if (node.collection) {
          const v = evalTemplate(String(node.collection), this.vars)
          items = Array.isArray(v) ? v : v === null || v === undefined ? [] : [v]
        } else if (node.count) {
          const n = Number(evalTemplate(String(node.count), this.vars))
          items = Array.from({ length: Number.isFinite(n) ? Math.max(0, Math.min(n, 200)) : 0 }, (_, i) => i + 1)
        } else {
          whileMode = true
        }
        const body = this.bodyIds(node.body)
        if (!body.length) return { t: 'continue' }
        if (!whileMode && !items.length) {
          this.log('info', type, id, 'Zero iterations — skipped')
          return { t: 'continue' }
        }
        this.frames.push({
          kind: 'loop', nodeId: id, ids: body, position: 0,
          resumeTarget: (node.next as string | undefined) ?? '',
          state: {
            mode: whileMode ? 'while' : 'collection', items, index: 0,
            total: items.length, max_iterations: Number(node.max_iterations ?? 1000),
            iterator: String(node.iterator ?? 'item'), index_variable: String(node.index ?? 'index'),
            while: String(node.while ?? 'true'),
          },
        })
        this.bindIteration(this.frames[this.frames.length - 1])
        this.log('info', type, id, `Iterate ${whileMode ? 'while guard holds' : items.length + ' item(s)'}`)
        return { t: 'jump', target: body[0] }
      }
      case 'parallel': {
        const childTasks = (node.child_tasks ?? []) as Array<string[] | SapoNode[]>
        const changed: Record<string, SELValue> = {}
        for (const [bi, branch] of childTasks.entries()) {
          const ids = this.bodyIds(branch as string[])
          const fork = { ...this.vars }
          for (const bid of ids) {
            const bn = this.nodes.get(bid)
            if (!bn) continue
            this.currentNodeId = bid
            const sig = await this.execute(bn)
            if (sig.t === 'terminate') return sig
          }
          for (const k of Object.keys(this.vars)) if (this.vars[k] !== fork[k]) changed[k] = this.vars[k]
          this.log('ok', type, id, `Branch ${bi + 1}: ${ids.length} node(s)`)
        }
        Object.assign(this.vars, changed)
        return { t: 'continue' }
      }
      case 'break': return { t: 'loopbreak' }
      case 'continue': return { t: 'loopcontinue' }
      case 'loop_control': return String(node.action ?? 'break') === 'continue' ? { t: 'loopcontinue' } : { t: 'loopbreak' }
      case 'terminate': {
        const output = resolveTemplates((node.output ?? {}) as Record<string, unknown>, this.vars)
        const message = node.message ? toText(evalTemplate(String(node.message), this.vars)) : undefined
        return { t: 'terminate', status: (node.status as 'success' | 'failure') ?? 'success', payload: output as Record<string, unknown>, message }
      }
      case 'wait': {
        this.log('info', type, id, `Wait ${String(node.duration ?? '')} (simulated instantly)`)
        return { t: 'continue' }
      }
      case 'event': {
        const payload = resolveTemplates((node.payload ?? {}) as Record<string, unknown>, this.vars)
        this.log('ok', type, id, `Published '${String(node.event ?? node.name ?? '')}' ${JSON.stringify(payload)}`)
        return { t: 'continue' }
      }
      case 'subflow': {
        this.vars[`${id}_session`] = `child-${Math.random().toString(36).slice(2, 6)}`
        this.vars[`${id}_status`] = 'completed'
        this.log('info', type, id, `Subflow '${String(node.workflow ?? '')}' simulated`)
        return { t: 'continue' }
      }
      case 'script': {
        // best-effort: evaluate `name = expression` lines
        const code = String(node.code ?? '')
        let count = 0
        for (const line of code.split('\n')) {
          const m = /^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$/.exec(line)
          if (m) { this.vars[m[1]] = evalTemplate(m[2], this.vars); count++ }
        }
        this.log('ok', type, id, `Script: ${count} assignment(s) simulated`)
        return { t: 'continue' }
      }
      case 'query': {
        const src = String(node.source ?? '')
        const rows = (this.vars[src] as SELValue[] | undefined) ?? []
        const out = String(node.output ?? 'rows')
        this.vars[out] = rows
        this.log('ok', type, id, `Query '${src}' → ${out} (${Array.isArray(rows) ? rows.length : 0} rows, mocked)`)
        return { t: 'continue' }
      }
      case 'transform': {
        const op = String(node.operation ?? 'assign')
        const input = node.input ? evalTemplate(String(node.input), this.vars) : null
        let result: SELValue = input
        if (op === 'filter' && Array.isArray(input)) {
          result = input.filter(() => true) // predicates inside mapping are approximated
        } else if (op === 'map' || op === 'project') {
          const mapping = (node.mapping ?? {}) as Record<string, string>
          const project = (item: SELValue) => {
            const scope = { ...this.vars, [String(node.item_variable ?? 'item')]: item }
            const out: Record<string, SELValue> = {}
            for (const [k, v] of Object.entries(mapping)) out[k] = evalTemplate(String(v), scope)
            return out
          }
          result = Array.isArray(input) ? input.map(project) : input ? project(input) : null
        } else if (op === 'count' || op === 'len') {
          result = Array.isArray(input) ? input.length : 0
        }
        this.vars[String(node.output ?? 'result')] = result
        this.log('ok', type, id, `Transform ${op} → ${String(node.output ?? 'result')}`)
        return { t: 'continue' }
      }
      case 'condition': {
        const taken = evalPredicate(String(node.expression ?? node.condition ?? 'true'), this.vars)
        const then = node.then as unknown
        this.log('jump', type, id, `${taken}`)
        if (taken && typeof then === 'string' && then) return { t: 'jump', target: then }
        if (!taken) {
          const els = node.else as unknown
          if (typeof els === 'string' && els) return { t: 'jump', target: els }
        }
        return { t: 'continue' }
      }
      default:
        this.log('info', type, id, `${type} node simulated (passthrough)`)
        return { t: 'continue' }
    }
  }

  private execAction(node: SapoNode & { next?: string }, id: string, label: string): Signal {
    const prompt = node.prompt_config as Record<string, unknown> | undefined
    const resumed = this.resumeNodeId === id
    if (resumed) this.resumeNodeId = ''

    const onEvent = node.on_event as Record<string, string> | undefined
    if (onEvent && !resumed) {
      const msg = `⏳ Waiting for event '${onEvent.event_name}'…`
      this.screen.push(msg)
      this.log('prompt', 'action', id, msg)
      this.prompt = { message: msg, interactionType: 'event', nodeId: id }
      this.status = 'awaiting_event'
      return { t: 'suspend', reason: 'event' }
    }

    if (prompt && node.await_input !== false && !resumed) {
      const message = toText(evalTemplate(String(prompt.message ?? ''), this.vars))
      this.screen.push(message)
      this.log('prompt', 'action', id, message.replace(/\n/g, ' ⏎ '))
      this.prompt = { message, interactionType: (prompt.interaction_type as VmPrompt['interactionType']) ?? 'input', nodeId: id }
      this.status = 'awaiting_input'
      return { t: 'suspend', reason: 'input' }
    }

    if (prompt && node.await_input === false) {
      const message = toText(evalTemplate(String(prompt.message ?? ''), this.vars))
      this.screen.push(message)
      this.log('prompt', 'action', id, message.replace(/\n/g, ' ⏎ '))
    }

    if (resumed && prompt) {
      const validation = prompt.input_validation as string | undefined
      if (validation && !evalPredicate(validation, { ...this.vars, input: this.vars.input, value: this.vars.input })) {
        throw new VmError('VALIDATION', `input for node '${id}' did not satisfy the prompt rule '${validation}'`, id, { input: this.vars.input })
      }
    }

    const nextTasks = (node.next_tasks ?? []) as Array<Record<string, string>>
    for (const nt of nextTasks) {
      if (evalPredicate(String(nt.execute_condition ?? 'true'), this.vars)) {
        this.log('jump', 'action', id, `next_tasks match → ${nt.task_id}`)
        return { t: 'jump', target: nt.task_id }
      }
    }
    void label
    return { t: 'continue' }
  }

  private async execCommand(node: SapoNode & { next?: string }, id: string, label: string): Promise<Signal> {
    const http = (node.http_request ?? {}) as Record<string, unknown>
    const method = String(node.command ?? 'http.get').replace('http.', '').toUpperCase()
    const url = toText(evalTemplate(String(http.url ?? ''), this.vars))
    const headers = resolveTemplates((http.headers ?? {}) as Record<string, string>, this.vars)
    const query = resolveTemplates((http.query ?? {}) as Record<string, string>, this.vars)
    const body = resolveTemplates(http.body as Record<string, unknown> | undefined, this.vars)

    let status = 200
    let respBody: unknown = { ok: true, message: 'Simulated API response', node: id }
    if (this.opts.liveHttp && this.opts.httpCall) {
      try {
        const res = await this.opts.httpCall({ method, url, headers: headers as Record<string, string>, query: query as Record<string, string>, body, timeout: http.timeout ? String(http.timeout) : undefined })
        status = res.status
        respBody = res.body
      } catch (e) {
        throw new VmError('HTTP', `request to ${url} failed: ${(e as Error).message}`, id)
      }
    } else {
      await new Promise((r) => setTimeout(r, 180))
      respBody = { ok: true, message: `Simulated ${method} ${url}`, received: body ?? query ?? null, at: new Date().toISOString() }
    }
    this.log('api', 'command', id, `${method} ${url} → ${status}`)

    if (status >= 400) {
      throw new VmError(status >= 500 ? 'HTTP_STATUS' : 'HTTP_STATUS', `HTTP ${status} from ${url}`, id, { status })
    }

    const outputs = (node.output ?? {}) as Record<string, string>
    for (const [key, path] of Object.entries(outputs)) {
      this.vars[key] = this.extractPath({ status, body: respBody }, String(path))
    }
    void label
    return { t: 'continue' }
  }

  private extractPath(root: unknown, path: string): SELValue {
    if (!path.startsWith('$')) return path
    let cur: SELValue = root as SELValue
    for (const part of path.replace(/^\$\.?/, '').split('.')) {
      if (!part) continue
      if (cur === null || cur === undefined) return null
      if (Array.isArray(cur)) cur = cur[parseInt(part, 10)] ?? null
      else if (typeof cur === 'object') cur = (cur as Record<string, SELValue>)[part] ?? null
      else return null
    }
    return cur === undefined ? null : cur
  }

  private bodyIds(value: unknown): string[] {
    if (!Array.isArray(value)) return []
    return (value as Array<string | SapoNode>).map((x) => (typeof x === 'string' ? x : String((x as SapoNode).id ?? ''))).filter(Boolean)
  }

  private log(kind: VmTraceEntry['kind'], type: string, nodeId: string, detail: string): void {
    const node = this.nodes.get(nodeId)
    this.trace.push({ nodeId, type, label: String(node?.label ?? nodeId), detail, kind })
  }
}
