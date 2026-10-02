<script setup lang="ts">
import {
  CheckCircleIcon, XCircleIcon, ArrowRightIcon, ArrowPathIcon, ArrowsRightLeftIcon,
} from '@heroicons/vue/24/outline'
import type { Plan, SessionPack } from '~/../shared/types'

// The short-code purchase wizard, shared by the onboarding checklist and the
// standalone "buy a short code" page. Three ways to get a code:
// system-assigned, developer-chosen, or ported in from another provider
// (Hubtel, Nalo, Africa's Talking, …). A session pack can optionally ride
// along on the same webcheckout.
const props = withDefaults(defineProps<{
  aggregator?: boolean
  source?: 'onboarding' | 'buy'
}>(), { aggregator: false, source: 'buy' })

const $api = useRequestFetch()

// --- catalogue ---------------------------------------------------------------
const plans = ref<Plan[]>([])
const packs = ref<SessionPack[]>([])
const setupFees = ref<Record<string, number>>({})
const portFlat = ref(105)
const suggested = ref('')

async function shuffleCode() {
  try { suggested.value = (await $api<{ code: string }>('/api/shortcodes/suggest')).code } catch { suggested.value = '' }
}

onMounted(async () => {
  plans.value = (await $api<{ plans: Plan[] }>('/api/billing')).plans ?? []
  const p = await $api<{ packs: SessionPack[]; setupFees: Record<string, number>; portFlatMonthly: number }>('/api/packs')
  packs.value = p.packs
  setupFees.value = p.setupFees
  portFlat.value = p.portFlatMonthly
  await shuffleCode()
})

// --- wizard state ------------------------------------------------------------
const mode = ref<'system' | 'user' | 'port'>('system')
const customCode = ref('')
const availability = ref<null | { available: boolean; message: string }>(null)
const checking = ref(false)
// porting a code in from another provider
const portCode = ref('')
const donorProvider = ref('')
const portAvailability = ref<null | { available: boolean; reason?: string; message: string }>(null)
const portChecking = ref(false)
const label = ref('')
const network = ref('all')
const planId = ref('starter')
const packId = ref('') // optional first-month session boost
const creating = ref(false)
const wizardError = ref('')

// fixed provider list — picked from a dropdown, no free text
const PROVIDERS = ['Hubtel', 'Nalo', "Africa's Talking", 'Korba', 'BPC']

let debounce: ReturnType<typeof setTimeout> | undefined
watch(customCode, (v) => {
  availability.value = null
  clearTimeout(debounce)
  if (!v.trim()) return
  checking.value = true
  debounce = setTimeout(async () => {
    try {
      availability.value = await $api<{ available: boolean; message: string }>(`/api/shortcodes/available`, { params: { code: v.trim() } })
    } finally { checking.value = false }
  }, 350)
})
// a ported code must simply not already live on ShortCodeExpress
let portDebounce: ReturnType<typeof setTimeout> | undefined
watch(portCode, (v) => {
  portAvailability.value = null
  clearTimeout(portDebounce)
  if (!v.trim()) return
  portChecking.value = true
  portDebounce = setTimeout(async () => {
    try {
      portAvailability.value = await $api<{ available: boolean; reason?: string; message: string }>(`/api/shortcodes/available`, { params: { code: v.trim() } })
      if (portAvailability.value?.available) portAvailability.value.message = `${v.trim()} is free to port into ShortCodeExpress`
    } finally { portChecking.value = false }
  }, 350)
})
onUnmounted(() => { clearTimeout(debounce); clearTimeout(portDebounce) })

const chosenCode = computed(() => (mode.value === 'system' ? suggested.value : mode.value === 'port' ? portCode.value.trim() : customCode.value.trim()))
const plan = computed(() => plans.value.find((p) => p.id === planId.value) ?? plans.value[0])
const porting = computed(() => mode.value === 'port')
const pack = computed(() => packs.value.find((p) => p.id === packId.value) ?? null)
const totalDue = computed(() => {
  if (porting.value) return portFlat.value
  const base = plan.value ? plan.value.priceMonthly + (setupFees.value[plan.value.id] ?? 250) : 0
  return base + (pack.value?.price ?? 0)
})

async function buyCode() {
  wizardError.value = ''
  creating.value = true
  try {
    const co = await $api<{ id: string }>('/api/checkout', {
      method: 'POST',
      body: porting.value
        ? { kind: 'port', code: portCode.value.trim(), provider: donorProvider.value.trim(), label: label.value.trim(), network: network.value, source: props.source }
        : {
            kind: 'shortcode', mode: mode.value, code: customCode.value.trim(), label: label.value.trim(), network: network.value, planId: planId.value,
            packId: packId.value || undefined, source: props.source,
          },
    })
    navigateTo(`/checkout/${co.id}`)
  } catch (e: unknown) {
    wizardError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not start checkout'
  } finally { creating.value = false }
}

const ghs = (n: number) => `GHS ${n.toLocaleString()}`
</script>

<template>
  <div class="space-y-5">
    <!-- mode -->
    <div class="grid gap-3 sm:grid-cols-3">
      <button class="rounded-xl border-2 p-4 text-left transition"
        :class="mode === 'system' ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 hover:border-slate-300'" @click="mode = 'system'">
        <div class="flex items-center justify-between">
          <span class="text-sm font-bold text-slate-900">Let the system choose</span>
          <span v-if="mode === 'system'" class="rounded-full bg-brand-600 px-2 py-0.5 text-[10px] font-bold uppercase text-white">Recommended</span>
        </div>
        <div class="mt-2 flex items-center gap-2">
          <span class="rounded-lg bg-white px-3 py-1.5 font-mono text-lg font-extrabold text-slate-900 ring-1 ring-slate-200">{{ suggested || '…' }}</span>
          <button class="rounded-lg p-1.5 text-slate-400 hover:bg-white hover:text-slate-700" title="Refresh suggestion" @click.stop="shuffleCode">
            <ArrowPathIcon class="h-4 w-4" />
          </button>
        </div>
        <p class="mt-2 text-xs text-slate-500">Next available code in the national pool — instantly live.</p>
      </button>

      <button class="rounded-xl border-2 p-4 text-left transition"
        :class="mode === 'user' ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 hover:border-slate-300'" @click="mode = 'user'">
        <span class="text-sm font-bold text-slate-900">I'll pick my own code</span>
        <input v-model="customCode" placeholder="*714*42#" @click.stop
          class="mt-2 w-full rounded-lg border px-3 py-2 font-mono text-lg font-extrabold outline-none"
          :class="availability?.available === false ? 'border-rose-300 text-rose-700' : availability?.available ? 'border-success-400 text-success-700' : 'border-slate-200 text-slate-900'" />
        <p class="mt-1.5 flex items-center gap-1 text-xs" :class="availability?.available === false ? 'text-rose-600' : availability?.available ? 'text-success-600' : 'text-slate-500'">
          <svg v-if="checking" class="h-3.5 w-3.5 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
          <CheckCircleIcon v-else-if="availability?.available" class="h-3.5 w-3.5" />
          <XCircleIcon v-else-if="availability?.available === false" class="h-3.5 w-3.5" />
          {{ availability?.message ?? 'Live availability check as you type.' }}
        </p>
      </button>

      <button class="rounded-xl border-2 p-4 text-left transition"
        :class="mode === 'port' ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 hover:border-slate-300'" @click="mode = 'port'">
        <div class="flex items-center gap-1.5">
          <ArrowsRightLeftIcon class="h-4 w-4 text-brand-600" />
          <span class="text-sm font-bold text-slate-900">{{ aggregator ? "Port a client's code" : 'Port a code I own' }}</span>
        </div>
        <p class="mt-2 text-xs leading-relaxed text-slate-500">
          Moving providers? Bring your existing code along — no downtime for your subscribers.
        </p>
        <p class="mt-1.5 text-[11px] font-semibold text-brand-600">Flat GHS {{ portFlat }}/mo · unlimited sessions</p>
      </button>
    </div>

    <!-- port details -->
    <div v-if="porting" class="space-y-3">
      <div class="grid gap-3 sm:grid-cols-2">
        <label class="block">
          <span class="mb-1 block text-xs font-semibold text-slate-600">The short code you own</span>
          <input v-model="portCode" placeholder="*714*42#"
            class="w-full rounded-xl border px-3.5 py-2.5 font-mono text-lg font-extrabold outline-none focus:ring-2 focus:ring-brand-100"
            :class="portAvailability?.available === false ? 'border-rose-300 text-rose-700' : portAvailability?.available ? 'border-success-400 text-success-700' : 'border-slate-200'" />
          <span class="mt-1 flex items-center gap-1 text-xs" :class="portAvailability?.available === false ? 'text-rose-600' : portAvailability?.available ? 'text-success-600' : 'text-slate-500'">
            <svg v-if="portChecking" class="h-3.5 w-3.5 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
            <CheckCircleIcon v-else-if="portAvailability?.available" class="h-3.5 w-3.5" />
            <XCircleIcon v-else-if="portAvailability?.available === false" class="h-3.5 w-3.5" />
            {{ portAvailability?.message ?? 'Type the code exactly as your subscribers dial it.' }}
          </span>
        </label>
        <label class="block">
          <span class="mb-1 block text-xs font-semibold text-slate-600">{{ aggregator ? "The code's current provider" : 'Your current provider' }}</span>
          <select v-model="donorProvider"
            class="w-full rounded-xl border border-slate-200 bg-white px-3.5 py-2.5 text-sm font-semibold text-slate-900 outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100">
            <option value="" disabled>Select a provider…</option>
            <option v-for="p in PROVIDERS" :key="p" :value="p">{{ p }}</option>
          </select>
          <span class="mt-1 block text-xs text-slate-500">Who routes this code today. We'll give you a link to send them.</span>
        </label>
      </div>
    </div>

    <!-- service details -->
    <div class="grid gap-3 sm:grid-cols-2">
      <label class="block">
        <span class="mb-1 block text-xs font-semibold text-slate-600">{{ aggregator ? 'Client service name' : 'Service name' }}</span>
        <input v-model="label" placeholder="Kofi Airtime" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
      </label>
      <label class="block">
        <span class="mb-1 block text-xs font-semibold text-slate-600">Network</span>
        <select v-model="network" class="w-full rounded-xl border border-slate-200 bg-white px-3.5 py-2.5 text-sm outline-none focus:border-brand-500">
          <option value="all">All Networks</option><option>MTN</option><option>Vodafone</option><option>AirtelTigo</option>
        </select>
      </label>
    </div>

    <!-- plans / flat porting rate -->
    <div v-if="porting" class="rounded-xl border-2 border-brand-200 bg-gradient-to-br from-brand-50/80 to-white p-5">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div>
          <div class="text-sm font-bold text-slate-900">Porting plan — flat rate</div>
          <ul class="mt-1.5 space-y-1 text-xs text-slate-600">
            <li>· <b class="text-slate-800">Unlimited sessions</b> — never buy packs, never hit a quota</li>
            <li>· No activation fee — just the monthly flat fee</li>
            <li>· Private porting link for your current provider, issued right after payment</li>
          </ul>
        </div>
        <div class="text-right">
          <div class="text-2xl font-extrabold text-brand-700">{{ ghs(portFlat) }}<span class="text-xs font-semibold text-slate-400">/month</span></div>
          <div class="text-[11px] font-semibold text-success-600">flat · no setup fee</div>
        </div>
      </div>
    </div>
    <div v-else>
      <span class="mb-2 block text-xs font-semibold text-slate-600">Plan (sets your session quota)</span>
      <div class="grid gap-3 sm:grid-cols-3">
        <button v-for="p in plans" :key="p.id" class="rounded-xl border-2 p-4 text-left transition"
          :class="planId === p.id ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 hover:border-slate-300'" @click="planId = p.id">
          <div class="text-sm font-bold text-slate-900">{{ p.name }}</div>
          <div class="mt-1 text-xs text-slate-500">{{ p.sessionQuota.toLocaleString() }} sessions/mo</div>
          <div class="mt-2 text-sm font-extrabold text-slate-900">{{ ghs(p.priceMonthly) }}<span class="text-xs font-normal text-slate-400">/mo</span></div>
          <div class="text-[11px] text-slate-400">+ {{ ghs(setupFees[p.id] ?? 250) }} one-time activation</div>
        </button>
      </div>

      <!-- OPTIONAL step: first-month session pack -->
      <div class="mt-4 rounded-xl border border-dashed border-slate-300 bg-slate-50/50 p-4">
        <div class="flex flex-wrap items-center justify-between gap-2">
          <span class="text-xs font-semibold text-slate-600">Optional — need more headroom in month one? Add a session pack</span>
          <button v-if="packId" class="text-[11px] font-semibold text-slate-400 hover:text-slate-600" @click="packId = ''">Remove pack</button>
        </div>
        <div class="mt-2.5 grid gap-2 sm:grid-cols-4">
          <button v-for="pk in packs" :key="pk.id" type="button"
            class="rounded-lg border-2 px-3 py-2.5 text-left transition"
            :class="packId === pk.id ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 bg-white hover:border-brand-300'"
            @click="packId = packId === pk.id ? '' : pk.id">
            <div class="text-sm font-extrabold text-slate-900">{{ pk.sessions.toLocaleString() }}</div>
            <div class="text-[11px] text-slate-500">sessions · {{ ghs(pk.price) }}</div>
          </button>
        </div>
        <p class="mt-2 text-[10.5px] text-slate-400">Skipped by default — your plan's sessions are included. You can top up anytime later.</p>
      </div>
    </div>

    <p v-if="wizardError" class="rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700">{{ wizardError }}</p>

    <div class="flex flex-wrap items-center justify-between gap-3 rounded-xl bg-slate-900 p-4">
      <div>
        <div class="text-[11px] font-bold uppercase tracking-wide text-slate-400">Total due at checkout</div>
        <div class="text-xl font-extrabold text-white">{{ ghs(totalDue) }}</div>
        <div class="text-[11px] text-slate-400">
          {{ chosenCode || 'a fresh code' }} ·
          {{ porting ? `first month · flat ${ghs(portFlat)} — no activation fee` : pack ? `first month + activation + ${pack.sessions.toLocaleString()}-session pack` : 'first month + activation' }}
        </div>
      </div>
      <button :disabled="creating || (mode === 'user' && availability?.available !== true) || (porting && (portAvailability?.available !== true || donorProvider.trim().length < 2)) || !label.trim()"
        class="flex items-center gap-2 rounded-xl bg-brand-500 px-5 py-3 text-sm font-bold text-white shadow-lg transition hover:bg-brand-400 disabled:opacity-40"
        @click="buyCode">
        <svg v-if="creating" class="h-4 w-4 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
        <ArrowRightIcon v-else class="h-4 w-4" />
        Continue to secure checkout
      </button>
    </div>
  </div>
</template>
