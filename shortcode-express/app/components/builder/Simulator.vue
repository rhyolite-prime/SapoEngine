<script setup lang="ts">
import { XMarkIcon, ArrowPathIcon, PhoneIcon, PlayIcon, PaperAirplaneIcon, WifiIcon } from '@heroicons/vue/24/outline'
import { SapoSimulator, type VmTraceEntry } from '~/../shared/utils/vm'
import type { SapoBlueprint } from '~/../shared/types'

const emit = defineEmits<{ (e: 'close'): void }>()
const ctx = useBuilder()

const liveApi = ref(false)
const sim = shallowRef<SapoSimulator | null>(null)
const input = ref('')
const screenHistory = ref<string[]>([])
const trace = ref<VmTraceEntry[]>([])
const status = ref<'idle' | 'running' | 'awaiting_input' | 'awaiting_event' | 'completed' | 'failed'>('idle')
const varsView = ref<Record<string, unknown>>({})
const simError = ref('')

async function dial() {
  simError.value = ''
  const bp: SapoBlueprint = ctx.toBlueprint()
  const s = new SapoSimulator(bp, {
    liveHttp: liveApi.value,
    msisdn: '0242602262',
    network: 'MTN',
    httpCall: async (req) => {
      const res = await $fetch<{ status: number; body: unknown }>('/api/simulate/http', {
        method: 'POST',
        body: { method: req.method, url: req.url, headers: req.headers, query: req.query, body: req.body, timeoutMs: 6000 },
      })
      return res
    },
  })
  sim.value = s
  try {
    await s.dial()
  } catch (e) {
    simError.value = (e as Error).message
  }
  sync()
}

async function send() {
  if (!sim.value || !input.value.trim()) return
  const value = input.value.trim()
  input.value = ''
  try {
    await sim.value.submit(value)
  } catch (e) {
    simError.value = (e as Error).message
  }
  sync()
}

function sync() {
  if (!sim.value) return
  status.value = sim.value.status
  screenHistory.value = [...sim.value.screen]
  trace.value = [...sim.value.trace].reverse()
  varsView.value = { ...sim.value.vars }
}

function reset() {
  sim.value = null
  screenHistory.value = []
  trace.value = []
  status.value = 'idle'
  varsView.value = {}
  simError.value = ''
  input.value = ''
}

const statusMeta = computed(() => {
  switch (status.value) {
    case 'idle': return { label: 'Not connected', cls: 'bg-slate-100 text-slate-500' }
    case 'running': return { label: 'Running…', cls: 'bg-sky-100 text-sky-700' }
    case 'awaiting_input': return { label: 'Waiting for reply', cls: 'bg-amber-100 text-amber-700' }
    case 'awaiting_event': return { label: 'Waiting for event', cls: 'bg-violet-100 text-violet-700' }
    case 'completed': return { label: 'Session completed', cls: 'bg-emerald-100 text-emerald-700' }
    case 'failed': return { label: 'Session failed', cls: 'bg-rose-100 text-rose-700' }
    default: return { label: status.value, cls: 'bg-slate-100 text-slate-500' }
  }
})

const traceColor: Record<string, string> = {
  ok: 'text-slate-500', prompt: 'text-violet-600 font-semibold', api: 'text-sky-600',
  jump: 'text-amber-600', error: 'text-rose-600', info: 'text-slate-400', end: 'text-emerald-600 font-semibold',
}
</script>

<template>
  <div class="fixed inset-y-0 right-0 z-40 flex w-[400px] flex-col border-l border-slate-200 bg-white shadow-2xl">
    <header class="flex h-12 shrink-0 items-center gap-2 border-b border-slate-200 px-4">
      <PhoneIcon class="h-4 w-4 text-brand-600" />
      <span class="text-sm font-bold text-slate-800">USSD Simulator</span>
      <span class="ml-2 rounded-full px-2 py-0.5 text-[10px] font-bold" :class="statusMeta.cls">{{ statusMeta.label }}</span>
      <button class="ml-auto rounded-lg p-1.5 text-slate-400 hover:bg-slate-100" title="Close" @click="emit('close')"><XMarkIcon class="h-5 w-5" /></button>
    </header>

    <div class="flex shrink-0 items-center gap-2 border-b border-slate-100 bg-slate-50 px-4 py-2.5">
      <button
        class="inline-flex items-center gap-1.5 rounded-lg bg-brand-600 px-3 py-1.5 text-xs font-bold text-white hover:bg-brand-500"
        @click="dial()"
      >
        <PlayIcon class="h-3.5 w-3.5" /> {{ status === 'idle' ? 'Dial short code' : 'Restart' }}
      </button>
      <label class="flex cursor-pointer items-center gap-1.5 text-[11px] font-semibold text-slate-600">
        <input v-model="liveApi" type="checkbox" class="accent-brand-600" @change="reset()" />
        <WifiIcon class="h-3.5 w-3.5" /> Live API calls
      </label>
      <button class="ml-auto inline-flex items-center gap-1 rounded-lg border border-slate-200 px-2.5 py-1.5 text-[11px] font-semibold text-slate-600 hover:border-slate-300" @click="reset()">
        <ArrowPathIcon class="h-3.5 w-3.5" /> Reset
      </button>
    </div>

    <!-- phone -->
    <div class="shrink-0 px-5 py-4">
      <div class="mx-auto w-[280px] rounded-[2rem] border-[6px] border-ink-900 bg-ink-950 p-3 shadow-xl">
        <div class="mb-2 flex items-center justify-between px-1 text-[9px] font-semibold text-slate-500">
          <span>{{ liveApi ? 'LIVE' : 'MOCK' }} · MTN</span>
          <span class="font-mono">*920*108#</span>
        </div>
        <div class="h-56 overflow-y-auto rounded-xl bg-[#c8e6c9] p-3 font-mono text-[12px] leading-relaxed text-[#12341a]">
          <div v-if="!screenHistory.length" class="flex h-full items-center justify-center text-center text-[#2e5c3a]/60">
            Press “Dial short code” to start a session
          </div>
          <div v-for="(line, i) in screenHistory" :key="i" class="mb-2 whitespace-pre-wrap">{{ line }}</div>
        </div>
        <div class="mt-2 flex gap-1.5">
          <input
            v-model="input" :disabled="status !== 'awaiting_input' && status !== 'awaiting_event'"
            :placeholder="status === 'awaiting_event' ? 'event payload…' : status === 'awaiting_input' ? 'type reply…' : '—'"
            class="min-w-0 flex-1 rounded-lg bg-ink-800 px-3 py-2 font-mono text-xs text-slate-100 placeholder:text-slate-600 focus:outline-none disabled:opacity-40"
            @keyup.enter="send()"
          />
          <button
            class="rounded-lg bg-emerald-500 px-3 text-white hover:bg-emerald-400 disabled:opacity-30"
            :disabled="status !== 'awaiting_input' && status !== 'awaiting_event'" @click="send()"
          >
            <PaperAirplaneIcon class="h-4 w-4" />
          </button>
        </div>
      </div>
      <p v-if="simError" class="mt-2 rounded-lg bg-rose-50 px-3 py-2 text-[11px] text-rose-700">{{ simError }}</p>
    </div>

    <!-- tabs: trace / context -->
    <div class="min-h-0 flex-1 border-t border-slate-200">
      <div class="grid h-full grid-rows-[auto_1fr]">
        <div class="flex border-b border-slate-100 text-[11px] font-bold uppercase tracking-wide">
          <span class="flex-1 px-4 py-2 text-slate-600">Execution trace</span>
          <span class="border-l border-slate-100 px-4 py-2 text-slate-400">Session context</span>
        </div>
        <div class="relative grid grid-cols-2 overflow-hidden">
          <div class="overflow-y-auto p-3">
            <div v-for="(t, i) in trace" :key="i" class="mb-1.5 rounded-lg bg-slate-50 px-2.5 py-1.5">
              <div class="flex items-center gap-1.5">
                <span class="font-mono text-[10px] font-bold" :class="traceColor[t.kind]">{{ t.nodeId || '—' }}</span>
                <span class="ml-auto rounded bg-white px-1.5 py-0.5 font-mono text-[9px] text-slate-400 ring-1 ring-slate-100">{{ t.type }}</span>
              </div>
              <div class="mt-0.5 text-[10.5px] leading-snug text-slate-500">{{ t.detail }}</div>
            </div>
            <div v-if="!trace.length" class="p-3 text-center text-[11px] text-slate-400">Trace appears as the session runs.</div>
          </div>
          <div class="overflow-y-auto border-l border-slate-100 bg-slate-50/50 p-3">
            <pre class="sce-json whitespace-pre-wrap break-all text-slate-600">{{ JSON.stringify(varsView, null, 2) }}</pre>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
