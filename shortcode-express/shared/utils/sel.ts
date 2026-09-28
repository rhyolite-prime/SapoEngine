// ---------------------------------------------------------------------------
// Mini-SEL — a TypeScript evaluator for the Sapo Expression Language (SEL)
// subset used inside USSD blueprints: predicates, ${...} templates, $var
// selectors and the standard helper functions (len, matches, coalesce, ...).
//
// It mirrors sapo::runtime::ExpressionEvaluator closely enough to power the
// in-browser flow simulator, live previews and client-side validation.
// ---------------------------------------------------------------------------

export type SELValue = string | number | boolean | null | SELValue[] | { [k: string]: SELValue } | undefined

export interface SELScope {
  vars: Record<string, SELValue>
}

// --- Tokenizer -------------------------------------------------------------

type TokType = 'num' | 'str' | 'ident' | 'op'
interface Tok { type: TokType; value: string }

const OPS = ['==', '!=', '>=', '<=', '&&', '||', '>', '<', '+', '-', '*', '/', '%', '!', '(', ')', ',', '.', '[', ']', '?', ':']

function tokenize(src: string): Tok[] {
  const toks: Tok[] = []
  let i = 0
  while (i < src.length) {
    const c = src[i]
    if (/\s/.test(c)) { i++; continue }
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1] ?? ''))) {
      let j = i
      while (j < src.length && /[0-9.]/.test(src[j])) j++
      toks.push({ type: 'num', value: src.slice(i, j) }); i = j; continue
    }
    if (c === "'" || c === '"') {
      let j = i + 1; let out = ''
      while (j < src.length && src[j] !== c) {
        if (src[j] === '\\' && j + 1 < src.length) { out += unescapeChar(src[j + 1]); j += 2 }
        else { out += src[j]; j++ }
      }
      toks.push({ type: 'str', value: out }); i = j + 1; continue
    }
    if (/[A-Za-z_$]/.test(c)) {
      let j = i
      while (j < src.length && /[A-Za-z0-9_$]/.test(src[j])) j++
      toks.push({ type: 'ident', value: src.slice(i, j) }); i = j; continue
    }
    const op = OPS.find((o) => src.startsWith(o, i))
    if (op) { toks.push({ type: 'op', value: op }); i += op.length; continue }
    throw new Error(`SEL: unexpected character '${c}'`)
  }
  return toks
}

function unescapeChar(c: string): string {
  if (c === 'n') return '\n'
  if (c === 't') return '\t'
  if (c === 'r') return '\r'
  return c
}

// --- Parser (precedence climbing) -------------------------------------------

type Node =
  | { t: 'num'; v: number }
  | { t: 'str'; v: string }
  | { t: 'bool'; v: boolean }
  | { t: 'null' }
  | { t: 'var'; name: string; path: string[] }
  | { t: 'call'; name: string; args: Node[] }
  | { t: 'un'; op: string; a: Node }
  | { t: 'bin'; op: string; a: Node; b: Node }
  | { t: 'cond'; c: Node; a: Node; b: Node }

const BIN_PREC: Record<string, number> = {
  '||': 1, '&&': 2,
  '==': 3, '!=': 3,
  '>': 4, '<': 4, '>=': 4, '<=': 4,
  '+': 5, '-': 5,
  '*': 6, '/': 6, '%': 6,
}

class Parser {
  pos = 0
  private toks: Tok[]
  constructor(toks: Tok[]) {
    this.toks = toks
  }

  peek(): Tok | undefined { return this.toks[this.pos] }
  next(): Tok | undefined { return this.toks[this.pos++] }
  expect(op: string) {
    const t = this.next()
    if (!t || t.value !== op) throw new Error(`SEL: expected '${op}'`)
  }

  parse(): Node {
    const n = this.parseTernary()
    if (this.pos < this.toks.length) throw new Error('SEL: trailing input')
    return n
  }

  parseTernary(): Node {
    const c = this.parseBin(0)
    if (this.peek()?.value === '?') {
      this.next()
      const a = this.parseTernary()
      this.expect(':')
      const b = this.parseTernary()
      return { t: 'cond', c, a, b }
    }
    return c
  }

  parseBin(min: number): Node {
    let left = this.parseUnary()
    for (;;) {
      const t = this.peek()
      if (!t || t.type !== 'op' || !(t.value in BIN_PREC)) break
      const prec = BIN_PREC[t.value]
      if (prec < min) break
      this.next()
      const right = this.parseBin(prec + 1)
      left = { t: 'bin', op: t.value, a: left, b: right }
    }
    return left
  }

  parseUnary(): Node {
    const t = this.peek()
    if (t && t.type === 'op' && (t.value === '!' || t.value === '-')) {
      this.next()
      return { t: 'un', op: t.value, a: this.parseUnary() }
    }
    return this.parsePostfix()
  }

  parsePostfix(): Node {
    let n = this.parsePrimary()
    for (;;) {
      const t = this.peek()
      if (!t || t.type !== 'op') break
      if (t.value === '.') {
        this.next()
        const id = this.next()
        if (!id) throw new Error('SEL: expected property name')
        if (this.peek()?.value === '(') { // method-ish call -> treat as function with receiver arg
          this.next()
          const args = this.parseArgs()
          n = { t: 'call', name: `__method_${id.value}`, args: [n, ...args] }
        } else {
          n = this.pathAppend(n, id.value)
        }
      } else if (t.value === '[') {
        this.next()
        const idx = this.parseTernary()
        this.expect(']')
        n = { t: 'call', name: '__index', args: [n, idx] }
      } else break
    }
    return n
  }

  pathAppend(n: Node, prop: string): Node {
    if (n.t === 'var') return { t: 'var', name: n.name, path: [...n.path, prop] }
    return { t: 'call', name: '__index', args: [n, { t: 'str', v: prop }] }
  }

  parseArgs(): Node[] {
    const args: Node[] = []
    if (this.peek()?.value === ')') { this.next(); return args }
    for (;;) {
      args.push(this.parseTernary())
      const t = this.next()
      if (!t) throw new Error('SEL: unterminated call')
      if (t.value === ')') break
      if (t.value !== ',') throw new Error("SEL: expected ',' or ')'")
    }
    return args
  }

  parsePrimary(): Node {
    const t = this.next()
    if (!t) throw new Error('SEL: unexpected end of expression')
    if (t.type === 'num') return { t: 'num', v: parseFloat(t.value) }
    if (t.type === 'str') return { t: 'str', v: t.value }
    if (t.type === 'ident') {
      if (t.value === 'true') return { t: 'bool', v: true }
      if (t.value === 'false') return { t: 'bool', v: false }
      if (t.value === 'null') return { t: 'null' }
      if (this.peek()?.value === '(') {
        this.next()
        return { t: 'call', name: t.value, args: this.parseArgs() }
      }
      return { t: 'var', name: t.value, path: [] }
    }
    if (t.value === '(') {
      const n = this.parseTernary()
      this.expect(')')
      return n
    }
    throw new Error(`SEL: unexpected token '${t.value}'`)
  }
}

// --- Value helpers ----------------------------------------------------------

export function toText(v: SELValue): string {
  if (v === null || v === undefined) return ''
  if (typeof v === 'string') return v
  if (typeof v === 'number' || typeof v === 'boolean') return String(v)
  return JSON.stringify(v)
}

function truthy(v: SELValue): boolean {
  if (v === null || v === undefined || v === false) return false
  if (v === true) return true
  if (typeof v === 'number') return v !== 0
  if (typeof v === 'string') return v.length > 0
  if (Array.isArray(v)) return v.length > 0
  return Object.keys(v).length > 0
}

function toNum(v: SELValue): number {
  if (typeof v === 'number') return v
  if (typeof v === 'boolean') return v ? 1 : 0
  const n = parseFloat(String(v ?? ''))
  return Number.isNaN(n) ? 0 : n
}

function asPattern(re: string): RegExp {
  const m = /^\/(.*)\/([a-z]*)$/.exec(re)
  return m ? new RegExp(m[1], m[2]) : new RegExp(re)
}

// --- Evaluator ---------------------------------------------------------------

export function evaluate(node: Node, scope: SELScope): SELValue {
  switch (node.t) {
    case 'num': return node.v
    case 'str': return node.v
    case 'bool': return node.v
    case 'null': return null
    case 'var': {
      let v: SELValue = scope.vars[node.name]
      if (v === undefined && node.path.length === 0 && node.name.startsWith('$')) v = scope.vars[node.name]
      if (v === undefined) v = scope.vars[`$${node.name}`]
      for (const p of node.path) {
        if (v === null || v === undefined) return null
        if (Array.isArray(v)) v = v[parseInt(p, 10)]
        else if (typeof v === 'object') v = (v as Record<string, SELValue>)[p]
        else return null
      }
      return v === undefined ? null : v
    }
    case 'un': {
      const a = evaluate(node.a, scope)
      if (node.op === '!') return !truthy(a)
      return -toNum(a)
    }
    case 'cond': return truthy(evaluate(node.c, scope)) ? evaluate(node.a, scope) : evaluate(node.b, scope)
    case 'bin': return evalBin(node.op, evaluate(node.a, scope), evaluate(node.b, scope))
    case 'call': return evalCall(node.name, node.args.map((a) => evaluate(a, scope)), scope)
  }
}

function evalBin(op: string, a: SELValue, b: SELValue): SELValue {
  switch (op) {
    case '==': return selEquals(a, b)
    case '!=': return !selEquals(a, b)
    case '>': return toNum(a) > toNum(b)
    case '<': return toNum(a) < toNum(b)
    case '>=': return toNum(a) >= toNum(b)
    case '<=': return toNum(a) <= toNum(b)
    case '&&': return truthy(a) ? b : a
    case '||': return truthy(a) ? a : b
    case '+':
      if (typeof a === 'string' || typeof b === 'string') return toText(a) + toText(b)
      return toNum(a) + toNum(b)
    case '-': return toNum(a) - toNum(b)
    case '*': return toNum(a) * toNum(b)
    case '/': return toNum(b) === 0 ? null : toNum(a) / toNum(b)
    case '%': return toNum(b) === 0 ? null : toNum(a) % toNum(b)
  }
  throw new Error(`SEL: unknown operator ${op}`)
}

function selEquals(a: SELValue, b: SELValue): boolean {
  if (a === null && b === null) return true
  if (typeof a === 'number' || typeof b === 'number') {
    const an = typeof a === 'number' ? a : parseFloat(String(a))
    const bn = typeof b === 'number' ? b : parseFloat(String(b))
    if (!Number.isNaN(an) && !Number.isNaN(bn)) return an === bn
  }
  if (typeof a === 'boolean' || typeof b === 'boolean') return truthy(a) === truthy(b)
  return toText(a) === toText(b)
}

function evalCall(name: string, args: SELValue[], scope: SELScope): SELValue {
  switch (name) {
    case 'len': return args[0] === null || args[0] === undefined ? 0
      : Array.isArray(args[0]) ? args[0].length
      : typeof args[0] === 'object' ? Object.keys(args[0]).length
      : String(args[0]).length
    case 'matches': return args[1] !== null && args[1] !== undefined ? asPattern(String(args[1])).test(toText(args[0])) : false
    case 'starts_with': return toText(args[0]).startsWith(toText(args[1]))
    case 'ends_with': return toText(args[0]).endsWith(toText(args[1]))
    case 'contains':
      if (Array.isArray(args[0])) return args[0].some((x) => selEquals(x, args[1]))
      return toText(args[0]).includes(toText(args[1]))
    case 'coalesce': { const found = args.find((a) => a !== null && a !== undefined && a !== ''); return found === undefined ? null : found }
    case 'if': return truthy(args[0]) ? args[1] : args[2]
    case 'to_number': { const n = parseFloat(toText(args[0])); return Number.isNaN(n) ? null : n }
    case 'to_string': return toText(args[0])
    case 'upper': return toText(args[0]).toUpperCase()
    case 'lower': return toText(args[0]).toLowerCase()
    case 'trim': return toText(args[0]).trim()
    case 'substring': return toText(args[0]).slice(toNum(args[1]), args[2] !== undefined && args[2] !== null ? toNum(args[2]) : undefined)
    case 'split': return toText(args[0]).split(toText(args[1]))
    case 'join': return (Array.isArray(args[0]) ? args[0] : []).map(toText).join(toText(args[1]))
    case 'abs': return Math.abs(toNum(args[0]))
    case 'min': return Math.min(...args.map(toNum))
    case 'max': return Math.max(...args.map(toNum))
    case 'round': return Math.round(toNum(args[0]))
    case 'floor': return Math.floor(toNum(args[0]))
    case 'ceil': return Math.ceil(toNum(args[0]))
    case 'sum': return (Array.isArray(args[0]) ? args[0] : []).reduce((s: number, x) => s + toNum(x), 0)
    case 'now': return Date.now()
    case 'now_iso': return new Date().toISOString()
    case 'uuid': return `sim-${Math.random().toString(36).slice(2, 10)}`
    case 'typeof': return typeof args[0]
    case '__index': {
      const obj = args[0]; const key = args[1]
      if (obj === null || obj === undefined) return null
      if (Array.isArray(obj) && typeof key === 'number') return obj[key] ?? null
      return (obj as Record<string, SELValue>)[toText(key)] ?? null
    }
    case '__method_length': return Array.isArray(args[0]) ? args[0].length : toText(args[0]).length
    case '__method_toUpperCase': return toText(args[0]).toUpperCase()
    case '__method_toLowerCase': return toText(args[0]).toLowerCase()
    case '__method_trim': return toText(args[0]).trim()
    case '__method_split': return toText(args[0]).split(toText(args[1] ?? ''))
    case '__method_includes': return toText(args[0]).includes(toText(args[1] ?? ''))
    default:
      // unknown function -> null (matches engine's graceful degradation in sim mode)
      return null
  }
}

// --- Public API --------------------------------------------------------------

/** Evaluate a SEL predicate program to a boolean. */
export function evalPredicate(src: string, vars: Record<string, SELValue>): boolean {
  try {
    return truthy(evaluate(new Parser(tokenize(src)).parse(), { vars }))
  } catch {
    return false
  }
}

/** Evaluate a raw expression (returns typed value). */
export function evalExpression(src: string, vars: Record<string, SELValue>): SELValue {
  try {
    return evaluate(new Parser(tokenize(src)).parse(), { vars })
  } catch {
    return null
  }
}

/** Evaluate a template: `${expr}` blocks mixed with literal text. A lone `$path` yields the typed value. */
export function evalTemplate(src: string, vars: Record<string, SELValue>): SELValue {
  if (typeof src !== 'string') return src
  const lone = /^\s*\$([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)*)\s*$/.exec(src)
  if (lone) {
    const v = evalExpression(lone[1], vars)
    return v === null ? null : v
  }
  return src.replace(/\$\{([^}]+)\}/g, (_, expr) => {
    const v = evalExpression(expr, vars)
    return v === null || v === undefined ? '' : toText(v)
  })
}

/** Recursively resolve templates inside an object/array structure (for assign/input/body payloads). */
export function resolveTemplates<T>(value: T, vars: Record<string, SELValue>): T {
  if (typeof value === 'string') return evalTemplate(value, vars) as unknown as T
  if (Array.isArray(value)) return value.map((v) => resolveTemplates(v, vars)) as unknown as T
  if (value && typeof value === 'object') {
    const out: Record<string, unknown> = {}
    for (const [k, v] of Object.entries(value)) out[k] = resolveTemplates(v, vars)
    return out as unknown as T
  }
  return value
}
