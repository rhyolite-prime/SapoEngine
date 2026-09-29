<script setup lang="ts">
import { SignalIcon, ArrowPathIcon, BoltIcon, ArrowPathRoundedSquareIcon } from '@heroicons/vue/24/outline'
import type { WebhookDelivery } from '~/../shared/types'

definePageMeta({ middleware: 'auth' })
useHead({ title: 'Live dialer · ShortCodeExpress' })

interface DialResponse {
  sessionId: string
  status: 'idle' | 'running' | 'awaiting_input' | 'awaiting_event' | 'completed' | 'failed'
  screen: string[]
  prompt: { message: string; interactionType: string } | null
  vars: Record<string, unknown>
  shortcode: { code: string; sessionsUsed: number; sessionsQuota: number; label: string }
}

const route = useRoute()
const dialInput = ref<string>(String(route.query.code ?? ''))
const msisdn = ref('0244123456')
const network = ref('MTN')

const session = ref<DialResponse | null>(null)
const dialing = ref(false)
const error = ref('')
const callElapsed = ref<number | null>(null)
let startedAt = 0
let tickTimer: ReturnType<typeof setInterval> | undefined

// webhook feed (live polling while a session runs)
const deliveries = ref<WebhookDelivery[]>([])
let lastDeliveryId = ''
let pollTimer: ReturnType<typeof setInterval> | undefined
async function pollWebhooks() {
  try {
    const res = await $fetch<{ deliveries: WebhookDelivery[] }>('/api/webhooks')
    const fresh = res.deliveries ?? []
    if (fresh[0] && fresh[0].id !== lastDeliveryId) lastDeliveryId = fresh[0].id
    deliveries.value = fresh.slice(0, 8)
  } catch { /* ignore */ }
}

const codes = ref<Array<{ id: string; code: string; label: string; flowName: string | null; mine: boolean }>>([])
async function loadCodes() {
  const rows = await $fetch<Array<{ id: string; code: string; label: string; flowName: string | null; ownerId?: string }>>('/api/shortcodes')
  const me = await $fetch<{ id: string }>('/api/auth/me').catch(() => null)
  codes.value = rows.map((r) => ({ ...r, flowName: r.flowName ?? null, mine: !!me && r.ownerId === me.id }))
}
await loadCodes()
await pollWebhooks()

async function dial() {
  const code = dialInput.value.replace(/\s+/g, '')
  if (!code) return
  error.value = ''
  dialing.value = true
  session.value = null
  try {
    session.value = await $fetch<DialResponse>('/api/ussd/dial', { method: 'POST', body: { code, msisdn: msisdn.value, network: network.value } })
    startedAt = Date.now()
    callElapsed.value = 0
    clearInterval(tickTimer)
    tickTimer = setInterval(() => { callElapsed.value = Math.round((Date.now() - startedAt) / 1000) }, 1000)
    clearInterval(pollTimer)
    pollTimer = setInterval(pollWebhooks, 2500)
  } catch (e: unknown) {
    const err = (e as { data?: { statusMessage?: string; data?: { code?: string; shortcodeId?: string } }, statusCode?: number })
    error.value = err.data?.statusMessage ?? 'Dial failed'
    if (err.data?.data?.code === 'quota_exceeded') error.value = `Quota exhausted on ${code} — top up sessions to keep serving.`
  } finally {
    dialing.value = false
  }
}

async function send(text: string) {
  if (!session.value) return
  error.value = ''
  try {
    session.value = await $fetch<DialResponse>('/api/ussd/input', { method: 'POST', body: { sessionId: session.value.sessionId, text } })
  } catch (e: unknown) {
    error.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not send'
  }
}

function hangup() {
  session.value = null
  callElapsed.value = null
  clearInterval(tickTimer)
  clearInterval(pollTimer)
}
onUnmounted(() => { clearInterval(tickTimer); clearInterval(pollTimer) })

const keypad = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#']
function press(k: string) {
  if (session.value && (session.value.status === 'awaiting_input' || session.value.status === 'awaiting_event')) send(k)
  else dialInput.value = (dialInput.value + k).slice(0, 14)
}
const screenText = computed(() => session.value?.screen.join('\n\n────\n\n') ?? '')
const sessionLive = computed(() => !!session.value && ['awaiting_input', 'awaiting_event', 'running'].includes(session.value.status))
const publicVars = computed(() => Object.fromEntries(Object.entries(session.value?.vars ?? {}).filter(([k]) => ['msisdn', 'network', 'business', 'code', 'balance', 'account_name', 'bundle_name', 'amount', 'confirm', 'choice', 'bundle_choice', 'transaction_id', 'payment_status', 'input', 'back'].includes(k))))
const eventColor: Record<string, string> = {
  'payment.succeeded': 'bg-success-500', 'session.started': 'bg-brand-500', 'session.completed': 'bg-success-600',
  'session.failed': 'bg-rose-500', 'flow.http_request': 'bg-sky-500', 'shortcode.assigned': 'bg-brand-400',
  'sessions.topped_up': 'bg-amber-500', 'test.ping': 'bg-slate-400',
}
</script>

<template>
  <div class="p-8">
    <div class="mb-6 flex flex-wrap items-end justify-between gap-3">
      <div>
        <h1 class="text-2xl font-bold text-slate-900">Live dialer</h1>
        <p class="mt-1 text-sm text-slate-500">Dial any provisioned code and drive the released flow over the live gateway API — with webhooks firing as you go.</p>
      </div>
      <NuxtLink to="/developers" class="inline-flex items-center gap-1.5 rounded-lg bg-white px-3.5 py-2 text-xs font-bold text-slate-700 ring-1 ring-slate-200 hover:bg-slate-50">
        <BoltIcon class="h-3.5 w-3.5 text-brand-600" /> API &amp; webhook docs
      </NuxtLink>
    </div>

    <div class="grid gap-6 lg:grid-cols-[380px_1fr]">
      <!-- PHONE -->
      <div class="mx-auto w-full max-w-[380px]">
        <div class="rounded-[2.5rem] bg-slate-900 p-3 shadow-2xl ring-1 ring-white/10">
          <!-- earpiece -->
          <div class="mb-2 flex items-center justify-center gap-2 px-4 pt-1">
            <span class="h-1.5 w-12 rounded-full bg-white/10"></span>
            <span class="text-[10px] font-semibold text-slate-500">{{ network }} · {{ msisdn }}</span>
          </div>

          <!-- screen -->
          <div class="relative min-h-[240px] rounded-3xl bg-gradient-to-b from-[#0b3d2e] to-[#062a1f] p-4 font-mono text-[13px] leading-snug text-success-200 ring-1 ring-success-900/50">
            <div class="flex items-center justify-between text-[10px] text-success-500/70">
              <span>USSD RUNNER</span>
              <span class="flex items-center gap-1">
                <span v-if="sessionLive" class="inline-block h-1.5 w-1.5 animate-pulse rounded-full bg-success-400"></span>
                {{ session ? session.status.replace('_', ' ') : 'ready' }}
                <span v-if="callElapsed !== null" class="ml-1 text-success-400">{{ callElapsed }}s</span>
              </span>
            </div>

            <pre v-if="session" class="mt-2 max-h-[290px] overflow-y-auto whitespace-pre-wrap break-words">{{ screenText || '(no screen yet)' }}</pre>
            <div v-else-if="dialing" class="mt-10 text-center">
              <svg class="mx-auto h-8 w-8 animate-spin text-success-400" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
              <p class="mt-3 text-xs text-success-500">Dialing {{ dialInput }}…</p>
            </div>
            <div v-else class="mt-10 text-center text-xs text-success-600/70">
              <p class="font-bold text-success-500">{{ dialInput || 'Enter a short code' }}</p>
              <p class="mt-2">Press the green key to dial.</p>
            </div>

            <div v-if="session?.prompt" class="absolute inset-x-3 bottom-3 rounded-xl bg-black/40 px-3 py-2 text-[10px] text-success-500 ring-1 ring-success-900/60">
              waiting for {{ session.prompt.interactionType }} — press a key below
            </div>
          </div>

          <!-- error strip -->
          <p v-if="error" class="mt-2 rounded-xl bg-rose-500/15 px-3 py-2 text-center text-[11px] font-semibold text-rose-300">{{ error }}</p>

          <!-- keypad -->
          <div class="mt-3 grid grid-cols-3 gap-2 px-1">
            <button v-for="k in keypad" :key="k" class="rounded-2xl bg-white/[0.07] py-3 font-mono text-lg font-bold text-slate-100 transition hover:bg-white/15 active:scale-95" @click="press(k)">{{ k }}</button>
            <button class="flex items-center justify-center rounded-2xl bg-rose-500/80 py-3 text-xs font-bold text-white transition hover:bg-rose-500 active:scale-95" @click="hangup">END</button>
            <button class="flex items-center justify-center rounded-2xl py-3 text-white transition active:scale-95"
              :class="dialing ? 'bg-success-500/40' : 'bg-success-500 hover:bg-success-400'" :disabled="dialing" @click="dial">
              <svg v-if="dialing" class="h-5 w-5 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
              <svg v-else class="h-5 w-5" viewBox="0 0 24 24" fill="currentColor"><path d="M6.62 10.79a15.05 15.05 0 0 0 6.59 6.59l2.2-2.2a1 1 0 0 1 1.02-.24c1.12.37 2.33.57 3.57.57a1 1 0 0 1 1 1V20a1 1 0 0 1-1 1C10.85 21 3 13.15 3 3.5a1 1 0 0 1 1-1h3.5a1 1 0 0 1 1 1c0 1.24.2 2.45.57 3.57a1 1 0 0 1-.25 1.02l-2.2 2.2Z" /></svg>
            </button>
            <button class="rounded-2xl bg-white/[0.07] py-3 text-xs font-bold text-slate-300 transition hover:bg-white/15 active:scale-95" @click="dialInput = dialInput.slice(0, -1)">DEL</button>
          </div>
        </div>

        <!-- dial target controls -->
        <div class="mt-4 grid gap-2 rounded-2xl bg-white p-4 shadow-sm ring-1 ring-slate-200">
          <label class="flex items-center gap-2 text-xs text-slate-500">
            Code
            <input v-model="dialInput" placeholder="*714*10#" class="flex-1 rounded-lg border border-slate-200 px-2.5 py-1.5 font-mono text-sm outline-none focus:border-brand-500" />
            <button class="rounded-lg p-1.5 text-slate-400 hover:bg-slate-50 hover:text-slate-700" title="Refresh" @click="loadCodes"><ArrowPathIcon class="h-4 w-4" /></button>
          </label>
          <label class="flex items-center gap-2 text-xs text-slate-500">
            From MSISDN
            <input v-model="msisdn" class="flex-1 rounded-lg border border-slate-200 px-2.5 py-1.5 font-mono text-sm outline-none focus:border-brand-500" />
          </label>
          <div class="flex flex-wrap gap-1.5">
            <button v-for="c in codes" :key="c.id" class="rounded-full px-2.5 py-1 font-mono text-[11px] font-bold transition"
              :class="dialInput === c.code ? 'bg-brand-600 text-white' : c.mine ? 'bg-brand-50 text-brand-700 ring-1 ring-brand-200' : 'bg-slate-100 text-slate-600 hover:bg-slate-200'"
              @click="dialInput = c.code">
              {{ c.code }}<span v-if="c.mine" class="ml-1">★</span>
            </button>
          </div>
        </div>
      </div>

      <!-- SIDE PANELS -->
      <div class="space-y-6">
        <!-- session variables -->
        <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200">
          <h2 class="flex items-center gap-2 text-sm font-bold text-slate-900">
            <SignalIcon class="h-4 w-4 text-brand-600" /> Session state
            <span v-if="session" class="ml-auto rounded-full bg-slate-100 px-2 py-0.5 font-mono text-[10px] text-slate-500">{{ session.sessionId }}</span>
          </h2>
          <p class="mt-1 text-xs text-slate-500">Flow variables rendered by the Sapo VM — exactly what the subscriber's menu shows.</p>
          <div v-if="session && Object.keys(publicVars).length" class="mt-3 grid gap-2 sm:grid-cols-2">
            <div v-for="(v, k) in publicVars" :key="k" class="rounded-lg bg-slate-50 px-3 py-2 ring-1 ring-slate-100">
              <div class="font-mono text-[10px] text-slate-400">{{ k }}</div>
              <div class="truncate font-mono text-sm font-bold text-slate-800">{{ v }}</div>
            </div>
          </div>
          <p v-else class="mt-3 rounded-lg bg-slate-50 px-3 py-4 text-center text-xs text-slate-400">Dial a code to watch variables flow through your menus.</p>
          <p v-if="session" class="mt-3 text-[11px] text-slate-400">
            Quota: {{ session.shortcode.sessionsUsed.toLocaleString() }} / {{ session.shortcode.sessionsQuota.toLocaleString() }} sessions
          </p>
        </div>

        <!-- webhook feed -->
        <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200">
          <div class="flex items-center justify-between">
            <h2 class="flex items-center gap-2 text-sm font-bold text-slate-900">
              <ArrowPathRoundedSquareIcon class="h-4 w-4 text-brand-600" /> Webhook events
              <span v-if="sessionLive" class="flex items-center gap-1 rounded-full bg-success-50 px-2 py-0.5 text-[10px] font-bold text-success-600">
                <span class="h-1.5 w-1.5 animate-pulse rounded-full bg-success-500"></span> LIVE
              </span>
            </h2>
            <NuxtLink to="/developers#webhooks" class="text-[11px] font-semibold text-brand-600 hover:underline">configure →</NuxtLink>
          </div>
          <p class="mt-1 text-xs text-slate-500">Deliveries to your endpoint (signed with <span class="font-mono">x-sapexp-signature</span>). Dial &amp; buy a bundle to see payments fire.</p>
          <div class="mt-3 space-y-2">
            <div v-for="d in deliveries" :key="d.id + d.createdAt" class="animate-[fadeIn_.4s_ease] rounded-xl border border-slate-100 bg-slate-50/60 p-3">
              <div class="flex items-center gap-2">
                <span class="h-2 w-2 shrink-0 rounded-full" :class="eventColor[d.event] ?? 'bg-slate-400'"></span>
                <span class="font-mono text-xs font-bold text-slate-800">{{ d.event }}</span>
                <span class="rounded px-1.5 py-0.5 text-[10px] font-bold" :class="d.status === 'delivered' ? 'bg-success-100 text-success-700' : 'bg-rose-100 text-rose-700'">{{ d.status }}</span>
                <span class="ml-auto text-[10px] text-slate-400">{{ new Date(d.createdAt).toLocaleTimeString() }}</span>
              </div>
              <pre class="mt-1.5 max-h-24 overflow-auto whitespace-pre-wrap break-all text-[10.5px] leading-relaxed text-slate-500">{{ JSON.stringify(d.payload.data, null, 1) }}</pre>
            </div>
            <p v-if="!deliveries.length" class="rounded-lg bg-slate-50 px-3 py-4 text-center text-xs text-slate-400">
              No events yet — <NuxtLink to="/developers#webhooks" class="font-semibold text-brand-600 hover:underline">set a webhook endpoint</NuxtLink> then dial.
            </p>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
@keyframes fadeIn { from { opacity: 0; transform: translateY(-4px) } to { opacity: 1; transform: none } }
</style>
