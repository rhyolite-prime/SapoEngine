// ---------------------------------------------------------------------------
// Starter flow — auto-provisioned with every new short code so a developer
// can dial live within minutes of signing up.
//
// It is defined as a CANVAS graph, deliberately: it goes through the exact
// same graphToBlueprint() serializer as anything composed by hand, is fully
// editable in the builder, and showcases dynamic menu rendering (${var}
// templates), a live HTTP integration, and a charge that fires a webhook.
// ---------------------------------------------------------------------------
import { useDb, saveDb, rid, formatRunDate } from './db'
import { graphToBlueprint, validateBlueprint, nodeRefId } from '../../shared/utils/sapo'
import type { Build, Flow, FlowGraph, Release, ShortCode, User } from '../../shared/types'

export function starterGraph(business: string, code: string): { name: string; description: string; meta: { name: string; version: string; defaults: Record<string, unknown> }; graph: FlowGraph } {
  const slug = business.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '') || 'my_service'
  const g = withSystemRefs(buildGraph(business, code))
  return {
    name: `${business} — USSD service`,
    description: `Starter service auto-generated for ${code}: dynamic menus, a live balance API call and a bundle purchase that fires a payment webhook. Edit it visually — the blueprint stays 1:1.`,
    meta: {
      name: `sce.${slug}.welcome`,
      version: '1.0',
      defaults: { business, code },
    },
    graph: g,
  }
}

/** every node gets a system-assigned 6-digit reference, like any node added on the canvas */
function withSystemRefs(graph: FlowGraph): FlowGraph {
  const taken = new Set<string>()
  const idMap = new Map<string, string>()
  for (const n of graph.nodes) idMap.set(n.id, nodeRefId(taken))
  const idOf = (old: string) => idMap.get(old) ?? old
  return {
    entryId: graph.entryId ? idOf(graph.entryId) : null,
    nodes: graph.nodes.map((n) => ({ ...n, id: idOf(n.id) })),
    edges: graph.edges.map((e) => ({ ...e, source: idOf(e.source), target: idOf(e.target) })),
  }
}

function buildGraph(business: string, code: string): FlowGraph {
  return {
      entryId: 'welcome',
      nodes: [
        {
          id: 'welcome', kind: 'menu', position: { x: 40, y: 300 },
          data: {
            label: 'Welcome menu', inputVariable: 'choice', autoMessage: true,
            options: [
              { id: 'opt_bal', label: 'My balance', value: '1' },
              { id: 'opt_bundle', label: 'Buy a data bundle', value: '2' },
              { id: 'opt_exit', label: 'Exit', value: '3' },
            ],
            validationMode: 'auto', timeout: '2m',
          },
        },
        {
          id: 'fetch_balance', kind: 'http', position: { x: 380, y: 80 },
          data: {
            label: 'Fetch balance (live API)', verb: 'get', url: '/api/mock/account',
            queryParams: [{ id: 'qp1', key: 'msisdn', value: '${msisdn}' }],
            outputs: [
              { id: 'o1', key: 'account_name', value: '$.body.name' },
              { id: 'o2', key: 'balance', value: '$.body.balance' },
            ],
            retryEnabled: true, retry: { max_attempts: 2, backoff_ms: 200, multiplier: 2, jitter: 40 },
          },
        },
        {
          id: 'show_balance', kind: 'input', position: { x: 700, y: 80 },
          data: {
            label: 'Show balance (dynamic)', inputVariable: 'back',
            message: 'Hi ${account_name}!\nYour balance is GHS ${balance}.\n\n1. Back',
            validationMode: 'off', timeout: '2m',
          },
        },
        {
          id: 'bundles', kind: 'menu', position: { x: 380, y: 300 },
          data: {
            label: 'Bundle menu', inputVariable: 'bundle_choice', autoMessage: true,
            options: [
              { id: 'opt_1gb', label: '1GB @ GHS 5', value: '1' },
              { id: 'opt_5gb', label: '5GB @ GHS 20', value: '2' },
              { id: 'opt_back', label: 'Back', value: '3' },
            ],
            validationMode: 'auto', timeout: '2m',
          },
        },
        {
          id: 'set_1gb', kind: 'assign', position: { x: 700, y: 260 },
          data: { label: 'Select 1GB', assignments: [{ id: 'a1', key: 'bundle_name', value: '1GB Data Bundle' }, { id: 'a2', key: 'amount', value: '5' }] },
        },
        {
          id: 'set_5gb', kind: 'assign', position: { x: 700, y: 360 },
          data: { label: 'Select 5GB', assignments: [{ id: 'a1', key: 'bundle_name', value: '5GB Data Bundle' }, { id: 'a2', key: 'amount', value: '20' }] },
        },
        {
          id: 'confirm', kind: 'input', position: { x: 980, y: 300 },
          data: {
            label: 'Confirm purchase', inputVariable: 'confirm',
            message: 'Buy ${bundle_name} for GHS ${amount}?\n\n1. Confirm\n2. Cancel',
            validationMode: 'off', timeout: '2m',
          },
        },
        {
          id: 'confirm_check', kind: 'if', position: { x: 1260, y: 300 },
          data: { label: 'Confirmed?', condition: "confirm == '1'" },
        },
        {
          id: 'charge', kind: 'http', position: { x: 1540, y: 220 },
          data: {
            label: 'Charge via MoMo', verb: 'post', url: '/api/mock/pay',
            bodyJson: '{\n  "msisdn": "${msisdn}",\n  "amount": "${amount}",\n  "description": "${bundle_name}",\n  "shortcode": "${code}"\n}',
            headers: [{ id: 'h1', key: 'content-type', value: 'application/json' }],
            outputs: [
              { id: 'o1', key: 'transaction_id', value: '$.body.transactionId' },
              { id: 'o2', key: 'payment_status', value: '$.body.status' },
            ],
            retryEnabled: true, retry: { max_attempts: 2, backoff_ms: 300, multiplier: 2, jitter: 50 },
          },
        },
        {
          id: 'receipt', kind: 'input', position: { x: 1860, y: 220 },
          data: {
            label: 'Receipt (dynamic)', inputVariable: 'back',
            message: 'Payment received!\n${bundle_name} is now active.\nRef: ${transaction_id}\n\n1. Back',
            validationMode: 'off', timeout: '2m',
          },
        },
        {
          id: 'cancelled', kind: 'end_success', position: { x: 1540, y: 420 },
          data: { label: 'Cancelled', endMessage: 'Order cancelled. Dial again any time!' },
        },
        {
          id: 'bye', kind: 'end_success', position: { x: 380, y: 520 },
          data: { label: 'Goodbye', endMessage: 'Thank you for using ${business}!' },
        },
      ],
      edges: [
        { id: 'e1', source: 'welcome', sourceHandle: 'option:opt_bal', target: 'fetch_balance' },
        { id: 'e2', source: 'welcome', sourceHandle: 'option:opt_bundle', target: 'bundles' },
        { id: 'e3', source: 'welcome', sourceHandle: 'option:opt_exit', target: 'bye' },
        { id: 'e4', source: 'fetch_balance', sourceHandle: 'next', target: 'show_balance' },
        { id: 'e5', source: 'show_balance', sourceHandle: 'next', target: 'welcome' },
        { id: 'e6', source: 'bundles', sourceHandle: 'option:opt_1gb', target: 'set_1gb' },
        { id: 'e7', source: 'bundles', sourceHandle: 'option:opt_5gb', target: 'set_5gb' },
        { id: 'e8', source: 'bundles', sourceHandle: 'option:opt_back', target: 'welcome' },
        { id: 'e9', source: 'set_1gb', sourceHandle: 'next', target: 'confirm' },
        { id: 'e10', source: 'set_5gb', sourceHandle: 'next', target: 'confirm' },
        { id: 'e11', source: 'confirm', sourceHandle: 'next', target: 'confirm_check' },
        { id: 'e12', source: 'confirm_check', sourceHandle: 'then', target: 'charge' },
        { id: 'e13', source: 'confirm_check', sourceHandle: 'else', target: 'cancelled' },
        { id: 'e14', source: 'charge', sourceHandle: 'next', target: 'receipt' },
        { id: 'e15', source: 'receipt', sourceHandle: 'next', target: 'welcome' },
        { id: 'e16', source: 'charge', sourceHandle: 'error', target: 'cancelled' },
      ],
  }
}

/**
 * Creates the starter flow for a freshly-purchased short code, runs the
 * standard build (serialize + validate) and releases v1.0.0 — so the code is
 * dialable the second checkout completes.
 */
export function provisionStarterFlow(db: ReturnType<typeof useDb>, user: User, shortcode: ShortCode): Flow {
  const business = shortcode.label || user.company || user.name.split(' ')[0] + "'s Service"
  const tpl = starterGraph(business, shortcode.code)
  const now = new Date().toISOString()

  const blueprint = graphToBlueprint(tpl.graph, {
    name: tpl.meta.name,
    description: tpl.description,
    version: tpl.meta.version,
    defaults: tpl.meta.defaults,
  })
  const validation = validateBlueprint(blueprint, tpl.graph)

  const build: Build = {
    id: rid('b'),
    number: 1,
    runId: `${formatRunDate(new Date())}.1`,
    status: validation.ok ? 'succeeded' : 'failed',
    createdAt: now,
    durationMs: 260,
    triggeredBy: user.email,
    blueprint,
    errors: validation.errors,
    warnings: validation.warnings,
    notes: 'Auto-generated starter service — dynamic menus, live API + payment webhook.',
  }
  const release: Release = {
    id: rid('r'),
    tag: 'v1.0.0',
    name: 'Starter release',
    status: 'active',
    buildId: build.id,
    buildRunId: build.runId,
    notes: 'Auto-released on short code purchase. Edit the flow and ship your own version.',
    releasedAt: now,
    releasedBy: user.email,
  }

  const flow: Flow = {
    id: rid('fl'),
    name: tpl.name,
    description: tpl.description,
    shortcodeId: shortcode.id,
    createdAt: now,
    updatedAt: now,
    graph: tpl.graph,
    meta: tpl.meta,
    builds: [build],
    releases: [release],
  }
  db.flows.push(flow)
  shortcode.flowId = flow.id
  saveDb(db)
  return flow
}
