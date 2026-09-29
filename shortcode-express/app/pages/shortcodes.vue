<script setup lang="ts">
import { PlusIcon, SignalIcon, DevicePhoneMobileIcon, BoltSlashIcon, WrenchScrewdriverIcon, ArrowPathIcon } from '@heroicons/vue/24/outline'
import type { Plan, SessionPack, ShortCode } from '~/../shared/types'

definePageMeta({ middleware: 'auth' })
useHead({ title: 'Short codes · ShortCodeExpress' })

interface Row extends ShortCode { planDetails?: Plan; flowName: string | null }
type RowFull = Row & { mine?: boolean }

const { data: rows, refresh } = await useFetch<Row[]>('/api/shortcodes')
const me = await $fetch<{ id: string }>('/api/auth/me').catch(() => null)
const packs = (await $fetch<{ packs: SessionPack[] }>('/api/packs')).packs

const showTopup = ref<RowFull | null>(null)
const busyTopup = ref('')

async function topup(row: RowFull, packId: string) {
  busyTopup.value = packId
  try {
    const co = await $fetch<{ id: string }>('/api/checkout', { method: 'POST', body: { kind: 'topup', shortcodeId: row.id, packId } })
    navigateTo(`/checkout/${co.id}`)
  } finally { busyTopup.value = '' }
}

async function activate(sc: Row) {
  await $fetch(`/api/shortcodes/${sc.id}`, { method: 'PATCH', body: { status: sc.status === 'active' ? 'suspended' : 'active' } })
  await refresh()
}

async function changePlan(sc: Row, plan: string) {
  await $fetch(`/api/shortcodes/${sc.id}`, { method: 'PATCH', body: { plan } })
  await refresh()
}

const statusColor: Record<string, string> = { active: 'bg-success-100 text-success-700 ring-success-200', provisioning: 'bg-amber-100 text-amber-700 ring-amber-200', suspended: 'bg-rose-100 text-rose-700 ring-rose-200' }
const fmt = (n: number) => n >= 1000 ? (n / 1000).toFixed(n >= 10000 ? 0 : 1) + 'k' : String(n)
const ghs = (n: number) => `GHS ${n.toLocaleString()}`
const decorated = computed<RowFull[]>(() => (rows.value ?? []).map((r) => ({ ...r, mine: !!me && r.ownerId === me.id })))
</script>

<template>
  <div class="p-8">
    <div class="mb-6 flex items-end justify-between">
      <div>
        <h1 class="text-2xl font-bold text-slate-900">Short codes</h1>
        <p class="mt-1 text-sm text-slate-500">Codes provisioned on the mobile networks, with per-code session quotas. ★ = yours.</p>
      </div>
      <NuxtLink to="/onboarding" class="inline-flex items-center gap-2 rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500">
        <PlusIcon class="h-4 w-4" /> Buy a short code
      </NuxtLink>
    </div>

    <div class="grid gap-4 lg:grid-cols-2">
      <div v-for="sc in decorated" :key="sc.id" class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200/70">
        <div class="flex items-start justify-between">
          <div class="flex items-center gap-3">
            <div class="flex h-12 w-12 items-center justify-center rounded-2xl bg-gradient-to-br from-brand-600 to-brand-400 font-mono text-lg font-bold text-white shadow-lg shadow-brand-200">
              <SignalIcon class="h-6 w-6" />
            </div>
            <div>
              <div class="flex items-center gap-2 font-mono text-lg font-bold text-slate-900">
                {{ sc.code }}
                <span v-if="sc.mine" title="Your code" class="text-amber-400">★</span>
              </div>
              <div class="text-xs text-slate-500">
                {{ sc.label }} · {{ sc.network }}
                <span v-if="sc.assignedBy" class="ml-1 rounded bg-slate-100 px-1.5 py-0.5 text-[10px] font-semibold text-slate-500">{{ sc.assignedBy === 'user' ? 'picked by you' : 'system-assigned' }}</span>
              </div>
            </div>
          </div>
          <span class="rounded-full px-2.5 py-1 text-[11px] font-bold uppercase ring-1" :class="statusColor[sc.status]">{{ sc.status }}</span>
        </div>

        <div class="mt-5 grid grid-cols-3 gap-3 text-center">
          <div class="rounded-xl bg-slate-50 py-3">
            <div class="text-xl font-extrabold text-slate-900">{{ fmt(sc.sessionsUsed) }}</div>
            <div class="text-[11px] font-medium text-slate-500">sessions used</div>
          </div>
          <div class="rounded-xl bg-slate-50 py-3">
            <div class="text-xl font-extrabold text-slate-900">{{ fmt(sc.sessionsQuota) }}</div>
            <div class="text-[11px] font-medium text-slate-500">quota <span v-if="sc.sessionsUsed >= sc.sessionsQuota" class="font-bold text-rose-500">· exhausted</span></div>
          </div>
          <div class="rounded-xl bg-slate-50 py-3">
            <div class="text-xl font-extrabold text-slate-900">{{ sc.planDetails?.name ?? sc.plan }}</div>
            <div class="text-[11px] font-medium text-slate-500">plan</div>
          </div>
        </div>

        <div class="mt-3 h-1.5 overflow-hidden rounded-full bg-slate-100">
          <div class="h-full rounded-full" :class="sc.sessionsUsed / Math.max(sc.sessionsQuota, 1) > 0.9 ? 'bg-rose-500' : sc.sessionsUsed / Math.max(sc.sessionsQuota, 1) > 0.7 ? 'bg-amber-500' : 'bg-success-500'"
            :style="{ width: Math.min(100, (sc.sessionsUsed / Math.max(sc.sessionsQuota, 1)) * 100) + '%' }" />
        </div>

        <div class="mt-4 flex flex-wrap items-center gap-2">
          <NuxtLink :to="`/dial?code=${sc.code}`" class="inline-flex items-center gap-1.5 rounded-lg bg-brand-600 px-3 py-2 text-xs font-bold text-white hover:bg-brand-500">
            <DevicePhoneMobileIcon class="h-3.5 w-3.5" /> Dial
          </NuxtLink>
          <button class="inline-flex items-center gap-1.5 rounded-lg bg-slate-900 px-3 py-2 text-xs font-bold text-white hover:bg-slate-800" @click="showTopup = sc">
            <BoltSlashIcon class="h-3.5 w-3.5" /> Top up sessions
          </button>
          <NuxtLink v-if="sc.flowId" :to="`/builder/${sc.flowId}`" class="inline-flex items-center gap-1.5 rounded-lg bg-white px-3 py-2 text-xs font-bold text-slate-700 ring-1 ring-slate-200 hover:bg-slate-50">
            <WrenchScrewdriverIcon class="h-3.5 w-3.5" /> {{ sc.flowName ?? 'Flow' }}
          </NuxtLink>
          <select :value="sc.plan" class="ml-auto rounded-lg border border-slate-200 bg-white px-2 py-1.5 text-xs font-semibold text-slate-600" @change="changePlan(sc, ($event.target as HTMLSelectElement).value)">
            <option v-for="p in sc.planDetails ? [sc.planDetails] : []" :key="p.id" :value="p.id">{{ p.name }}</option>
            <option v-for="p in ['starter', 'growth', 'scale'].filter((x) => x !== sc.plan)" :key="p" :value="p">{{ p }}</option>
          </select>
          <button class="rounded-lg px-2.5 py-2 text-xs font-semibold text-slate-500 hover:bg-slate-100" @click="activate(sc)">
            {{ sc.status === 'active' ? 'Suspend' : 'Activate' }}
          </button>
        </div>
      </div>
    </div>

    <!-- top-up modal -->
    <div v-if="showTopup" class="fixed inset-0 z-50 flex items-center justify-center bg-slate-900/50 p-4" @click.self="showTopup = null">
      <div class="w-full max-w-lg rounded-2xl bg-white p-6 shadow-2xl">
        <div class="flex items-start justify-between">
          <div>
            <h3 class="text-lg font-bold text-slate-900">Top up sessions</h3>
            <p class="mt-1 text-sm text-slate-500"><span class="font-mono font-bold">{{ showTopup.code }}</span> · {{ showTopup.sessionsUsed.toLocaleString() }} / {{ showTopup.sessionsQuota.toLocaleString() }} used</p>
          </div>
          <button class="rounded-lg p-1.5 text-slate-400 hover:bg-slate-100" @click="showTopup = null">✕</button>
        </div>
        <div class="mt-4 grid gap-3 sm:grid-cols-2">
          <button v-for="pk in packs" :key="pk.id" :disabled="!!busyTopup"
            class="rounded-xl border-2 border-slate-200 p-4 text-left transition hover:border-brand-400 hover:bg-brand-50/40 disabled:opacity-40" @click="topup(showTopup, pk.id)">
            <div class="flex items-center justify-between">
              <span class="text-base font-extrabold text-slate-900">{{ pk.sessions.toLocaleString() }}</span>
              <ArrowPathIcon v-if="busyTopup === pk.id" class="h-4 w-4 animate-spin text-brand-600" />
            </div>
            <div class="text-xs text-slate-500">sessions</div>
            <div class="mt-2 text-sm font-bold text-brand-700">{{ ghs(pk.price) }} <span class="font-normal text-slate-400">· GHS {{ pk.perSession.toFixed(4) }}/session</span></div>
          </button>
        </div>
        <p class="mt-3 text-[11px] text-slate-400">Pay by mobile money or card in the secure webcheckout — sessions credit instantly and a webhook fires.</p>
      </div>
    </div>
  </div>
</template>
