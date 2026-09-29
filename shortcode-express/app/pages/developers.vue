<script setup lang="ts">
import {
  KeyIcon, PlusIcon, TrashIcon, ClipboardDocumentIcon, ClipboardDocumentCheckIcon,
  BoltIcon, ArrowPathRoundedSquareIcon, SignalIcon, ArrowPathIcon, PaperAirplaneIcon, CheckCircleIcon,
} from '@heroicons/vue/24/outline'
import type { ApiKey, ShortCode, WebhookDelivery, WebhookEndpoint } from '~/../shared/types'

definePageMeta({ middleware: 'auth' })
useHead({ title: 'Developers · ShortCodeExpress' })

interface WebhookEventDef { id: string; label: string; desc: string }

const keys = ref<ApiKey[]>([])
const $api = useRequestFetch()
const codes = ref<ShortCode[]>([])
const endpoint = ref<WebhookEndpoint | null>(null)
const deliveries = ref<WebhookDelivery[]>([])
const events = ref<WebhookEventDef[]>([])
const busy = ref(false)
const newKeyName = ref('')
const copied = ref('')
const testing = ref(false)
const testResult = ref<'' | 'ok' | 'fail'>('')

// webhook form
const whUrl = ref('')
const whEvents = ref<string[]>([])
const whActive = ref(true)
const useTestReceiver = ref(false)
const whSaved = ref(false)

async function refresh() {
  keys.value = await $api<ApiKey[]>('/api/keys')
  codes.value = await $api<ShortCode[]>('/api/shortcodes')
  const wh = await $api<{ endpoint: WebhookEndpoint | null; deliveries: WebhookDelivery[] }>('/api/webhooks')
  endpoint.value = wh.endpoint
  deliveries.value = wh.deliveries ?? []
  if (!whUrl.value && wh.endpoint) { whUrl.value = wh.endpoint.url; whEvents.value = wh.endpoint.events; whActive.value = wh.endpoint.active }
  events.value = (await $api<{ events?: WebhookEventDef[] }>('/api/webhooks/events').catch(() => ({}))).events ?? []
}
await refresh()
if (!events.value.length) {
  // static fallback list (kept in sync with WEBHOOK_EVENTS on the server)
  events.value = [
    { id: 'payment.succeeded', label: 'payment.succeeded', desc: 'Webcheckout or in-flow charge succeeded' },
    { id: 'shortcode.assigned', label: 'shortcode.assigned', desc: 'New short code provisioned & live' },
    { id: 'sessions.topped_up', label: 'sessions.topped_up', desc: 'Session pack credited' },
    { id: 'session.started', label: 'session.started', desc: 'Subscriber dialed your code' },
    { id: 'session.completed', label: 'session.completed', desc: 'Session reached success terminate' },
    { id: 'session.failed', label: 'session.failed', desc: 'Session failed' },
    { id: 'flow.http_request', label: 'flow.http_request', desc: 'Flow called an HTTP integration live' },
    { id: 'test.ping', label: 'test.ping', desc: 'Test event' },
  ]
}

function copy(text: string, id: string) {
  navigator.clipboard?.writeText(text)
  copied.value = id
  setTimeout(() => { copied.value = '' }, 1600)
}

async function createKey() {
  busy.value = true
  try {
    await $api('/api/keys', { method: 'POST', body: { name: newKeyName.value } })
    newKeyName.value = ''
    await refresh()
  } finally { busy.value = false }
}
async function revokeKey(id: string) {
  await $api(`/api/keys/${id}`, { method: 'DELETE' })
  await refresh()
}

async function saveWebhook() {
  busy.value = true
  whSaved.value = false
  try {
    endpoint.value = await $api<WebhookEndpoint>('/api/webhooks', {
      method: 'PUT',
      body: { url: useTestReceiver.value ? `${location.origin}/api/webhooks/echo` : whUrl.value, events: whEvents.value, active: whActive.value },
    })
    whSaved.value = true
    await refresh()
  } finally { busy.value = false }
}

async function sendTest() {
  testing.value = true
  testResult.value = ''
  try {
    const d = await $api<WebhookDelivery>('/api/webhooks/test', { method: 'POST' })
    testResult.value = d.status === 'delivered' ? 'ok' : 'fail'
    await refresh()
  } catch { testResult.value = 'fail' } finally { testing.value = false }
}

async function resend(id: string) {
  await $api(`/api/webhooks/deliveries/${id}/resend`, { method: 'POST' }).catch(() => null)
  await refresh()
}

function toggleEvent(id: string) {
  whEvents.value = whEvents.value.includes(id) ? whEvents.value.filter((e) => e !== id) : [...whEvents.value, id]
}
watch(useTestReceiver, (v) => { if (v) whUrl.value = '' })

const origin = typeof window !== 'undefined' ? window.location.origin : 'https://your-app.example'
const myKey = computed(() => keys.value[0]?.key ?? 'sk_live_yourkey')
const myCode = computed(() => codes.value.find((c) => c.ownerId)?.code ?? '*714*10#')
const curlDial = `curl -X POST ${origin}/api/ussd/dial \\
  -H "Authorization: Bearer ${myKey}" \\
  -H "Content-Type: application/json" \\
  -d '{ "code": "${myCode}", "msisdn": "0244123456" }'`
const curlInput = `curl -X POST ${origin}/api/ussd/input \\
  -H "Authorization: Bearer ${myKey}" \\
  -H "Content-Type: application/json" \\
  -d '{ "sessionId": "ussd_xxx", "text": "1" }'`

const eventColor: Record<string, string> = {
  'payment.succeeded': 'bg-emerald-500', 'session.started': 'bg-brand-500', 'session.completed': 'bg-teal-500',
  'session.failed': 'bg-rose-500', 'flow.http_request': 'bg-sky-500', 'shortcode.assigned': 'bg-fuchsia-500',
  'sessions.topped_up': 'bg-amber-500', 'test.ping': 'bg-slate-400',
}
</script>

<template>
  <div class="p-8">
    <div class="mb-8">
      <h1 class="text-2xl font-bold text-slate-900">Developers</h1>
      <p class="mt-1 text-sm text-slate-500">API keys, the live USSD gateway, and signed webhooks — everything you need to integrate.</p>
    </div>

    <!-- API KEYS -->
    <section class="mb-8">
      <h2 class="mb-3 flex items-center gap-2 text-sm font-bold uppercase tracking-wide text-slate-400"><KeyIcon class="h-4 w-4" /> API keys</h2>
      <div class="overflow-hidden rounded-2xl bg-white shadow-sm ring-1 ring-slate-200">
        <div v-for="k in keys" :key="k.id" class="flex flex-wrap items-center gap-3 border-b border-slate-100 p-4 last:border-0">
          <div class="min-w-0 flex-1">
            <div class="flex items-center gap-2">
              <span class="text-sm font-bold text-slate-800">{{ k.name }}</span>
              <span v-if="k.lastUsedAt" class="rounded-full bg-slate-100 px-2 py-0.5 text-[10px] font-semibold text-slate-500">used {{ new Date(k.lastUsedAt).toLocaleDateString() }}</span>
            </div>
            <div class="mt-1 flex items-center gap-2">
              <code class="truncate rounded bg-slate-50 px-2 py-1 font-mono text-xs text-slate-700 ring-1 ring-slate-100">{{ k.key }}</code>
              <button class="shrink-0 rounded-lg p-1.5 text-slate-400 hover:bg-slate-50 hover:text-slate-700" @click="copy(k.key, k.id)">
                <ClipboardDocumentCheckIcon v-if="copied === k.id" class="h-4 w-4 text-emerald-500" />
                <ClipboardDocumentIcon v-else class="h-4 w-4" />
              </button>
            </div>
          </div>
          <button class="rounded-lg p-2 text-slate-400 hover:bg-rose-50 hover:text-rose-600" title="Revoke" @click="revokeKey(k.id)"><TrashIcon class="h-4 w-4" /></button>
        </div>
        <div class="flex items-center gap-2 bg-slate-50 p-3">
          <input v-model="newKeyName" placeholder="Key name (e.g. Production gateway)" class="flex-1 rounded-lg border border-slate-200 bg-white px-3 py-2 text-sm outline-none focus:border-brand-500" @keyup.enter="createKey" />
          <button :disabled="busy || !newKeyName.trim()" class="flex items-center gap-1.5 rounded-lg bg-brand-600 px-3.5 py-2 text-sm font-semibold text-white hover:bg-brand-500 disabled:opacity-40" @click="createKey">
            <PlusIcon class="h-4 w-4" /> Create key
          </button>
        </div>
      </div>
    </section>

    <!-- GATEWAY QUICKSTART -->
    <section class="mb-8">
      <h2 class="mb-3 flex items-center gap-2 text-sm font-bold uppercase tracking-wide text-slate-400"><BoltIcon class="h-4 w-4" /> USSD gateway quickstart</h2>
      <div class="grid gap-4 lg:grid-cols-2">
        <div class="rounded-2xl bg-slate-900 p-5 shadow-lg">
          <div class="mb-2 flex items-center gap-2 text-xs font-bold uppercase tracking-wide text-slate-400"><SignalIcon class="h-4 w-4 text-emerald-400" /> 1 — start a session</div>
          <pre class="overflow-x-auto text-[11.5px] leading-relaxed text-emerald-300">{{ curlDial }}</pre>
          <p class="mt-2 text-[11px] text-slate-500">Returns sessionId + the first rendered screen (dynamic <span class="font-mono">${{ '{' }}var{{ '}' }}</span> templates resolved by the Sapo VM).</p>
        </div>
        <div class="rounded-2xl bg-slate-900 p-5 shadow-lg">
          <div class="mb-2 flex items-center gap-2 text-xs font-bold uppercase tracking-wide text-slate-400"><ArrowPathRoundedSquareIcon class="h-4 w-4 text-emerald-400" /> 2 — send subscriber input</div>
          <pre class="overflow-x-auto text-[11.5px] leading-relaxed text-emerald-300">{{ curlInput }}</pre>
          <p class="mt-2 text-[11px] text-slate-500">Quotas are enforced per code; 402 means top-up time. Sandbox rails: <span class="font-mono">/api/mock/pay</span> always approves and fires <span class="font-mono">payment.succeeded</span>.</p>
        </div>
      </div>
    </section>

    <!-- WEBHOOKS -->
    <section id="webhooks" class="mb-8 scroll-mt-8">
      <h2 class="mb-3 flex items-center gap-2 text-sm font-bold uppercase tracking-wide text-slate-400"><ArrowPathRoundedSquareIcon class="h-4 w-4" /> Webhooks</h2>
      <div class="grid gap-4 lg:grid-cols-[420px_1fr]">
        <!-- config -->
        <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200">
          <label class="mb-3 flex items-center gap-2 text-sm">
            <input v-model="whActive" type="checkbox" class="h-4 w-4 rounded border-slate-300 text-brand-600" />
            Endpoint active
          </label>

          <label class="block">
            <span class="mb-1 block text-xs font-semibold text-slate-600">Endpoint URL</span>
            <input v-model="whUrl" :disabled="useTestReceiver" placeholder="https://api.yourapp.gh/hooks/sapexp"
              class="w-full rounded-xl border px-3 py-2.5 text-sm outline-none disabled:bg-slate-50 disabled:text-slate-400"
              :class="useTestReceiver ? 'border-slate-200' : 'border-slate-200 focus:border-brand-500 focus:ring-2 focus:ring-brand-100'" />
          </label>

          <label class="mt-3 flex items-start gap-2 rounded-xl bg-slate-50 p-3 text-xs text-slate-600 ring-1 ring-slate-100">
            <input v-model="useTestReceiver" type="checkbox" class="mt-0.5 h-4 w-4 rounded border-slate-300 text-brand-600" />
            <span>
              <span class="font-bold">Use the built-in test receiver</span> — posts to <span class="font-mono">/api/webhooks/echo</span> so you can watch deliveries without running a server. Uncheck for your real URL.
            </span>
          </label>

          <div class="mt-4">
            <span class="mb-2 block text-xs font-semibold text-slate-600">Events</span>
            <div class="space-y-1.5">
              <label v-for="ev in events" :key="ev.id" class="flex items-start gap-2.5 rounded-lg p-2 transition hover:bg-slate-50" :class="whEvents.includes(ev.id) ? 'bg-brand-50/50' : ''">
                <input type="checkbox" :checked="whEvents.includes(ev.id)" class="mt-0.5 h-4 w-4 rounded border-slate-300 text-brand-600" @change="toggleEvent(ev.id)" />
                <div>
                  <div class="font-mono text-xs font-bold text-slate-800">{{ ev.label }}</div>
                  <div class="text-[11px] text-slate-500">{{ ev.desc }}</div>
                </div>
              </label>
            </div>
          </div>

          <div class="mt-4 flex items-center gap-2">
            <button :disabled="busy" class="flex items-center gap-1.5 rounded-xl bg-slate-900 px-4 py-2.5 text-sm font-bold text-white hover:bg-slate-800 disabled:opacity-40" @click="saveWebhook">
              <CheckCircleIcon class="h-4 w-4" /> Save endpoint
            </button>
            <button :disabled="testing || !endpoint?.url" class="flex items-center gap-1.5 rounded-xl bg-brand-600 px-4 py-2.5 text-sm font-bold text-white hover:bg-brand-500 disabled:opacity-40" @click="sendTest">
              <PaperAirplaneIcon class="h-4 w-4" /> Send test event
            </button>
            <span v-if="whSaved" class="text-xs font-bold text-emerald-600">saved ✓</span>
            <span v-if="testResult === 'ok'" class="text-xs font-bold text-emerald-600">delivered ✓</span>
            <span v-if="testResult === 'fail'" class="text-xs font-bold text-rose-600">failed ✗</span>
          </div>

          <div v-if="endpoint?.secret" class="mt-4 rounded-xl bg-slate-50 p-3 ring-1 ring-slate-100">
            <div class="text-[11px] font-bold uppercase tracking-wide text-slate-400">Signing secret</div>
            <div class="mt-1 flex items-center gap-2">
              <code class="truncate font-mono text-xs text-slate-700">{{ endpoint.secret }}</code>
              <button class="shrink-0 rounded p-1 text-slate-400 hover:text-slate-700" @click="copy(endpoint.secret, 'secret')">
                <ClipboardDocumentCheckIcon v-if="copied === 'secret'" class="h-4 w-4 text-emerald-500" /><ClipboardDocumentIcon v-else class="h-4 w-4" />
              </button>
            </div>
            <p class="mt-1.5 text-[10.5px] text-slate-500">Verify with HMAC-SHA256 over <span class="font-mono">"{timestamp}.{body}"</span> from the <span class="font-mono">x-sapexp-signature</span> header (<span class="font-mono">t=…,v1=…</span>).</p>
          </div>
        </div>

        <!-- delivery log -->
        <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200">
          <div class="flex items-center justify-between">
            <h3 class="text-sm font-bold text-slate-900">Recent deliveries</h3>
            <button class="flex items-center gap-1 text-xs font-semibold text-slate-500 hover:text-slate-800" @click="refresh"><ArrowPathIcon class="h-3.5 w-3.5" /> refresh</button>
          </div>
          <div class="mt-3 max-h-[520px] space-y-2 overflow-y-auto pr-1">
            <details v-for="d in deliveries" :key="d.id" class="group rounded-xl border border-slate-100 bg-slate-50/60 p-3">
              <summary class="flex cursor-pointer list-none items-center gap-2">
                <span class="h-2 w-2 shrink-0 rounded-full" :class="eventColor[d.event] ?? 'bg-slate-400'"></span>
                <span class="font-mono text-xs font-bold text-slate-800">{{ d.event }}</span>
                <span class="rounded px-1.5 py-0.5 text-[10px] font-bold" :class="d.status === 'delivered' ? 'bg-emerald-100 text-emerald-700' : 'bg-rose-100 text-rose-700'">{{ d.status }}{{ d.responseStatus ? ' ' + d.responseStatus : '' }}</span>
                <span class="ml-auto text-[10px] text-slate-400">{{ new Date(d.createdAt).toLocaleTimeString() }}</span>
              </summary>
              <div class="mt-2 space-y-1.5">
                <div class="text-[10.5px] text-slate-500">→ {{ d.url }}</div>
                <pre class="max-h-56 overflow-auto whitespace-pre-wrap break-all rounded-lg bg-slate-900 p-2.5 text-[10.5px] leading-relaxed text-emerald-300">{{ JSON.stringify(d.payload, null, 2) }}</pre>
                <div class="flex items-center gap-2">
                  <code class="truncate rounded bg-white px-2 py-1 font-mono text-[10px] text-slate-500 ring-1 ring-slate-100">x-sapexp-signature: {{ d.signature.slice(0, 34) }}…</code>
                  <button class="ml-auto shrink-0 rounded-lg bg-white px-2.5 py-1 text-[10px] font-bold text-slate-600 ring-1 ring-slate-200 hover:bg-slate-100" @click="resend(d.id)">resend</button>
                </div>
                <p v-if="d.error" class="text-[10.5px] text-rose-500">error: {{ d.error }}</p>
              </div>
            </details>
            <p v-if="!deliveries.length" class="rounded-lg bg-slate-50 px-3 py-6 text-center text-xs text-slate-400">
              No deliveries yet. Save an endpoint (try the test receiver) and press <span class="font-semibold">Send test event</span>.
            </p>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>
