<script setup lang="ts">
import { PlusIcon, SignalIcon } from '@heroicons/vue/24/outline'
import type { Plan, ShortCode } from '~/../shared/types'

definePageMeta({ middleware: 'auth' })
useHead({ title: 'Short codes · ShortCodeExpress' })

interface Row extends ShortCode { planDetails?: Plan; flowName: string | null }

const { data: rows, refresh } = await useFetch<Row[]>('/api/shortcodes')
const flows = await useFetch<{ id: string; name: string }[]>('/api/flows')
const showNew = ref(false)
const saving = ref(false)
const form = reactive({ code: '', label: '', network: 'MTN', plan: 'starter', flowId: '' })
const error = ref('')

async function provision() {
  error.value = ''
  saving.value = true
  try {
    await $fetch('/api/shortcodes', { method: 'POST', body: { ...form, flowId: form.flowId || undefined } })
    showNew.value = false
    form.code = ''; form.label = ''; form.flowId = ''
    await refresh()
  } catch (e: unknown) {
    error.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not provision short code'
  } finally {
    saving.value = false
  }
}

async function activate(sc: Row) {
  await $fetch(`/api/shortcodes/${sc.id}`, { method: 'PATCH', body: { status: sc.status === 'active' ? 'suspended' : 'active' } })
  await refresh()
}

async function changePlan(sc: Row, plan: string) {
  await $fetch(`/api/shortcodes/${sc.id}`, { method: 'PATCH', body: { plan } })
  await refresh()
}

const statusColor: Record<string, string> = { active: 'bg-emerald-100 text-emerald-700 ring-emerald-200', provisioning: 'bg-amber-100 text-amber-700 ring-amber-200', suspended: 'bg-rose-100 text-rose-700 ring-rose-200' }
const fmt = (n: number) => n >= 1000 ? (n / 1000).toFixed(n >= 10000 ? 0 : 1) + 'k' : String(n)
</script>

<template>
  <div class="p-8">
    <div class="mb-6 flex items-end justify-between">
      <div>
        <h1 class="text-2xl font-bold text-slate-900">Short codes</h1>
        <p class="mt-1 text-sm text-slate-500">Codes provisioned on the mobile networks, with per-code session quotas.</p>
      </div>
      <button class="inline-flex items-center gap-2 rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500" @click="showNew = true">
        <PlusIcon class="h-4 w-4" /> Provision code
      </button>
    </div>

    <div class="grid gap-4 lg:grid-cols-2">
      <div v-for="sc in rows ?? []" :key="sc.id" class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200/70">
        <div class="flex items-start justify-between">
          <div class="flex items-center gap-3">
            <div class="flex h-12 w-12 items-center justify-center rounded-2xl bg-gradient-to-br from-brand-500 to-fuchsia-600 font-mono text-lg font-bold text-white shadow-lg shadow-brand-200">
              <SignalIcon class="h-6 w-6" />
            </div>
            <div>
              <div class="font-mono text-lg font-bold text-slate-900">{{ sc.code }}</div>
              <div class="text-xs text-slate-500">{{ sc.label }} · {{ sc.network }}</div>
            </div>
          </div>
          <span class="rounded-full px-2.5 py-1 text-[11px] font-bold uppercase ring-1" :class="statusColor[sc.status]">{{ sc.status }}</span>
        </div>

        <div class="mt-5 grid grid-cols-3 gap-3 text-center">
          <div class="rounded-xl bg-slate-50 py-3">
            <div class="text-xl font-extrabold text-slate-900">{{ fmt(sc.sessionsUsed) }}</div>
            <div class="text-[10px] font-semibold uppercase tracking-wide text-slate-400">sessions used</div>
          </div>
          <div class="rounded-xl bg-slate-50 py-3">
            <div class="text-xl font-extrabold text-slate-900">{{ fmt(sc.sessionsQuota) }}</div>
            <div class="text-[10px] font-semibold uppercase tracking-wide text-slate-400">quota / month</div>
          </div>
          <div class="rounded-xl bg-slate-50 py-3">
            <div class="text-xl font-extrabold" :class="sc.sessionsUsed / Math.max(1, sc.sessionsQuota) > 0.9 ? 'text-rose-600' : 'text-slate-900'">
              {{ Math.round((sc.sessionsUsed / Math.max(1, sc.sessionsQuota)) * 100) }}%
            </div>
            <div class="text-[10px] font-semibold uppercase tracking-wide text-slate-400">utilisation</div>
          </div>
        </div>

        <div class="mt-4">
          <div class="h-2 overflow-hidden rounded-full bg-slate-100">
            <div
              class="h-full rounded-full" :class="sc.sessionsUsed / Math.max(1, sc.sessionsQuota) > 0.9 ? 'bg-rose-500' : sc.sessionsUsed / Math.max(1, sc.sessionsQuota) > 0.7 ? 'bg-amber-500' : 'bg-gradient-to-r from-brand-500 to-fuchsia-500'"
              :style="{ width: Math.min(100, (sc.sessionsUsed / Math.max(1, sc.sessionsQuota)) * 100) + '%' }"
            />
          </div>
          <div v-if="sc.sessionsUsed > sc.sessionsQuota" class="mt-2 text-[11px] font-medium text-rose-600">
            ⚠ Over quota by {{ fmt(sc.sessionsUsed - sc.sessionsQuota) }} sessions — overage billed at GHS {{ sc.planDetails?.overagePerSession }}/session.
          </div>
        </div>

        <div class="mt-5 flex flex-wrap items-center gap-2 border-t border-slate-100 pt-4">
          <select :value="sc.plan" class="rounded-lg border border-slate-200 bg-white px-2.5 py-1.5 text-xs font-semibold text-slate-700 focus:border-brand-400 focus:outline-none" @change="changePlan(sc, ($event.target as HTMLSelectElement).value)">
            <option value="starter">Starter · GHS 149/mo</option>
            <option value="growth">Growth · GHS 499/mo</option>
            <option value="scale">Scale · GHS 1,499/mo</option>
          </select>
          <span class="text-xs text-slate-400">{{ sc.flowName ? `serves: ${sc.flowName}` : 'no flow linked' }}</span>
          <button
            class="ml-auto rounded-lg px-3 py-1.5 text-xs font-semibold"
            :class="sc.status === 'active' ? 'text-rose-600 hover:bg-rose-50' : 'text-emerald-700 hover:bg-emerald-50'"
            @click="activate(sc)"
          >
            {{ sc.status === 'active' ? 'Suspend' : 'Activate' }}
          </button>
        </div>
      </div>
    </div>

    <div v-if="showNew" class="fixed inset-0 z-50 flex items-center justify-center bg-slate-900/40 p-4 backdrop-blur-sm" @click.self="showNew = false">
      <div class="w-full max-w-md rounded-2xl bg-white p-6 shadow-2xl">
        <h2 class="text-lg font-bold text-slate-900">Provision a short code</h2>
        <p class="mt-1 text-xs text-slate-500">New codes start in <b>provisioning</b> while the networks confirm routing.</p>
        <div class="mt-4 space-y-3">
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Code (USSD format)</label>
            <input v-model="form.code" placeholder="*713*9#" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-sm focus:border-brand-400 focus:outline-none" />
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Label</label>
            <input v-model="form.label" placeholder="e.g. MoMo Transfer Guard" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-sm focus:border-brand-400 focus:outline-none" />
          </div>
          <div class="grid grid-cols-2 gap-3">
            <div>
              <label class="mb-1 block text-xs font-semibold text-slate-600">Network</label>
              <select v-model="form.network" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-sm focus:border-brand-400">
                <option>MTN</option><option>Vodafone</option><option>AirtelTigo</option><option>All networks</option>
              </select>
            </div>
            <div>
              <label class="mb-1 block text-xs font-semibold text-slate-600">Plan</label>
              <select v-model="form.plan" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-sm focus:border-brand-400">
                <option value="starter">Starter</option><option value="growth">Growth</option><option value="scale">Scale</option>
              </select>
            </div>
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Flow to serve</label>
            <select v-model="form.flowId" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-sm focus:border-brand-400">
              <option value="">Link later</option>
              <option v-for="f in flows.data.value ?? []" :key="f.id" :value="f.id">{{ f.name }}</option>
            </select>
          </div>
          <p v-if="error" class="text-xs text-rose-600">{{ error }}</p>
        </div>
        <div class="mt-5 flex justify-end gap-2">
          <button class="rounded-lg px-4 py-2 text-sm font-semibold text-slate-600 hover:bg-slate-100" @click="showNew = false">Cancel</button>
          <button :disabled="saving" class="rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500 disabled:opacity-60" @click="provision()">
            {{ saving ? 'Provisioning…' : 'Provision' }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
