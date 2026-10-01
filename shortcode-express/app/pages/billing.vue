<script setup lang="ts">
import { BanknotesIcon, DocumentTextIcon, ExclamationTriangleIcon } from '@heroicons/vue/24/outline'
import type { Invoice, Plan } from '~/../shared/types'

useHead({ title: 'Billing & quotas · ShortCodeExpress' })

interface Usage {
  shortcodeId: string; code: string; label: string; plan: string; planName: string
  priceMonthly: number; quota: number; used: number; monthSessions: number
  overage: number; overageRate: number; overageCost: number; projectedTotal: number; utilisation: number
}
interface BillingData {
  business: { name: string; currency: string; billingEmail: string }
  plans: Plan[]
  invoices: Invoice[]
  usage: Usage[]
  summary: { monthlyRecurring: number; overageTotal: number; projectedNextInvoice: number; currency: string }
}

const { data } = await useFetch<BillingData>('/api/billing')
const money = (n: number) => `GHS ${n.toLocaleString('en-GH', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`
const statusStyle: Record<string, string> = { paid: 'bg-success-100 text-success-700', due: 'bg-amber-100 text-amber-700', overdue: 'bg-rose-100 text-rose-700' }
const fmt = (n: number) => n >= 1000 ? (n / 1000).toFixed(n >= 10000 ? 0 : 1) + 'k' : String(n)

const invoiceOption = computed(() => ({
  tooltip: { trigger: 'axis' as const },
  grid: { left: 8, right: 8, top: 16, bottom: 8, containLabel: true },
  xAxis: { type: 'category' as const, data: (data.value?.invoices ?? []).slice().reverse().map((i) => i.period) },
  yAxis: { type: 'value' as const, splitNumber: 3 },
  series: [{ type: 'bar' as const, data: (data.value?.invoices ?? []).slice().reverse().map((i) => i.total), itemStyle: { color: '#80004d', borderRadius: [4, 4, 0, 0] }, barMaxWidth: 40 }],
}))
</script>

<template>
  <div class="p-8">
    <div class="mb-6">
      <h1 class="text-2xl font-bold text-slate-900">Billing &amp; quotas</h1>
      <p class="mt-1 text-sm text-slate-500">Session-based billing for {{ data?.business.name }} — quotas per short code, overage and invoices.</p>
    </div>

    <div class="grid gap-4 sm:grid-cols-3">
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
        <div class="flex items-center justify-between text-slate-500"><span class="text-xs font-semibold uppercase tracking-wide">Monthly recurring</span><BanknotesIcon class="h-5 w-5 text-brand-500" /></div>
        <div class="mt-2 text-3xl font-extrabold text-slate-900">{{ money(data?.summary.monthlyRecurring ?? 0) }}</div>
        <div class="mt-1 text-xs text-slate-500">{{ data?.plans.length }} plans · {{ data?.usage.length }} short codes</div>
      </div>
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
        <div class="flex items-center justify-between text-slate-500"><span class="text-xs font-semibold uppercase tracking-wide">Overage this cycle</span><ExclamationTriangleIcon class="h-5 w-5 text-amber-500" /></div>
        <div class="mt-2 text-3xl font-extrabold" :class="(data?.summary.overageTotal ?? 0) > 0 ? 'text-amber-600' : 'text-slate-900'">{{ money(data?.summary.overageTotal ?? 0) }}</div>
        <div class="mt-1 text-xs text-slate-500">Charged at each plan's per-session rate beyond quota</div>
      </div>
      <div class="rounded-2xl bg-gradient-to-br from-brand-700 to-brand-500 p-5 text-white shadow-lg shadow-brand-200">
        <div class="flex items-center justify-between"><span class="text-xs font-semibold uppercase tracking-wide text-brand-100">Projected next invoice</span><DocumentTextIcon class="h-5 w-5 text-brand-200" /></div>
        <div class="mt-2 text-3xl font-extrabold">{{ money(data?.summary.projectedNextInvoice ?? 0) }}</div>
        <div class="mt-1 text-xs text-brand-100">Billed to {{ data?.business.billingEmail }}</div>
      </div>
    </div>

    <!-- usage table -->
    <div class="mt-6 rounded-2xl bg-white shadow-sm ring-1 ring-slate-200/70">
      <div class="px-5 py-4"><h3 class="text-sm font-bold text-slate-800">Usage by short code — current cycle</h3></div>
      <div class="overflow-x-auto">
        <table class="w-full text-sm">
          <thead>
            <tr class="border-t border-slate-100 text-left text-xs uppercase tracking-wide text-slate-400">
              <th class="px-5 py-3 font-semibold">Short code</th>
              <th class="px-5 py-3 font-semibold">Plan</th>
              <th class="px-5 py-3 font-semibold">Quota</th>
              <th class="px-5 py-3 font-semibold">Used</th>
              <th class="px-5 py-3 font-semibold">Overage</th>
              <th class="px-5 py-3 font-semibold text-right">Projected</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="u in data?.usage ?? []" :key="u.shortcodeId" class="border-t border-slate-50">
              <td class="px-5 py-3.5"><span class="font-mono font-semibold text-slate-800">{{ u.code }}</span><span class="ml-2 text-xs text-slate-400">{{ u.label }}</span></td>
              <td class="px-5 py-3.5"><span class="rounded-full bg-brand-50 px-2 py-0.5 text-[11px] font-bold text-brand-700">{{ u.planName }}</span></td>
              <td class="px-5 py-3.5 text-slate-600">{{ fmt(u.quota) }}</td>
              <td class="px-5 py-3.5">
                <div class="flex items-center gap-2">
                  <span class="font-semibold text-slate-800">{{ fmt(u.used) }}</span>
                  <div class="h-1.5 w-20 overflow-hidden rounded-full bg-slate-100">
                    <div class="h-full rounded-full" :class="u.utilisation > 90 ? 'bg-rose-500' : u.utilisation > 70 ? 'bg-amber-500' : 'bg-brand-500'" :style="{ width: Math.min(100, u.utilisation) + '%' }" />
                  </div>
                </div>
              </td>
              <td class="px-5 py-3.5">
                <span v-if="u.overage > 0" class="font-semibold text-amber-600">{{ fmt(u.overage) }} × GHS {{ u.overageRate }} = {{ money(u.overageCost) }}</span>
                <span v-else class="text-slate-400">—</span>
              </td>
              <td class="px-5 py-3.5 text-right font-bold text-slate-900">{{ money(u.projectedTotal) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <div class="mt-6 grid gap-6 lg:grid-cols-3">
      <!-- invoices -->
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70 lg:col-span-2">
        <h3 class="mb-3 text-sm font-bold text-slate-800">Invoices</h3>
        <table class="w-full text-sm">
          <thead>
            <tr class="text-left text-xs uppercase tracking-wide text-slate-400">
              <th class="py-2 font-semibold">Invoice</th><th class="py-2 font-semibold">Period</th>
              <th class="py-2 font-semibold">Issued</th><th class="py-2 font-semibold">Status</th>
              <th class="py-2 text-right font-semibold">Total</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="inv in data?.invoices ?? []" :key="inv.id" class="border-t border-slate-50">
              <td class="py-3 font-mono text-xs font-semibold text-brand-700">{{ inv.number }}</td>
              <td class="py-3 text-slate-600">{{ inv.period }}</td>
              <td class="py-3 text-slate-500">{{ new Date(inv.issuedAt).toLocaleDateString() }}</td>
              <td class="py-3"><span class="rounded-full px-2 py-0.5 text-[11px] font-bold" :class="statusStyle[inv.status]">{{ inv.status }}</span></td>
              <td class="py-3 text-right font-bold text-slate-900">{{ money(inv.total) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <!-- invoice chart + plans -->
      <div class="space-y-6">
        <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
          <h3 class="mb-2 text-sm font-bold text-slate-800">Invoice history</h3>
          <EChart :option="invoiceOption" height="180px" />
        </div>
        <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
          <h3 class="mb-3 text-sm font-bold text-slate-800">Plans</h3>
          <div class="space-y-3">
            <div v-for="p in data?.plans ?? []" :key="p.id" class="rounded-xl border border-slate-100 p-3">
              <div class="flex items-center justify-between">
                <span class="text-sm font-bold text-slate-800">{{ p.name }}</span>
                <span class="text-sm font-extrabold text-brand-700">GHS {{ p.priceMonthly }}<span class="text-[10px] font-medium text-slate-400">/mo</span></span>
              </div>
              <div class="mt-1 text-[11px] text-slate-500">{{ p.sessionQuota.toLocaleString() }} sessions · overage GHS {{ p.overagePerSession }}/session</div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
