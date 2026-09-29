// ---------------------------------------------------------------------------
// ShortCodeExpress file-backed JSON store (Nitro server util).
// In production this would be PostgreSQL + Redis; for the reference build a
// single JSON document keeps the app self-contained and inspectable.
// ---------------------------------------------------------------------------
import fs from 'node:fs'
import path from 'node:path'
import { randomUUID } from 'node:crypto'
import type { Build, CheckoutSession, Flow, Invoice, Invite, Member, Plan, Release, SessionPack, ShortCode, User, WebhookDelivery } from '../../shared/types'
import { blueprintToGraph, graphToBlueprint, validateBlueprint } from '../../shared/utils/sapo'

const DATA_DIR = path.resolve(process.cwd(), '.data')
const DB_FILE = path.join(DATA_DIR, 'db.json')

export interface StatsDay {
  date: string
  shortcodeId: string
  sessions: number
  completed: number
  failed: number
  revenue: number
  avgDurationSec: number
}

export interface DB {
  users: User[]
  flows: Flow[]
  shortcodes: ShortCode[]
  members: Member[]
  invites: Invite[]
  invoices: Invoice[]
  plans: Plan[]
  checkouts: CheckoutSession[]
  webhookDeliveries: WebhookDelivery[]
  stats: {
    days: StatsDay[]
    networks: Record<string, number>
    hourlyToday: number[]
    topFlows: Array<{ flowId: string; sessions: number; completion: number }>
  }
  business: { name: string; currency: string; billingEmail: string; country: string }
}

export const PLANS: Plan[] = [
  {
    id: 'starter', name: 'Starter', priceMonthly: 149, sessionQuota: 10000, overagePerSession: 0.05, maxShortcodes: 1,
    features: ['1 short code', '10,000 sessions / month', 'Community support', 'Blueprint simulator'],
  },
  {
    id: 'growth', name: 'Growth', priceMonthly: 499, sessionQuota: 75000, overagePerSession: 0.035, maxShortcodes: 3,
    features: ['3 short codes', '75,000 sessions / month', 'Rollback & release history', 'Live API integration', 'Priority support'],
  },
  {
    id: 'scale', name: 'Scale', priceMonthly: 1499, sessionQuota: 500000, overagePerSession: 0.02, maxShortcodes: 10,
    features: ['10 short codes', '500,000 sessions / month', 'Team collaboration (unlimited seats)', 'Dedicated throughput', 'SLA 99.95%'],
  },

]

/** Session top-up packs (pay-as-you-grow, webcheckout) */
export const SESSION_PACKS: SessionPack[] = [
  { id: 'p5', sessions: 5_000, price: 120, label: '5,000 sessions', perSession: 0.024 },
  { id: 'p20', sessions: 20_000, price: 400, label: '20,000 sessions', perSession: 0.02 },
  { id: 'p50', sessions: 50_000, price: 850, label: '50,000 sessions', perSession: 0.017 },
  { id: 'p150', sessions: 150_000, price: 2_200, label: '150,000 sessions', perSession: 0.0147 },
]

/** One-time activation fee for a new short code, by plan */
export const SHORTCODE_SETUP_FEES: Record<string, number> = { starter: 250, growth: 400, scale: 750 }

export const WEBHOOK_EVENTS: Array<{ id: import('../../shared/types').WebhookEventName; label: string; desc: string }> = [
  { id: 'payment.succeeded', label: 'payment.succeeded', desc: 'A webcheckout payment succeeded (code purchase or top-up), or an in-flow charge was accepted' },
  { id: 'shortcode.assigned', label: 'shortcode.assigned', desc: 'A new short code was provisioned and is live' },
  { id: 'sessions.topped_up', label: 'sessions.topped_up', desc: 'A session pack purchase was credited to a short code' },
  { id: 'session.started', label: 'session.started', desc: 'A subscriber dialed one of your codes' },
  { id: 'session.completed', label: 'session.completed', desc: 'A USSD session reached a success terminate node' },
  { id: 'session.failed', label: 'session.failed', desc: 'A USSD session ended in failure or quota exhaustion' },
  { id: 'flow.http_request', label: 'flow.http_request', desc: 'Your flow called an HTTP integration while a session was live' },
  { id: 'test.ping', label: 'test.ping', desc: 'Test event you can fire any time' },
]

function rid(prefix: string): string {
  return `${prefix}_${randomUUID().slice(0, 8)}`
}

function loadExample(file: string): Record<string, unknown> | null {
  try {
    const p = path.resolve(process.cwd(), '..', 'examples', file)
    return JSON.parse(fs.readFileSync(p, 'utf-8'))
  } catch {
    return null
  }
}

function seed(): DB {
  const now = new Date()
  const iso = (d: Date) => d.toISOString()
  const users: User[] = [
    { id: 'u_ama', name: 'Ama Mensah', email: 'ama@rhyoliteprime.com', role: 'owner', avatarHue: 268, title: 'Head of USSD Engineering' },
    { id: 'u_kwame', name: 'Kwame Boateng', email: 'kwame@rhyoliteprime.com', role: 'editor', avatarHue: 200, title: 'Integration Engineer' },
    { id: 'u_zainab', name: 'Zainab Musah', email: 'zainab@rhyoliteprime.com', role: 'viewer', avatarHue: 330, title: 'Product Analyst' },
  ]

  const shortcodes: ShortCode[] = [
    { id: 'sc_daccu', code: '*920*108#', label: 'DACCU Credit Union', network: 'MTN', status: 'active', plan: 'scale', sessionsUsed: 341_208, sessionsQuota: 500_000, createdAt: iso(new Date('2025-11-02')), flowId: 'fl_daccu' },
    { id: 'sc_momo', code: '*713*9#', label: 'MoMo Transfer Guard', network: 'AirtelTigo', status: 'active', plan: 'growth', sessionsUsed: 61_442, sessionsQuota: 75_000, createdAt: iso(new Date('2026-02-14')), flowId: 'fl_momo' },
    { id: 'sc_kyc', code: '*384*2#', label: 'KYC Re-check', network: 'Vodafone', status: 'active', plan: 'growth', sessionsUsed: 18_930, sessionsQuota: 75_000, createdAt: iso(new Date('2026-05-20')), flowId: 'fl_kyc' },
    { id: 'sc_hello', code: '*565#', label: 'Promo Blast', network: 'MTN', status: 'provisioning', plan: 'starter', sessionsUsed: 312, sessionsQuota: 10_000, createdAt: iso(new Date('2026-09-01')), flowId: 'fl_hello' },
  ]

  // Flows seeded from the engine's own example blueprints — the builder graph
  // is derived by importing the real DSL JSON.
  const mkFlow = (id: string, name: string, description: string, exampleFile: string | null, shortcodeId?: string): Flow => {
    let graph
    let meta = { name: `examples.${id.replace('fl_', '')}`, description, version: '1.0' }
    const raw = exampleFile ? loadExample(exampleFile) : null
    if (raw) {
      graph = blueprintToGraph(raw as never)
      meta = {
        name: String(raw.name ?? meta.name),
        description: String(raw.description ?? description),
        version: String(raw.version ?? '1.0'),
      }
    } else {
      graph = { entryId: null, nodes: [], edges: [] }
    }
    const flow: Flow = {
      id, name, description, shortcodeId, createdAt: iso(new Date(now.getTime() - 90 * 864e5)), updatedAt: iso(now),
      graph, meta: { name: meta.name, version: meta.version, defaults: (raw?.defaults as Record<string, unknown>) ?? {} }, builds: [], releases: [],
    }
    // give each flow a build + active release history
    const bpMeta = { name: meta.name, description: meta.description, version: meta.version, defaults: (raw?.defaults as Record<string, unknown>) ?? {} }
    const bp = graphToBlueprint(flow.graph, bpMeta)
    const v = validateBlueprint(bp, flow.graph)
    const build: Build = {
      id: rid('b'), number: 1, runId: `${formatRunDate(now)}.1`, status: v.ok ? 'succeeded' : 'failed',
      createdAt: iso(new Date(now.getTime() - 6 * 864e5)), durationMs: 420, triggeredBy: 'ama@rhyoliteprime.com',
      blueprint: bp, errors: v.errors, warnings: v.warnings, notes: 'Initial import from Sapo DSL example',
    }
    const release: Release = {
      id: rid('r'), tag: 'v1.0.0', name: 'Initial release', status: 'active', buildId: build.id, buildRunId: build.runId,
      notes: 'Imported from examples/ and validated against the Sapo VM schema.', releasedAt: iso(new Date(now.getTime() - 6 * 864e5)),
      releasedBy: 'ama@rhyoliteprime.com',
    }
    flow.builds = [build]
    flow.releases = [release]
    return flow
  }

  const flows: Flow[] = [
    mkFlow('fl_daccu', 'DACCU Banking Service', 'Multi-level USSD banking menus with live API integration and resilient error handling.', 'daccu_ussd_service.json', 'sc_daccu'),
    mkFlow('fl_momo', 'MoMo Transfer Guard', 'Guarded mobile-money transfer: debit with refund-on-failure, receipt by finally, degraded queueing.', 'momo_transfer_with_guard.json', 'sc_momo'),
    mkFlow('fl_kyc', 'KYC Re-check Flow', 'PIN prompt that survives restarts, validates the reply and degrades when the KYC API is down.', 'kyc_ussd_flow.json', 'sc_kyc'),
    mkFlow('fl_hello', 'Promo Blast', 'Small promotional flow — currently a draft on the canvas.', null, 'sc_hello'),
  ]

  const members: Member[] = [
    { id: 'm_1', name: 'Ama Mensah', email: 'ama@rhyoliteprime.com', role: 'owner', joinedAt: iso(new Date('2025-11-02')), status: 'active' },
    { id: 'm_2', name: 'Kwame Boateng', email: 'kwame@rhyoliteprime.com', role: 'editor', joinedAt: iso(new Date('2026-01-11')), status: 'active' },
    { id: 'm_3', name: 'Zainab Musah', email: 'zainab@rhyoliteprime.com', role: 'viewer', joinedAt: iso(new Date('2026-03-05')), status: 'active' },
  ]

  const invites: Invite[] = [
    { id: rid('inv'), email: 'nii@daccultd.com', role: 'editor', invitedBy: 'ama@rhyoliteprime.com', invitedAt: iso(new Date(now.getTime() - 3 * 864e5)), token: 'inv-' + randomUUID().slice(0, 10), status: 'pending' },
  ]

  // --- usage stats (deterministic pseudo-random series over 30 days) ---------
  let seedN = 42
  const rnd = () => { seedN = (seedN * 1103515245 + 12345) % 2147483648; return seedN / 2147483648 }
  const days: StatsDay[] = []
  const activeCodes = shortcodes.filter((s) => s.status === 'active')
  for (let d = 29; d >= 0; d--) {
    const date = new Date(now.getTime() - d * 864e5)
    const weekend = [0, 6].includes(date.getDay())
    for (const sc of activeCodes) {
      const base = sc.id === 'sc_daccu' ? 9200 : sc.id === 'sc_momo' ? 2100 : 700
      const sessions = Math.round(base * (weekend ? 0.55 : 1) * (0.82 + rnd() * 0.45))
      const completed = Math.round(sessions * (0.86 + rnd() * 0.09))
      const rate = sc.plan === 'scale' ? 0.02 : 0.035
      days.push({
        date: date.toISOString().slice(0, 10), shortcodeId: sc.id, sessions,
        completed, failed: sessions - completed,
        revenue: Math.round(sessions * rate * 100) / 100,
        avgDurationSec: Math.round((38 + rnd() * 30) * 10) / 10,
      })
    }
  }
  const hourlyToday = Array.from({ length: 24 }, (_, h) => {
    const dayShape = h < 6 ? 0.15 : h < 9 ? 0.7 : h < 12 ? 1.0 : h < 15 ? 0.85 : h < 19 ? 1.15 : h < 22 ? 0.65 : 0.2
    return Math.round(280 * dayShape * (0.8 + rnd() * 0.4))
  })

  const invoices: Invoice[] = [
    {
      id: rid('in'), number: 'SCE-2026-0092', period: 'September 2026', issuedAt: iso(new Date('2026-09-01')), dueAt: iso(new Date('2026-09-15')), status: 'due',
      lines: [
        { label: 'Scale plan — *920*108#', qty: 1, unit: 'month', amount: 1499 },
        { label: 'Growth plan — *713*9#', qty: 1, unit: 'month', amount: 499 },
        { label: 'Growth plan — *384*2#', qty: 1, unit: 'month', amount: 499 },
        { label: 'Overage — *713*9# (12,442 sessions × GHS 0.035)', qty: 12442, unit: 'session', amount: Math.round(12442 * 0.035 * 100) / 100 },
      ],
      total: 1499 + 499 + 499 + Math.round(12442 * 0.035 * 100) / 100,
    },
    {
      id: rid('in'), number: 'SCE-2026-0087', period: 'August 2026', issuedAt: iso(new Date('2026-08-01')), dueAt: iso(new Date('2026-08-15')), status: 'paid',
      lines: [
        { label: 'Scale plan — *920*108#', qty: 1, unit: 'month', amount: 1499 },
        { label: 'Growth plan — *713*9#', qty: 1, unit: 'month', amount: 499 },
        { label: 'Growth plan — *384*2#', qty: 1, unit: 'month', amount: 499 },
      ],
      total: 2497,
    },
    {
      id: rid('in'), number: 'SCE-2026-0081', period: 'July 2026', issuedAt: iso(new Date('2026-07-01')), dueAt: iso(new Date('2026-07-15')), status: 'paid',
      lines: [
        { label: 'Scale plan — *920*108#', qty: 1, unit: 'month', amount: 1499 },
        { label: 'Growth plan — *713*9#', qty: 1, unit: 'month', amount: 499 },
      ],
      total: 1998,
    },
  ]

  return {
    users, flows, shortcodes, members, invites, invoices, plans: PLANS, checkouts: [], webhookDeliveries: [],
    stats: {
      days,
      networks: { MTN: 58, 'AirtelTigo': 24, Vodafone: 18 },
      hourlyToday,
      topFlows: flows.slice(0, 3).map((f, i) => ({ flowId: f.id, sessions: [288_410, 51_204, 16_845][i] ?? 1000, completion: [92.4, 88.1, 84.9][i] ?? 90 })),
    },
    business: { name: 'Rhyolite Prime', currency: 'GHS', billingEmail: 'finance@rhyoliteprime.com', country: 'Ghana' },
  }
}

function formatRunDate(d: Date): string {
  return `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, '0')}${String(d.getDate()).padStart(2, '0')}`
}

export { formatRunDate }

let cache: DB | null = null

/**
 * Upgrade a persisted DB to the current shape. Deployments that ran older
 * builds keep their .data/db.json around — backfill anything added since
 * (e.g. `checkouts` / `webhookDeliveries` arrived with the webcheckout
 * release, `apiKeys` with developer signup) instead of crashing on it.
 */
function migrateDb(loaded: Partial<DB>): { db: DB; changed: boolean } {
  let changed = false
  const ensureArray = <T>(value: T[] | undefined): T[] => {
    if (Array.isArray(value)) return value
    changed = true
    return []
  }

  const db = loaded as DB
  db.users = ensureArray(db.users)
  for (const u of db.users) {
    if (!Array.isArray(u.apiKeys)) { u.apiKeys = []; changed = true }
  }
  db.flows = ensureArray(db.flows)
  db.shortcodes = ensureArray(db.shortcodes)
  db.members = ensureArray(db.members)
  db.invites = ensureArray(db.invites)
  db.invoices = ensureArray(db.invoices)
  db.checkouts = ensureArray(db.checkouts)
  db.webhookDeliveries = ensureArray(db.webhookDeliveries)

  if (!db.stats || typeof db.stats !== 'object') { db.stats = { days: [], networks: {}, hourlyToday: [], topFlows: [] }; changed = true }
  if (!Array.isArray(db.stats.days)) { db.stats.days = []; changed = true }
  if (!db.stats.networks || typeof db.stats.networks !== 'object') { db.stats.networks = {}; changed = true }
  if (!Array.isArray(db.stats.hourlyToday) || db.stats.hourlyToday.length !== 24) {
    db.stats.hourlyToday = Array.from({ length: 24 }, (_, i) => db.stats.hourlyToday?.[i] ?? 0)
    changed = true
  }
  if (!Array.isArray(db.stats.topFlows)) { db.stats.topFlows = []; changed = true }

  if (!db.business || typeof db.business !== 'object') {
    db.business = { name: 'ShortCodeExpress', currency: 'GHS', billingEmail: 'billing@example.com', country: 'Ghana' }
    changed = true
  }

  return { db, changed }
}

export function useDb(): DB {
  if (cache) return cache
  if (fs.existsSync(DB_FILE)) {
    try {
      const loaded = JSON.parse(fs.readFileSync(DB_FILE, 'utf-8')) as Partial<DB>
      const { db, changed } = migrateDb(loaded)
      // plans are code-defined; keep them fresh
      db.plans = PLANS
      cache = db
      if (changed) saveDb(cache) // persist the upgrade once
      return cache
    } catch {
      // corrupted file -> reseed
    }
  }
  cache = seed()
  saveDb(cache)
  return cache
}

export function saveDb(db: DB): void {
  fs.mkdirSync(DATA_DIR, { recursive: true })
  fs.writeFileSync(DB_FILE, JSON.stringify(db, null, 2))
}

export { rid }
