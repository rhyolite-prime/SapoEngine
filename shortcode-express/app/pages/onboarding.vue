<script setup lang="ts">
import {
  CheckCircleIcon, KeyIcon, SignalIcon, BoltSlashIcon, DevicePhoneMobileIcon,
  ArrowPathIcon, ArrowRightIcon, SparklesIcon, WrenchScrewdriverIcon, ClockIcon, XCircleIcon,
} from '@heroicons/vue/24/outline'
import type { Plan, SessionPack } from '~/../shared/types'

definePageMeta({ middleware: 'auth' })
useHead({ title: 'Go live in 5 minutes · ShortCodeExpress' })

interface OnboardingStatus {
  user: { id: string; name: string; company: string; createdAt: string }
  hasKey: boolean
  webhookConfigured: boolean
  shortcode: null | {
    id: string; code: string; label: string; network: string; status: string; plan: string; assignedBy: 'user' | 'system'
    sessionsUsed: number; sessionsQuota: number; flowId: string | null; flowName: string | null; hasRelease: boolean
  }
}

const status = ref<OnboardingStatus | null>(null)
const $api = useRequestFetch()
const plans = ref<Plan[]>([])
const packs = ref<SessionPack[]>([])
const setupFees = ref<Record<string, number>>({})
const suggested = ref('')

async function refresh() {
  status.value = await $api<OnboardingStatus>('/api/onboarding')
  plans.value = (await $api<{ plans: Plan[] }>('/api/billing')).plans ?? plans.value
  const p = await $api<{ packs: SessionPack[]; setupFees: Record<string, number> }>('/api/packs')
  packs.value = p.packs
  setupFees.value = p.setupFees
  if (!suggested.value) await shuffleCode()
}
await refresh()

async function shuffleCode() {
  try { suggested.value = (await $api<{ code: string }>('/api/shortcodes/suggest')).code } catch { suggested.value = '' }
}

// --- step 2 wizard state -----------------------------------------------------
const mode = ref<'system' | 'user'>('system')
const customCode = ref('')
const availability = ref<null | { available: boolean; message: string }>(null)
const checking = ref(false)
const label = ref('')
const network = ref('MTN')
const planId = ref('starter')
const creating = ref(false)
const wizardError = ref('')

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
onUnmounted(() => clearTimeout(debounce))

const chosenCode = computed(() => (mode.value === 'system' ? suggested.value : customCode.value.trim()))
const plan = computed(() => plans.value.find((p) => p.id === planId.value) ?? plans.value[0])
const totalDue = computed(() => (plan.value ? plan.value.priceMonthly + (setupFees.value[plan.value.id] ?? 250) : 0))

async function buyCode() {
  wizardError.value = ''
  creating.value = true
  try {
    const co = await $api<{ id: string }>('/api/checkout', {
      method: 'POST',
      body: { kind: 'shortcode', mode: mode.value, code: customCode.value.trim(), label: label.value.trim(), network: network.value, planId: planId.value },
    })
    navigateTo(`/checkout/${co.id}`)
  } catch (e: unknown) {
    wizardError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not start checkout'
  } finally { creating.value = false }
}

async function topup(packId: string) {
  if (!status.value?.shortcode) return
  const co = await $api<{ id: string }>('/api/checkout', {
    method: 'POST', body: { kind: 'topup', shortcodeId: status.value.shortcode.id, packId },
  })
  navigateTo(`/checkout/${co.id}`)
}

// elapsed since signup ("5-minute" clock)
const elapsed = computed(() => {
  const t = status.value ? Date.now() - Date.parse(status.value.user.createdAt || new Date().toISOString()) : 0
  const mins = Math.floor(t / 60000)
  if (!status.value?.user.createdAt) return null
  return mins < 1 ? 'just started' : mins === 1 ? '1 min in' : `${mins} mins in`
})

const stepDone = computed(() => ({
  account: !!status.value?.hasKey,
  code: !!status.value?.shortcode,
  topup: !!status.value?.shortcode, // optional step: done once you have a code
  dial: !!status.value?.shortcode?.hasRelease,
}))

const allDone = computed(() => stepDone.value.dial)
const ghs = (n: number) => `GHS ${n.toLocaleString()}`
</script>

<template>
  <div class="mx-auto max-w-4xl p-8">
    <div class="mb-8 flex flex-wrap items-end justify-between gap-4">
      <div>
        <h1 class="text-2xl font-bold text-slate-900">Go live in 5 minutes ⚡</h1>
        <p class="mt-1 text-sm text-slate-500">
          {{ status?.user.name ? `${status.user.name} — ` : '' }}signup → short code → dial. No tickets, no calls to the telco.
        </p>
      </div>
      <div v-if="elapsed" class="flex items-center gap-1.5 rounded-full bg-brand-50 px-3 py-1.5 text-xs font-bold text-brand-700 ring-1 ring-brand-100">
        <ClockIcon class="h-4 w-4" /> {{ elapsed }}
      </div>
    </div>

    <!-- completion banner -->
    <div v-if="allDone && status?.shortcode" class="mb-8 overflow-hidden rounded-2xl bg-gradient-to-r from-success-600 to-success-400 p-6 text-white shadow-lg">
      <div class="flex flex-wrap items-center justify-between gap-4">
        <div>
          <div class="flex items-center gap-2 text-[11px] font-bold uppercase tracking-widest text-white/70"><SparklesIcon class="h-4 w-4" /> You're live</div>
          <div class="mt-1 font-mono text-3xl font-extrabold">{{ status.shortcode.code }}</div>
          <div class="mt-1 text-sm text-white/85">{{ status.shortcode.flowName }} · {{ status.shortcode.sessionsQuota.toLocaleString() }} sessions ready</div>
        </div>
        <div class="flex gap-2">
          <NuxtLink :to="`/dial?code=${status.shortcode.code}`" class="flex items-center gap-2 rounded-xl bg-white px-4 py-2.5 text-sm font-bold text-success-700 shadow hover:bg-success-50">
            <DevicePhoneMobileIcon class="h-4 w-4" /> Dial it
          </NuxtLink>
          <NuxtLink v-if="status.shortcode.flowId" :to="`/builder/${status.shortcode.flowId}`" class="flex items-center gap-2 rounded-xl bg-white/15 px-4 py-2.5 text-sm font-bold text-white ring-1 ring-white/40 hover:bg-white/25">
            <WrenchScrewdriverIcon class="h-4 w-4" /> Edit the flow
          </NuxtLink>
        </div>
      </div>
    </div>

    <div class="space-y-4">
      <!-- STEP 1 — account + keys -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 transition" :class="stepDone.account ? 'ring-success-100' : 'ring-slate-200'">
        <div class="flex items-start gap-4">
          <div class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl font-bold"
            :class="stepDone.account ? 'bg-success-100 text-success-600' : 'bg-amber-100 text-amber-600'">
            <CheckCircleIcon v-if="stepDone.account" class="h-5 w-5" /><span v-else>1</span>
          </div>
          <div class="min-w-0 flex-1">
            <div class="flex flex-wrap items-center gap-2">
              <h2 class="font-bold text-slate-900">Account &amp; API keys</h2>
              <span v-if="stepDone.account" class="rounded-full bg-success-50 px-2 py-0.5 text-[11px] font-bold text-success-600">DONE</span>
            </div>
            <p class="mt-1 text-sm text-slate-500">Your workspace is ready and an <span class="font-mono text-xs">sk_live_…</span> key was generated at signup.</p>
            <div class="mt-3 flex flex-wrap gap-2">
              <NuxtLink to="/developers" class="inline-flex items-center gap-1.5 rounded-lg bg-slate-900 px-3.5 py-2 text-xs font-bold text-white hover:bg-slate-800">
                <KeyIcon class="h-3.5 w-3.5" /> View API keys
              </NuxtLink>
              <NuxtLink to="/developers#webhooks" class="inline-flex items-center gap-1.5 rounded-lg bg-white px-3.5 py-2 text-xs font-bold text-slate-700 ring-1 ring-slate-200 hover:bg-slate-50">
                Configure webhooks
              </NuxtLink>
            </div>
          </div>
        </div>
      </div>

      <!-- STEP 2 — buy a short code -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 transition" :class="stepDone.code ? 'ring-success-100' : 'ring-slate-200'">
        <div class="flex items-start gap-4">
          <div class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl font-bold"
            :class="stepDone.code ? 'bg-success-100 text-success-600' : 'bg-amber-100 text-amber-600'">
            <CheckCircleIcon v-if="stepDone.code" class="h-5 w-5" /><span v-else>2</span>
          </div>
          <div class="min-w-0 flex-1">
            <div class="flex flex-wrap items-center gap-2">
              <h2 class="font-bold text-slate-900">Get your short code</h2>
              <span v-if="stepDone.code" class="rounded-full bg-success-50 px-2 py-0.5 text-[11px] font-bold text-success-600">LIVE</span>
            </div>

            <div v-if="status?.shortcode" class="mt-3 rounded-xl bg-slate-50 p-4 ring-1 ring-slate-100">
              <div class="flex flex-wrap items-center gap-3">
                <span class="font-mono text-xl font-extrabold text-slate-900">{{ status.shortcode.code }}</span>
                <span class="rounded-full bg-success-100 px-2 py-0.5 text-[11px] font-bold uppercase text-success-700">{{ status.shortcode.status }}</span>
                <span class="text-xs text-slate-500">assigned by {{ status.shortcode.assignedBy }} · {{ status.shortcode.network }}</span>
              </div>
              <p class="mt-2 text-sm text-slate-600">
                {{ status.shortcode.sessionsQuota.toLocaleString() }} sessions included ·
                <button class="font-semibold text-brand-600 hover:underline" @click="topup('p20')">top up anytime</button>
              </p>
            </div>

            <!-- purchase wizard -->
            <div v-else class="mt-4 space-y-5">
              <!-- mode -->
              <div class="grid gap-3 sm:grid-cols-2">
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
              </div>

              <!-- service details -->
              <div class="grid gap-3 sm:grid-cols-2">
                <label class="block">
                  <span class="mb-1 block text-xs font-semibold text-slate-600">Service name</span>
                  <input v-model="label" placeholder="Kofi Airtime" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
                </label>
                <label class="block">
                  <span class="mb-1 block text-xs font-semibold text-slate-600">Network</span>
                  <select v-model="network" class="w-full rounded-xl border border-slate-200 bg-white px-3.5 py-2.5 text-sm outline-none focus:border-brand-500">
                    <option>MTN</option><option>Vodafone</option><option>AirtelTigo</option>
                  </select>
                </label>
              </div>

              <!-- plans -->
              <div>
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
              </div>

              <p v-if="wizardError" class="rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700">{{ wizardError }}</p>

              <div class="flex flex-wrap items-center justify-between gap-3 rounded-xl bg-slate-900 p-4">
                <div>
                  <div class="text-[11px] font-bold uppercase tracking-wide text-slate-400">Total due at checkout</div>
                  <div class="text-xl font-extrabold text-white">{{ ghs(totalDue) }}</div>
                  <div class="text-[11px] text-slate-400">{{ chosenCode || 'a fresh code' }} · first month + activation</div>
                </div>
                <button :disabled="creating || (mode === 'user' && availability?.available !== true) || !label.trim()"
                  class="flex items-center gap-2 rounded-xl bg-brand-500 px-5 py-3 text-sm font-bold text-white shadow-lg transition hover:bg-brand-400 disabled:opacity-40"
                  @click="buyCode">
                  <svg v-if="creating" class="h-4 w-4 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
                  <ArrowRightIcon v-else class="h-4 w-4" />
                  Continue to secure checkout
                </button>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- STEP 3 — sessions -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 transition" :class="status?.shortcode ? 'ring-success-100' : 'ring-slate-200'">
        <div class="flex items-start gap-4">
          <div class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl font-bold"
            :class="status?.shortcode ? 'bg-success-100 text-success-600' : 'bg-slate-100 text-slate-400'">
            <CheckCircleIcon v-if="status?.shortcode" class="h-5 w-5" /><span v-else>3</span>
          </div>
          <div class="min-w-0 flex-1">
            <div class="flex flex-wrap items-center gap-2">
              <h2 class="font-bold text-slate-900">Sessions &amp; top-ups</h2>
              <span class="rounded-full bg-slate-100 px-2 py-0.5 text-[11px] font-bold text-slate-500">OPTIONAL</span>
            </div>
            <p v-if="status?.shortcode" class="mt-1 text-sm text-slate-500">
              <span class="font-bold text-slate-800">{{ status.shortcode.sessionsUsed.toLocaleString() }} / {{ status.shortcode.sessionsQuota.toLocaleString() }}</span> sessions used.
              Every dial counts against the quota — top up in one click when you need more.
            </p>
            <p v-else class="mt-1 text-sm text-slate-500">Your plan's sessions arrive with the code. Buy extra packs anytime via webcheckout.</p>
            <div class="mt-3 grid gap-2 sm:grid-cols-4">
              <div v-for="pk in packs" :key="pk.id" class="rounded-xl border border-slate-200 p-3">
                <div class="text-sm font-bold text-slate-900">{{ pk.sessions.toLocaleString() }}</div>
                <div class="text-[11px] text-slate-500">sessions · {{ ghs(pk.price) }}</div>
                <button :disabled="!status?.shortcode" class="mt-2 w-full rounded-lg bg-slate-900 px-2 py-1.5 text-[11px] font-bold text-white hover:bg-slate-800 disabled:opacity-30" @click="topup(pk.id)">
                  <BoltSlashIcon class="mr-1 inline h-3.5 w-3.5" /> Top up
                </button>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- STEP 4 — dial live -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 transition" :class="stepDone.dial ? 'ring-success-100' : 'ring-slate-200'">
        <div class="flex items-start gap-4">
          <div class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl font-bold"
            :class="stepDone.dial ? 'bg-success-100 text-success-600' : 'bg-slate-100 text-slate-400'">
            <CheckCircleIcon v-if="stepDone.dial" class="h-5 w-5" /><span v-else>4</span>
          </div>
          <div class="min-w-0 flex-1">
            <h2 class="font-bold text-slate-900">Dial your code — live</h2>
            <p class="mt-1 text-sm text-slate-500">
              Your starter flow is deployed: dynamic menus (<span class="font-mono text-xs">${{ '{' }}balance{{ '}' }}</span> templating), a live API call and a MoMo charge that fires
              a <span class="font-mono text-xs">payment.succeeded</span> webhook. Navigate it on the keypad like a real subscriber.
            </p>
            <div class="mt-3 flex flex-wrap gap-2">
              <NuxtLink v-if="status?.shortcode" :to="`/dial?code=${status.shortcode.code}`"
                class="inline-flex items-center gap-1.5 rounded-lg bg-brand-600 px-3.5 py-2 text-xs font-bold text-white hover:bg-brand-500">
                <DevicePhoneMobileIcon class="h-3.5 w-3.5" /> Open the dialer
              </NuxtLink>
              <NuxtLink v-if="status?.shortcode?.flowId" :to="`/builder/${status.shortcode.flowId}`"
                class="inline-flex items-center gap-1.5 rounded-lg bg-white px-3.5 py-2 text-xs font-bold text-slate-700 ring-1 ring-slate-200 hover:bg-slate-50">
                <WrenchScrewdriverIcon class="h-3.5 w-3.5" /> Edit flow in builder
              </NuxtLink>
              <NuxtLink v-if="status?.shortcode" to="/developers#webhooks"
                class="inline-flex items-center gap-1.5 rounded-lg bg-white px-3.5 py-2 text-xs font-bold text-slate-700 ring-1 ring-slate-200 hover:bg-slate-50">
                <SignalIcon class="h-3.5 w-3.5" /> Watch webhooks fire
              </NuxtLink>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
