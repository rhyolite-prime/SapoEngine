// ---------------------------------------------------------------------------
// ShortCodeExpress shared types — used by both the Nuxt app and the Nitro API
// ---------------------------------------------------------------------------

/** Sapo DSL node types (grammar v1) */
export type SapoNodeType =
  | 'noop' | 'transform' | 'query' | 'command' | 'event' | 'wait' | 'schedule'
  | 'subflow' | 'script' | 'condition' | 'if' | 'choice' | 'switch' | 'parallel'
  | 'action' | 'loop' | 'break' | 'continue' | 'loop_control' | 'try' | 'terminate'

/** UI palette node kinds — each maps onto one or more Sapo DSL node types */
export type NodeKind =
  | 'menu' | 'input' | 'pin' | 'display' | 'await_event'
  | 'http' | 'subflow'
  | 'if' | 'choice' | 'try' | 'loop' | 'parallel' | 'script' | 'break'
  | 'assign' | 'transform' | 'query' | 'wait' | 'schedule' | 'event'
  | 'end_success' | 'end_failure'

export interface MenuOption { id: string; label: string; value: string }
export interface KVPair { id: string; key: string; value: string }
export interface ChoiceCase { id: string; label: string; value: string }

export interface FlowNodeData {
  label?: string
  // prompt-based nodes
  inputVariable?: string
  message?: string
  autoMessage?: boolean
  options?: MenuOption[]
  validationMode?: 'off' | 'auto' | 'custom'
  customValidation?: string
  timeout?: string
  // await_event
  eventName?: string
  triggerCondition?: string
  // http
  verb?: 'get' | 'post' | 'put' | 'patch' | 'delete'
  url?: string
  headers?: KVPair[]
  queryParams?: KVPair[]
  bodyJson?: string
  retryEnabled?: boolean
  retry?: { max_attempts: number; backoff_ms: number; multiplier: number; jitter: number }
  outputs?: KVPair[] // key -> $.body.path
  // subflow
  workflow?: string
  inputsJson?: string
  returnJson?: string
  // control flow
  condition?: string
  expression?: string
  cases?: ChoiceCase[]
  errorVariable?: string
  loopMode?: 'collection' | 'count' | 'while'
  collection?: string
  count?: string
  while?: string
  iterator?: string
  indexVar?: string
  maxIterations?: number
  onItemError?: 'fail' | 'continue'
  mergePolicy?: 'wait_all' | 'fail_fast' | 'quorum'
  failFast?: boolean
  scriptCode?: string
  bindingsJson?: string
  breakAction?: 'break' | 'continue'
  // data nodes
  assignments?: KVPair[]
  operation?: string
  inputExpr?: string
  mappingJson?: string
  where?: string
  outputKey?: string
  asArray?: boolean
  source?: string
  filter?: string
  queryStatement?: string
  paramsJson?: string
  limit?: number
  offset?: number
  // wait / schedule
  duration?: string
  until?: string
  pollInterval?: string
  cron?: string
  timezone?: string
  jobId?: string
  // event
  payloadJson?: string
  // terminate
  status?: 'success' | 'failure'
  endMessage?: string
  endOutputs?: KVPair[]
}

export interface FlowNode {
  id: string
  kind: NodeKind
  position: { x: number; y: number }
  data: FlowNodeData
}

export interface FlowEdge {
  id: string
  source: string
  sourceHandle: string // 'next' | 'error' | 'then' | 'else' | 'option:<optId>' | 'case:<caseId>' | 'body' | 'catch' | 'finally' | 'branch:<n>'
  target: string
}

export interface FlowGraph {
  entryId: string | null
  nodes: FlowNode[]
  edges: FlowEdge[]
}

export interface FlowDefaults {
  name: string
  description: string
  version: string
  defaults: Record<string, unknown>
}

// --- Blueprint (the Sapo DSL JSON that runs on the Sapo VM) ---

export interface SapoNode { [key: string]: unknown }
export interface SapoBlueprint {
  name?: string
  version?: string
  description?: string
  defaults?: Record<string, unknown>
  trigger?: { event?: string; input?: Record<string, unknown> }
  nodes: SapoNode[]
}

// --- Validation ---

export interface ValidationIssue {
  level: 'error' | 'warning'
  nodeId?: string
  message: string
}

export interface ValidationResult {
  ok: boolean
  errors: ValidationIssue[]
  warnings: ValidationIssue[]
}

// --- Build & release model (Azure-style pipeline) ---

export interface Build {
  id: string
  number: number // build number, e.g. 14
  runId: string // 20260928.3
  status: 'succeeded' | 'failed' | 'running'
  createdAt: string
  durationMs: number
  triggeredBy: string
  blueprint: SapoBlueprint
  errors: ValidationIssue[]
  warnings: ValidationIssue[]
  notes?: string
}

export interface Release {
  id: string
  tag: string // v1.0.2
  name: string
  status: 'active' | 'superseded' | 'rolled-back'
  buildId: string
  buildRunId: string
  notes: string
  releasedAt: string
  releasedBy: string
  rollbackOf?: string // when this release is a rollback of a previous release
}

// --- Domain: flows, shortcodes, team, billing ---

export interface Flow {
  id: string
  name: string
  description: string
  shortcodeId?: string
  createdAt: string
  updatedAt: string
  graph: FlowGraph
  meta: { name: string; version: string; defaults: Record<string, unknown> }
  builds: Build[]
  releases: Release[]
}

export type PlanId = 'starter' | 'growth' | 'scale'

export interface Plan {
  id: PlanId
  name: string
  priceMonthly: number
  sessionQuota: number
  overagePerSession: number
  maxShortcodes: number
  features: string[]
}

/** A port-in request: the customer pays, then shares `interactionUrl` with
 *  their current (donor) provider, who approves/rejects via that URL. */
export interface PortRequest {
  /** donor provider name, e.g. "Hubtel" */
  provider: string
  /** secret token of the provider interaction URL (/port/:token) */
  token: string
  requestedAt: string
  approvedAt?: string
  rejectedAt?: string
  rejectedReason?: string
}

export interface ShortCode {
  id: string
  code: string // *920*108#
  label: string
  network: string
  status: 'active' | 'provisioning' | 'suspended' | 'porting'
  plan: PlanId
  sessionsUsed: number
  sessionsQuota: number
  createdAt: string
  flowId?: string
  ownerId?: string
  assignedBy?: 'user' | 'system' | 'port'
  setupFeePaid?: number
  /** present on ported codes: flat monthly billing, unlimited sessions */
  flatMonthly?: number
  port?: PortRequest
}

export type MemberRole = 'owner' | 'editor' | 'viewer'

export interface Member {
  id: string
  name: string
  email: string
  role: MemberRole
  joinedAt: string
  status: 'active' | 'invited'
}

export interface Invite {
  id: string
  email: string
  role: MemberRole
  invitedBy: string
  invitedAt: string
  token: string
  status: 'pending' | 'accepted'
}

export interface InvoiceLine {
  label: string
  qty: number
  unit: string
  amount: number
}

export interface Invoice {
  id: string
  number: string
  period: string
  issuedAt: string
  dueAt: string
  status: 'paid' | 'due' | 'overdue'
  lines: InvoiceLine[]
  total: number
}

export interface User {
  id: string
  name: string
  email: string
  role: string
  avatarHue: number
  title: string
  company?: string
  createdAt?: string
  apiKeys?: ApiKey[]
  webhook?: WebhookEndpoint
}

// --- Developer platform: API keys, webcheckout, webhooks ---

export interface ApiKey {
  id: string
  name: string
  key: string // sk_live_… (demo store keeps it visible)
  createdAt: string
  lastUsedAt?: string
  revoked?: boolean
}

export type WebhookEventName =
  | 'test.ping'
  | 'payment.succeeded'
  | 'shortcode.assigned'
  | 'shortcode.ported'
  | 'sessions.topped_up'
  | 'session.started'
  | 'session.completed'
  | 'session.failed'
  | 'flow.http_request'

export interface WebhookEndpoint {
  url: string
  secret: string
  events: WebhookEventName[]
  active: boolean
}

export interface WebhookDelivery {
  id: string
  userId: string
  eventId: string
  event: WebhookEventName
  url: string
  payload: Record<string, unknown>
  signature: string
  status: 'delivered' | 'failed'
  responseStatus?: number
  error?: string
  createdAt: string
}

export interface CheckoutItem {
  label: string
  detail?: string
  qty: number
  unit: string
  amount: number
}

export type CheckoutKind = 'shortcode' | 'topup' | 'port'

export interface CheckoutSession {
  id: string
  kind: CheckoutKind
  status: 'pending' | 'paid' | 'failed' | 'expired'
  userId: string
  items: CheckoutItem[]
  total: number
  currency: 'GHS'
  /** shortcode: { mode, code?, label, network, planId } · topup: { shortcodeId, packId, sessions } · port: { code, label, network, provider } */
  meta: Record<string, unknown>
  createdAt: string
  paidAt?: string
  paymentMethod?: 'momo' | 'card'
  failureReason?: string
  /** set on fulfilment */
  result?: Record<string, unknown>
}

export interface SessionPack {
  id: string
  sessions: number
  price: number
  label: string
  perSession: number
}
