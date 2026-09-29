// ---------------------------------------------------------------------------
// Live USSD gateway — the API a telco (or our dialer) talks to. It runs the
// released blueprint of a short code on the SapoSimulator, enforces session
// quotas, records usage stats, and fires webhooks (session lifecycle, HTTP
// integrations, payments) exactly like the production gateway would.
// ---------------------------------------------------------------------------
import { useDb, saveDb, rid } from './db'
import { emitWebhook } from './webhooks'
import { SapoSimulator } from '../../shared/utils/vm'
import type { Flow, ShortCode, User } from '../../shared/types'
import type { StatsDay } from './db'

export interface LiveSession {
  id: string
  shortcodeId: string
  code: string
  flowId: string
  ownerId: string
  msisdn: string
  network: string
  sim: SapoSimulator
  startedAt: string
  inputs: string[]
  ended: boolean
}

const sessions = new Map<string, LiveSession>()

function pruneSessions(): void {
  const cutoff = Date.now() - 30 * 60_000
  for (const [id, s] of sessions) {
    if (Date.parse(s.startedAt) < cutoff || sessions.size > 200) sessions.delete(id)
  }
}

export function normalizeCode(raw: string): string {
  return raw.replace(/\s+/g, '').toLowerCase()
}

export function findShortcodeByCode(code: string): ShortCode | null {
  const db = useDb()
  const wanted = normalizeCode(code)
  return db.shortcodes.find((s) => normalizeCode(s.code) === wanted) ?? null
}

export function activeBlueprintFor(shortcode: ShortCode): { flow: Flow; blueprint: Record<string, unknown> } | null {
  const db = useDb()
  const flow = db.flows.find((f) => f.id === shortcode.flowId)
  if (!flow) return null
  const release = [...flow.releases].reverse().find((r) => r.status === 'active')
  const build = release
    ? flow.builds.find((b) => b.id === release.buildId)
    : flow.builds[flow.builds.length - 1]
  if (!build) return null
  return { flow, blueprint: build.blueprint as Record<string, unknown> }
}

function todayRow(db: ReturnType<typeof useDb>, shortcodeId: string): StatsDay {
  const date = new Date().toISOString().slice(0, 10)
  let row = db.stats.days.find((d) => d.date === date && d.shortcodeId === shortcodeId)
  if (!row) {
    row = { date, shortcodeId, sessions: 0, completed: 0, failed: 0, revenue: 0, avgDurationSec: 0 }
    db.stats.days.push(row)
  }
  return row
}

function recordUsage(db: ReturnType<typeof useDb>, shortcode: ShortCode, kind: 'session' | 'completed' | 'failed' | 'revenue', amount = 0): void {
  const row = todayRow(db, shortcode.id)
  if (kind === 'session') {
    row.sessions += 1
    shortcode.sessionsUsed += 1
    const h = new Date().getHours()
    db.stats.hourlyToday[h] = (db.stats.hourlyToday[h] ?? 0) + 1
  }
  if (kind === 'completed') row.completed += 1
  if (kind === 'failed') row.failed += 1
  if (kind === 'revenue') row.revenue = Math.round((row.revenue + amount) * 100) / 100
  saveDb(db)
}

/**
 * HTTP executor handed to the simulator: relative URLs are resolved against
 * the request origin (so sandbox APIs like /api/mock/pay work), and payment /
 integration calls are surfaced as webhooks.
 */
function makeHttpExecutor(origin: string, owner: User, session: LiveSession) {
  const db = () => useDb()
  return async (req: { method: string; url: string; headers: Record<string, string>; query: Record<string, string>; body?: unknown; timeout?: string }) => {
    let parsed: URL
    try {
      parsed = new URL(req.url, origin)
    } catch {
      return { status: 400, body: { error: 'invalid_url' } }
    }
    for (const [k, v] of Object.entries(req.query ?? {})) parsed.searchParams.set(k, String(v))

    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), 6000)
    let status = 0
    let body: unknown = null
    try {
      const res = await fetch(parsed.toString(), {
        method: req.method.toUpperCase(),
        headers: req.headers,
        body: ['GET', 'HEAD'].includes(req.method.toUpperCase()) ? undefined : (typeof req.body === 'string' ? req.body : JSON.stringify(req.body ?? {})),
        signal: controller.signal,
      })
      status = res.status
      body = await res.json().catch(() => null)
    } catch (e) {
      status = 0
      body = { error: (e as Error).message }
    } finally {
      clearTimeout(timer)
    }

    // webhooks: every integration call + explicit payment events
    const isPayment = /pay|charge|checkout|collect/i.test(parsed.pathname)
    void emitWebhook(db(), owner, 'flow.http_request', {
      session_id: session.id,
      shortcode: session.code,
      msisdn: session.msisdn,
      method: req.method.toUpperCase(),
      url: parsed.toString(),
      status,
      ok: status >= 200 && status < 300,
    })
    if (isPayment && status >= 200 && status < 300) {
      const b = (body ?? {}) as Record<string, unknown>
      void emitWebhook(db(), owner, 'payment.succeeded', {
        session_id: session.id,
        shortcode: session.code,
        msisdn: session.msisdn,
        amount: b.amount ?? null,
        currency: b.currency ?? 'GHS',
        description: b.description ?? null,
        transaction_id: b.transactionId ?? null,
        source: 'ussd_flow',
      })
    }
    return { status, body }
  }
}

export interface DialResult {
  sessionId: string
  status: SapoSimulator['status']
  screen: string[]
  prompt: { message: string; interactionType: string } | null
  vars: Record<string, unknown>
  shortcode: ShortCode
}

function sessionPayload(s: LiveSession): DialResult {
  return {
    sessionId: s.id,
    status: s.sim.status,
    screen: [...s.sim.screen],
    prompt: s.sim.prompt ? { message: s.sim.prompt.message, interactionType: s.sim.prompt.interactionType } : null,
    vars: Object.fromEntries(Object.entries(s.sim.vars).filter(([k]) => !k.startsWith('_'))),
    shortcode: useDb().shortcodes.find((x) => x.id === s.shortcodeId) as ShortCode,
  }
}

export async function startSession(event: Parameters<typeof getCookie>[0], opts: { shortcode: ShortCode; msisdn: string; network: string }): Promise<DialResult> {
  const db = useDb()
  const { shortcode } = opts

  if (shortcode.status !== 'active') {
    throw createError({ statusCode: 409, statusMessage: `Short code ${shortcode.code} is ${shortcode.status} — activate it first` })
  }
  if (shortcode.sessionsUsed >= shortcode.sessionsQuota) {
    throw createError({
      statusCode: 402,
      statusMessage: `Session quota exhausted for ${shortcode.code} (${shortcode.sessionsQuota.toLocaleString()} sessions). Top up to keep serving.`,
      data: { code: 'quota_exceeded', shortcodeId: shortcode.id },
    })
  }

  const target = activeBlueprintFor(shortcode)
  if (!target) {
    throw createError({ statusCode: 409, statusMessage: `No deployed flow on ${shortcode.code} — build & release one first` })
  }

  const owner = db.users.find((u) => u.id === shortcode.ownerId) ?? null
  const origin = getRequestURL(event).origin
  // allocate the session shell first so the HTTP executor can close over it
  const session: LiveSession = {
    id: rid('ussd'),
    shortcodeId: shortcode.id,
    code: shortcode.code,
    flowId: target.flow.id,
    ownerId: shortcode.ownerId ?? '',
    msisdn: opts.msisdn,
    network: opts.network,
    sim: null as never,
    startedAt: new Date().toISOString(),
    inputs: [],
    ended: false,
  }
  session.sim = new SapoSimulator(target.blueprint as never, {
    msisdn: opts.msisdn,
    network: opts.network,
    liveHttp: true,
    httpCall: owner ? makeHttpExecutor(origin, owner, session) : undefined,
  })

  await session.sim.dial()
  sessions.set(session.id, session)
  pruneSessions()

  recordUsage(db, shortcode, 'session')
  if (owner) {
    void emitWebhook(db, owner, 'session.started', {
      session_id: session.id,
      shortcode: shortcode.code,
      msisdn: opts.msisdn,
      network: opts.network,
      flow: target.flow.name,
      first_screen: session.sim.prompt?.message ?? session.sim.screen[0] ?? null,
    })
  }

  return sessionPayload(session)
}

export async function sendInput(sessionId: string, text: string): Promise<DialResult> {
  const session = sessions.get(sessionId)
  if (!session) throw createError({ statusCode: 404, statusMessage: 'USSD session not found or expired — dial again' })
  const db = useDb()
  const shortcode = db.shortcodes.find((x) => x.id === session.shortcodeId)
  if (!shortcode) throw createError({ statusCode: 404, statusMessage: 'Short code no longer exists' })

  session.inputs.push(text)
  await session.sim.submit(text)

  const owner = db.users.find((u) => u.id === shortcode.ownerId) ?? null
  if (session.sim.status === 'completed' || session.sim.status === 'failed') {
    if (!session.ended) {
      session.ended = true
      recordUsage(db, shortcode, session.sim.status === 'completed' ? 'completed' : 'failed')
      if (owner) {
        void emitWebhook(db, owner, session.sim.status === 'completed' ? 'session.completed' : 'session.failed', {
          session_id: session.id,
          shortcode: shortcode.code,
          msisdn: session.msisdn,
          inputs: session.inputs,
          duration_sec: Math.round((Date.now() - Date.parse(session.startedAt)) / 100) / 10,
          output: session.sim.output,
        })
      }
    }
  }

  return sessionPayload(session)
}

export function getSession(sessionId: string): DialResult {
  const session = sessions.get(sessionId)
  if (!session) throw createError({ statusCode: 404, statusMessage: 'USSD session not found or expired' })
  return sessionPayload(session)
}
