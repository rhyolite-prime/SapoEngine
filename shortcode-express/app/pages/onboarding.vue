<script setup lang="ts">
import {
  CheckCircleIcon, KeyIcon, SignalIcon, BoltSlashIcon, DevicePhoneMobileIcon,
  SparklesIcon, WrenchScrewdriverIcon, ClockIcon,
  LinkIcon, ClipboardDocumentIcon, ExclamationTriangleIcon,
  BuildingStorefrontIcon, UsersIcon,
} from '@heroicons/vue/24/outline'
import type { SessionPack } from '~/../shared/types'

useHead({ title: 'Go live in 5 minutes · ShortCodeExpress' })

interface OnboardingStatus {
  user: { id: string; name: string; company: string; createdAt: string }
  useCase: 'merchant' | 'aggregator' | null
  hasKey: boolean
  webhookConfigured: boolean
  shortcode: null | {
    id: string; code: string; label: string; network: string; status: string; plan: string; assignedBy: 'user' | 'system' | 'port'
    sessionsUsed: number; sessionsQuota: number; flowId: string | null; flowName: string | null; hasRelease: boolean
    flatMonthly: number | null
    port: null | {
      provider: string; interactionUrl: string; requestedAt: string
      approvedAt: string | null; rejectedAt: string | null; rejectedReason: string | null
    }
  }
}

const status = ref<OnboardingStatus | null>(null)
const $api = useRequestFetch()
const packs = ref<SessionPack[]>([])
const portFlat = ref(105)

async function refresh() {
  status.value = await $api<OnboardingStatus>('/api/onboarding')
  const p = await $api<{ packs: SessionPack[]; portFlatMonthly: number }>('/api/packs')
  packs.value = p.packs
  portFlat.value = p.portFlatMonthly
}
await refresh()

// --- step 1: how the platform will be used -------------------------------
const savingUseCase = ref(false)
const useCaseError = ref('')
const showUseCasePicker = ref(false)
const isAggregator = computed(() => status.value?.useCase === 'aggregator')

async function chooseUseCase(kind: 'merchant' | 'aggregator') {
  useCaseError.value = ''
  savingUseCase.value = true
  try {
    const r = await $api<{ useCase: 'merchant' | 'aggregator' }>('/api/onboarding/use-case', { method: 'POST', body: { useCase: kind } })
    if (status.value) status.value.useCase = r.useCase
    showUseCasePicker.value = false
  } catch (e: unknown) {
    useCaseError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not save your choice — try again'
  } finally { savingUseCase.value = false }
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

async function copyPortUrl() {
  const url = status.value?.shortcode?.port?.interactionUrl
  if (!url) return
  try { await navigator.clipboard.writeText(url); portUrlCopied.value = true; setTimeout(() => (portUrlCopied.value = false), 1600) } catch {}
}
const portInfo = computed(() => status.value?.shortcode?.port ?? null)
const portPending = computed(() => !!portInfo.value && !portInfo.value.approvedAt && !portInfo.value.rejectedAt)
const portApproved = computed(() => !!portInfo.value?.approvedAt)
const portRejected = computed(() => !!portInfo.value?.rejectedAt)

const stepDone = computed(() => ({
  useCase: !!status.value?.useCase,
  account: !!status.value?.hasKey,
  code: !!status.value?.shortcode,
  topup: !!status.value?.shortcode, // optional step: done once you have a code
  dial: !!status.value?.shortcode?.hasRelease && status.value.shortcode.status === 'active',
}))

const allDone = computed(() => stepDone.value.dial && stepDone.value.useCase)
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
      <!-- STEP 1 — merchant or aggregator -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 transition" :class="stepDone.useCase ? 'ring-success-100' : 'ring-slate-200'">
        <div class="flex items-start gap-4">
          <div class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl font-bold"
            :class="stepDone.useCase ? 'bg-success-100 text-success-600' : 'bg-amber-100 text-amber-600'">
            <CheckCircleIcon v-if="stepDone.useCase" class="h-5 w-5" /><span v-else>1</span>
          </div>
          <div class="min-w-0 flex-1">
            <div class="flex flex-wrap items-center gap-2">
              <h2 class="font-bold text-slate-900">How will you use ShortCodeExpress?</h2>
              <span v-if="stepDone.useCase" class="rounded-full bg-brand-50 px-2 py-0.5 text-[11px] font-bold uppercase tracking-wide text-brand-700">
                {{ isAggregator ? 'Aggregator' : 'Business merchant' }}
              </span>
              <button v-if="stepDone.useCase && !showUseCasePicker" class="text-xs font-semibold text-brand-600 hover:underline" @click="showUseCasePicker = true">
                Change
              </button>
            </div>

            <!-- picker -->
            <div v-if="!stepDone.useCase || showUseCasePicker" class="mt-4 grid gap-3 sm:grid-cols-2">
              <button :disabled="savingUseCase"
                class="group rounded-xl border-2 p-5 text-left transition disabled:opacity-50"
                :class="status?.useCase === 'merchant' ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 hover:border-brand-300 hover:bg-brand-50/30'"
                @click="chooseUseCase('merchant')">
                <div class="flex items-center gap-2.5">
                  <span class="flex h-9 w-9 items-center justify-center rounded-xl bg-brand-100 text-brand-700"><BuildingStorefrontIcon class="h-5 w-5" /></span>
                  <span class="text-sm font-bold text-slate-900">Business merchant</span>
                </div>
                <p class="mt-2.5 text-xs leading-relaxed text-slate-500">
                  I run a business and want USSD for <b class="text-slate-700">my own services</b> — menus, payments and alerts under my own short code.
                </p>
                <p class="mt-2 text-[10.5px] font-semibold uppercase tracking-wide text-slate-400">Shops · fintechs · schools · churches · SACCOs</p>
              </button>

              <button :disabled="savingUseCase"
                class="group rounded-xl border-2 p-5 text-left transition disabled:opacity-50"
                :class="status?.useCase === 'aggregator' ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 hover:border-brand-300 hover:bg-brand-50/30'"
                @click="chooseUseCase('aggregator')">
                <div class="flex items-center gap-2.5">
                  <span class="flex h-9 w-9 items-center justify-center rounded-xl bg-brand-100 text-brand-700"><UsersIcon class="h-5 w-5" /></span>
                  <span class="text-sm font-bold text-slate-900">Aggregator</span>
                </div>
                <p class="mt-2.5 text-xs leading-relaxed text-slate-500">
                  I build and manage USSD services <b class="text-slate-700">for client businesses</b> — many short codes, flows and quotas from one workspace.
                </p>
                <p class="mt-2 text-[10.5px] font-semibold uppercase tracking-wide text-slate-400">Agencies · resellers · platforms serving many merchants</p>
              </button>
            </div>

            <!-- summary once chosen -->
            <p v-else-if="isAggregator" class="mt-1 text-sm text-slate-500">
              Aggregator mode: every short code you add belongs to a client — each with its own flow, quota and webhooks. Invite teammates on the Team page to manage them together.
            </p>
            <p v-else class="mt-1 text-sm text-slate-500">
              Merchant mode: your codes run your own services end to end — build a flow, release it, and take payments.
            </p>

            <p v-if="useCaseError" class="mt-2 rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700">{{ useCaseError }}</p>
          </div>
        </div>
      </div>

      <!-- STEP 2 — account + keys -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 transition" :class="stepDone.account ? 'ring-success-100' : 'ring-slate-200'">
        <div class="flex items-start gap-4">
          <div class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl font-bold"
            :class="stepDone.account ? 'bg-success-100 text-success-600' : 'bg-amber-100 text-amber-600'">
            <CheckCircleIcon v-if="stepDone.account" class="h-5 w-5" /><span v-else>2</span>
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
            <CheckCircleIcon v-if="stepDone.code" class="h-5 w-5" /><span v-else>3</span>
          </div>
          <div class="min-w-0 flex-1">
            <div class="flex flex-wrap items-center gap-2">
              <h2 class="font-bold text-slate-900">{{ isAggregator ? "Add your first client's short code" : 'Get your short code' }}</h2>
              <span v-if="stepDone.code" class="rounded-full bg-success-50 px-2 py-0.5 text-[11px] font-bold text-success-600">LIVE</span>
            </div>
            <p v-if="isAggregator" class="mt-1 text-xs text-slate-400">Each purchase below provisions one client code — repeat for every client you onboard.</p>

            <div v-if="status?.shortcode" class="mt-3 space-y-3">
              <div class="rounded-xl bg-slate-50 p-4 ring-1 ring-slate-100">
                <div class="flex flex-wrap items-center gap-3">
                  <span class="font-mono text-xl font-extrabold text-slate-900">{{ status.shortcode.code }}</span>
                  <span class="rounded-full px-2.5 py-0.5 text-[11px] font-bold uppercase"
                    :class="status.shortcode.status === 'active' ? 'bg-success-100 text-success-700' : status.shortcode.status === 'porting' ? 'bg-amber-100 text-amber-700' : 'bg-slate-200 text-slate-600'">
                    {{ status.shortcode.status }}
                  </span>
                  <span class="text-xs text-slate-500">
                    {{ status.shortcode.assignedBy === 'port' ? `ported from ${status.shortcode.port?.provider ?? 'your provider'}` : `assigned by ${status.shortcode.assignedBy}` }} · {{ status.shortcode.network }}
                  </span>
                </div>
                <p v-if="status.shortcode.flatMonthly" class="mt-2 text-sm text-slate-600">
                  <b>Unlimited sessions</b> — flat {{ ghs(status.shortcode.flatMonthly) }}/month, no packs needed.
                </p>
                <p v-else class="mt-2 text-sm text-slate-600">
                  {{ status.shortcode.sessionsQuota.toLocaleString() }} sessions included ·
                  <button class="font-semibold text-brand-600 hover:underline" @click="topup('p20')">top up anytime</button>
                </p>
              </div>

              <!-- porting tracker: waiting for the donor provider -->
              <div v-if="portPending" class="rounded-xl border border-amber-200 bg-amber-50/70 p-4">
                <div class="flex items-center gap-2 text-sm font-bold text-amber-800">
                  <span class="relative flex h-2.5 w-2.5"><span class="absolute inline-flex h-full w-full animate-ping rounded-full bg-amber-400 opacity-75"></span><span class="relative inline-flex h-2.5 w-2.5 rounded-full bg-amber-500"></span></span>
                  Waiting for {{ portInfo?.provider }} to release {{ status.shortcode.code }}
                </div>
                <p class="mt-1.5 text-xs leading-relaxed text-amber-700">
                  Send this private link to your current provider — the code goes live on ShortCodeExpress the moment they approve.
                  Your flow is already built and released, so nothing else is needed from you.
                </p>
                <div class="mt-2.5 flex items-center gap-2">
                  <div class="flex min-w-0 flex-1 items-center gap-2 rounded-lg border border-amber-200 bg-white px-3 py-2">
                    <LinkIcon class="h-4 w-4 shrink-0 text-amber-500" />
                    <span class="truncate font-mono text-xs font-semibold text-slate-700">{{ portInfo?.interactionUrl }}</span>
                  </div>
                  <button class="shrink-0 rounded-lg bg-amber-600 px-3 py-2 text-xs font-bold text-white hover:bg-amber-500" @click="copyPortUrl">
                    <ClipboardDocumentIcon class="mr-1 inline h-3.5 w-3.5" />{{ portUrlCopied ? 'Copied!' : 'Copy link' }}
                  </button>
                  <a :href="portInfo?.interactionUrl" target="_blank" rel="noopener"
                    class="shrink-0 rounded-lg bg-white px-3 py-2 text-xs font-bold text-amber-700 ring-1 ring-amber-300 hover:bg-amber-50" title="See what your provider sees">
                    Preview
                  </a>
                </div>
                <p class="mt-2 text-[10.5px] text-amber-600/90">Tip: the “Preview” link opens the exact page your provider will use to approve the port.</p>
              </div>

              <!-- port rejected -->
              <div v-else-if="portRejected" class="flex items-start gap-2.5 rounded-xl border border-rose-200 bg-rose-50 p-4">
                <ExclamationTriangleIcon class="mt-0.5 h-5 w-5 shrink-0 text-rose-500" />
                <div>
                  <div class="text-sm font-bold text-rose-700">{{ portInfo?.provider }} declined the port</div>
                  <p class="mt-0.5 text-xs text-rose-600">Reason: {{ portInfo?.rejectedReason ?? 'not given' }}. Your {{ ghs(status.shortcode.flatMonthly ?? portFlat) }} payment is safe — contact support to retry the port or get a refund.</p>
                </div>
              </div>

              <!-- port approved -->
              <div v-else-if="portApproved" class="flex items-center gap-2.5 rounded-xl border border-success-200 bg-success-50 p-4">
                <CheckCircleIcon class="h-5 w-5 shrink-0 text-success-600" />
                <p class="text-sm text-success-800"><b>{{ status.shortcode.code }} is yours.</b> Ported from {{ portInfo?.provider }} and live — unlimited sessions at a flat {{ ghs(status.shortcode.flatMonthly ?? portFlat) }}/month.</p>
              </div>
            </div>

            <!-- purchase wizard (shared with the standalone buy flow) -->
            <div v-else class="mt-4">
              <ShortcodesPurchaseWizard :aggregator="isAggregator" source="onboarding" />
            </div>
          </div>
        </div>
      </div>

      <!-- STEP 3 — sessions -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 transition" :class="status?.shortcode ? 'ring-success-100' : 'ring-slate-200'">
        <div class="flex items-start gap-4">
          <div class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl font-bold"
            :class="status?.shortcode ? 'bg-success-100 text-success-600' : 'bg-slate-100 text-slate-400'">
            <CheckCircleIcon v-if="status?.shortcode" class="h-5 w-5" /><span v-else>4</span>
          </div>
          <div class="min-w-0 flex-1">
            <div class="flex flex-wrap items-center gap-2">
              <h2 class="font-bold text-slate-900">Sessions &amp; top-ups</h2>
              <span class="rounded-full bg-slate-100 px-2 py-0.5 text-[11px] font-bold text-slate-500">OPTIONAL</span>
            </div>
            <p v-if="status?.shortcode?.flatMonthly" class="mt-1 text-sm text-slate-500">
              Ported codes are <b class="text-slate-800">flat-rated</b> — {{ status.shortcode.sessionsUsed.toLocaleString() }} sessions so far and counting nothing.
              No packs, no quotas; you simply pay {{ ghs(status.shortcode.flatMonthly) }}/month.
            </p>
            <p v-else-if="status?.shortcode" class="mt-1 text-sm text-slate-500">
              <span class="font-bold text-slate-800">{{ status.shortcode.sessionsUsed.toLocaleString() }} / {{ status.shortcode.sessionsQuota.toLocaleString() }}</span> sessions used.
              Every dial counts against the quota — top up in one click when you need more.
            </p>
            <p v-else class="mt-1 text-sm text-slate-500">Your plan's sessions arrive with the code. Buy extra packs anytime via webcheckout.</p>
            <p v-if="isAggregator" class="mt-1 text-xs text-slate-400">Quotas are per short code — every client's code has its own counter, so one busy client can't drain the others.</p>
            <div v-if="!status?.shortcode?.flatMonthly" class="mt-3 grid gap-2 sm:grid-cols-4">
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
            <CheckCircleIcon v-if="stepDone.dial" class="h-5 w-5" /><span v-else>5</span>
          </div>
          <div class="min-w-0 flex-1">
            <h2 class="font-bold text-slate-900">{{ isAggregator ? "Dial your client's code — live" : 'Dial your code — live' }}</h2>
            <p class="mt-1 text-sm text-slate-500">
              Your starter flow is deployed: dynamic menus (<span class="font-mono text-xs">${{ '{' }}balance{{ '}' }}</span> templating), a live API call and a MoMo charge that fires
              a <span class="font-mono text-xs">payment.succeeded</span> webhook. Navigate it on the keypad like a real subscriber.
            </p>
            <p v-if="portPending" class="mt-2 flex items-center gap-1.5 rounded-lg bg-amber-50 px-3 py-2 text-xs font-semibold text-amber-700 ring-1 ring-amber-100">
              <ClockIcon class="h-4 w-4" /> The dialer unlocks the moment {{ portInfo?.provider }} approves your port — step 2 above.
            </p>
            <div class="mt-3 flex flex-wrap gap-2">
              <NuxtLink v-if="status?.shortcode && status.shortcode.status === 'active'" :to="`/dial?code=${status.shortcode.code}`"
                class="inline-flex items-center gap-1.5 rounded-lg bg-brand-600 px-3.5 py-2 text-xs font-bold text-white hover:bg-brand-500">
                <DevicePhoneMobileIcon class="h-3.5 w-3.5" /> Open the dialer
              </NuxtLink>
              <span v-else-if="status?.shortcode" class="inline-flex cursor-not-allowed items-center gap-1.5 rounded-lg bg-slate-100 px-3.5 py-2 text-xs font-bold text-slate-400">
                <DevicePhoneMobileIcon class="h-3.5 w-3.5" /> Dialer opens when the code is active
              </span>
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
