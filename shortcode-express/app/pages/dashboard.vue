<script setup lang="ts">
import { ArrowTrendingUpIcon, ArrowTrendingDownIcon, SignalIcon, BanknotesIcon, CheckBadgeIcon, ClockIcon } from '@heroicons/vue/24/outline'

definePageMeta({ middleware: 'auth' })
useHead({ title: 'Dashboard · ShortCodeExpress' })
const { refresh } = useAuth()
refresh()

interface ScAgg { id: string; code: string; label: string; plan: string; status: string; network: string; sessions: number; completed: number; failed: number; revenue: number; sessionsUsed: number; sessionsQuota: number; flat: boolean; flatMonthly: number | null; completionRate: number; flowName: string | null; series: number[] }
interface Stats {
  currency: string
  today: { sessions: number; hourly: number[] }
  kpis: { sessions30d: number; sessionsToday: number; sessionsTodayDelta: number; completed30d: number; failed30d: number; completionRate: number; revenue30d: number; revenueMtd: number; wowChange: number; activeShortcodes: number; totalShortcodes: number; avgDurationSec: number }
  trend: { dates: string[]; sessions: number[]; completed: number[]; failed: number[]; revenue: number[] }
  shortcodes: ScAgg[]
  networks: Record<string, number>
  topFlows: Array<{ flowId: string; name: string; sessions: number; completion: number }>
}

const { data: stats } = await useFetch<Stats>('/api/stats')

const fmt = (n: number) => n >= 1_000_000 ? (n / 1e6).toFixed(1) + 'M' : n >= 1000 ? (n / 1e3).toFixed(1) + 'k' : String(Math.round(n))
const money = (n: number) => `GHS ${n >= 1000 ? (n / 1000).toFixed(1) + 'k' : n.toFixed(0)}`

const shortDate = (d: string) => d.slice(5).replace('-', '/')

const sessionsOption = computed(() => ({
  tooltip: { trigger: 'axis' as const },
  legend: { data: ['Sessions', 'Completed', 'Failed'], top: 0, textStyle: { fontSize: 11, color: '#64748b' }, itemWidth: 14 },
  grid: { left: 8, right: 16, top: 34, bottom: 8, containLabel: true },
  xAxis: { type: 'category' as const, data: stats.value?.trend.dates.map(shortDate) ?? [], boundaryGap: false },
  yAxis: { type: 'value' as const, splitNumber: 4 },
  series: [
    {
      name: 'Sessions', type: 'line' as const, smooth: true, symbol: 'none', data: stats.value?.trend.sessions ?? [],
      lineStyle: { width: 3, color: '#80004d' }, areaStyle: { color: { type: 'linear', x: 0, y: 0, x2: 0, y2: 1, colorStops: [{ offset: 0, color: 'rgba(128,0,77,0.25)' }, { offset: 1, color: 'rgba(128,0,77,0)' }] } },
    },
    { name: 'Completed', type: 'line' as const, smooth: true, symbol: 'none', data: stats.value?.trend.completed ?? [], lineStyle: { width: 1.5, color: '#4bb543' } },
    { name: 'Failed', type: 'line' as const, smooth: true, symbol: 'none', data: stats.value?.trend.failed ?? [], lineStyle: { width: 1.5, color: '#f43f5e' } },
  ],
}))

const revenueOption = computed(() => ({
  tooltip: { trigger: 'axis' as const, valueFormatter: (v: number) => `GHS ${v.toFixed(2)}` },
  grid: { left: 8, right: 8, top: 16, bottom: 8, containLabel: true },
  xAxis: { type: 'category' as const, data: stats.value?.trend.dates.map(shortDate) ?? [] },
  yAxis: { type: 'value' as const, splitNumber: 4, axisLabel: { formatter: (v: number) => (v >= 1000 ? (v / 1000) + 'k' : String(v)) } },
  series: [{
    name: 'Revenue', type: 'bar' as const, data: stats.value?.trend.revenue ?? [],
    itemStyle: { color: '#0ea5e9', borderRadius: [3, 3, 0, 0] }, barMaxWidth: 14,
  }],
}))

const networkOption = computed(() => ({
  tooltip: { trigger: 'item' as const, formatter: '{b}: {c}%' },
  legend: { bottom: 0, textStyle: { fontSize: 11, color: '#64748b' } },
  series: [{
    type: 'pie' as const, radius: ['52%', '76%'], center: ['50%', '42%'],
    itemStyle: { borderRadius: 6, borderColor: '#fff', borderWidth: 2 },
    label: { show: false },
    data: Object.entries(stats.value?.networks ?? {}).map(([name, value]) => ({
      name, value,
      itemStyle: { color: name === 'MTN' ? '#facc15' : name === 'AirtelTigo' ? '#0ea5e9' : '#ef4444' },
    })),
  }],
}))

const hourlyOption = computed(() => ({
  tooltip: { trigger: 'axis' as const },
  grid: { left: 8, right: 8, top: 16, bottom: 8, containLabel: true },
  xAxis: { type: 'category' as const, data: stats.value?.today.hourly.map((_, i) => `${String(i).padStart(2, '0')}:00`) ?? [] },
  yAxis: { type: 'value' as const, splitNumber: 3 },
  series: [{
    type: 'bar' as const, data: stats.value?.today.hourly ?? [],
    itemStyle: { color: (p: { dataIndex: number }) => (p.dataIndex === new Date().getHours() ? '#80004d' : '#ec9dc4'), borderRadius: [3, 3, 0, 0] },
    barMaxWidth: 12,
  }],
}))

const statusColor: Record<string, string> = { active: 'bg-success-100 text-success-700', provisioning: 'bg-amber-100 text-amber-700', suspended: 'bg-rose-100 text-rose-700' }
</script>

<template>
  <div class="p-8">
    <div class="mb-6 flex items-end justify-between">
      <div>
        <h1 class="text-2xl font-bold text-slate-900">Operations dashboard</h1>
        <p class="mt-1 text-sm text-slate-500">Live usage, revenue and health across every provisioned short code.</p>
      </div>
      <div class="flex gap-2">
        <NuxtLink to="/flows" class="rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500">Open flows</NuxtLink>
        <NuxtLink to="/shortcodes" class="rounded-lg border border-slate-200 bg-white px-4 py-2 text-sm font-semibold text-slate-700 hover:border-slate-300">Provision short code</NuxtLink>
      </div>
    </div>

    <!-- KPI cards -->
    <div class="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
        <div class="flex items-center justify-between text-slate-500"><span class="text-xs font-semibold uppercase tracking-wide">Sessions today</span><SignalIcon class="h-5 w-5 text-brand-500" /></div>
        <div class="mt-2 text-3xl font-extrabold text-slate-900">{{ fmt(stats?.kpis.sessionsToday ?? 0) }}</div>
        <div class="mt-1 flex items-center gap-1 text-xs" :class="(stats?.kpis.sessionsTodayDelta ?? 0) >= 0 ? 'text-success-600' : 'text-rose-600'">
          <component :is="(stats?.kpis.sessionsTodayDelta ?? 0) >= 0 ? ArrowTrendingUpIcon : ArrowTrendingDownIcon" class="h-4 w-4" />
          {{ Math.abs(stats?.kpis.sessionsTodayDelta ?? 0) }}% vs yesterday · {{ fmt(stats?.kpis.sessions30d ?? 0) }} in 30d
        </div>
      </div>
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
        <div class="flex items-center justify-between text-slate-500"><span class="text-xs font-semibold uppercase tracking-wide">Revenue MTD</span><BanknotesIcon class="h-5 w-5 text-sky-500" /></div>
        <div class="mt-2 text-3xl font-extrabold text-slate-900">{{ money(stats?.kpis.revenueMtd ?? 0) }}</div>
        <div class="mt-1 text-xs text-slate-500">{{ money(stats?.kpis.revenue30d ?? 0) }} in the last 30 days</div>
      </div>
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
        <div class="flex items-center justify-between text-slate-500"><span class="text-xs font-semibold uppercase tracking-wide">Completion rate</span><CheckBadgeIcon class="h-5 w-5 text-success-500" /></div>
        <div class="mt-2 text-3xl font-extrabold text-slate-900">{{ stats?.kpis.completionRate ?? 0 }}%</div>
        <div class="mt-1 text-xs text-slate-500">{{ fmt(stats?.kpis.completed30d ?? 0) }} completed · {{ fmt(stats?.kpis.failed30d ?? 0) }} failed (30d)</div>
      </div>
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
        <div class="flex items-center justify-between text-slate-500"><span class="text-xs font-semibold uppercase tracking-wide">Short codes</span><SignalIcon class="h-5 w-5 text-amber-500" /></div>
        <div class="mt-2 text-3xl font-extrabold text-slate-900">{{ stats?.kpis.activeShortcodes ?? 0 }}<span class="text-lg text-slate-400">/{{ stats?.kpis.totalShortcodes ?? 0 }}</span></div>
        <div class="mt-1 flex items-center gap-1 text-xs text-slate-500"><ClockIcon class="h-3.5 w-3.5" /> avg session {{ stats?.kpis.avgDurationSec ?? 0 }}s</div>
      </div>
    </div>

    <!-- charts row 1 -->
    <div class="mt-6 grid gap-6 lg:grid-cols-3">
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70 lg:col-span-2">
        <h3 class="mb-2 text-sm font-bold text-slate-800">Sessions — last 30 days</h3>
        <EChart :option="sessionsOption" height="290px" />
      </div>
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
        <h3 class="mb-2 text-sm font-bold text-slate-800">Network split</h3>
        <EChart :option="networkOption" height="290px" />
      </div>
    </div>

    <!-- charts row 2 -->
    <div class="mt-6 grid gap-6 lg:grid-cols-3">
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70 lg:col-span-2">
        <h3 class="mb-2 text-sm font-bold text-slate-800">Session revenue — last 30 days</h3>
        <EChart :option="revenueOption" height="240px" />
      </div>
      <div class="rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70">
        <h3 class="mb-2 text-sm font-bold text-slate-800">Today's hourly load</h3>
        <EChart :option="hourlyOption" height="240px" />
      </div>
    </div>

    <!-- short code table -->
    <div class="mt-6 rounded-2xl bg-white shadow-sm ring-1 ring-slate-200/70">
      <div class="flex items-center justify-between px-5 py-4">
        <h3 class="text-sm font-bold text-slate-800">Short codes &amp; quota utilisation</h3>
        <NuxtLink to="/billing" class="text-xs font-semibold text-brand-600 hover:text-brand-500">Billing details →</NuxtLink>
      </div>
      <div class="overflow-x-auto">
        <table class="w-full text-sm">
          <thead>
            <tr class="border-t border-slate-100 text-left text-xs uppercase tracking-wide text-slate-400">
              <th class="px-5 py-3 font-semibold">Short code</th>
              <th class="px-5 py-3 font-semibold">Flow</th>
              <th class="px-5 py-3 font-semibold">Sessions (30d)</th>
              <th class="px-5 py-3 font-semibold">Completion</th>
              <th class="px-5 py-3 font-semibold">Revenue</th>
              <th class="px-5 py-3 font-semibold">Quota used</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="sc in stats?.shortcodes ?? []" :key="sc.id" class="border-t border-slate-50 hover:bg-slate-50/60">
              <td class="px-5 py-3.5">
                <div class="font-mono text-[13px] font-semibold text-slate-800">{{ sc.code }}</div>
                <div class="mt-0.5 flex items-center gap-2">
                  <span class="text-xs text-slate-400">{{ sc.label }}</span>
                  <span class="rounded px-1.5 py-0.5 text-[10px] font-bold uppercase" :class="statusColor[sc.status]">{{ sc.status }}</span>
                </div>
              </td>
              <td class="px-5 py-3.5 text-slate-600">{{ sc.flowName ?? '—' }}</td>
              <td class="px-5 py-3.5 font-semibold text-slate-800">{{ fmt(sc.sessions) }}</td>
              <td class="px-5 py-3.5">
                <div class="flex items-center gap-2">
                  <div class="h-1.5 w-16 overflow-hidden rounded-full bg-slate-100">
                    <div class="h-full rounded-full bg-success-500" :style="{ width: sc.completionRate + '%' }" />
                  </div>
                  <span class="text-xs text-slate-500">{{ sc.completionRate }}%</span>
                </div>
              </td>
              <td class="px-5 py-3.5 font-semibold text-slate-800">{{ money(sc.revenue) }}</td>
              <td class="px-5 py-3.5">
                <div class="w-40">
                  <template v-if="sc.flat">
                    <div class="flex items-center gap-1.5 text-[11px] font-semibold text-slate-600">
                      {{ fmt(sc.sessionsUsed) }} / ∞ <span class="rounded bg-brand-50 px-1.5 py-0.5 text-[10px] font-bold text-brand-700">flat {{ sc.flatMonthly ?? 105 }}/mo</span>
                    </div>
                    <div class="mt-1 h-1.5 overflow-hidden rounded-full bg-brand-100">
                      <div class="h-full w-full rounded-full bg-brand-400" />
                    </div>
                  </template>
                  <template v-else>
                    <div class="flex justify-between text-[11px] text-slate-500">
                      <span>{{ fmt(sc.sessionsUsed) }} / {{ fmt(sc.sessionsQuota) }}</span>
                      <span :class="sc.sessionsUsed / Math.max(1, sc.sessionsQuota) > 0.9 ? 'font-bold text-rose-600' : ''">{{ Math.round((sc.sessionsUsed / Math.max(1, sc.sessionsQuota)) * 100) }}%</span>
                    </div>
                    <div class="mt-1 h-1.5 overflow-hidden rounded-full bg-slate-100">
                      <div
                        class="h-full rounded-full" :class="sc.sessionsUsed / Math.max(1, sc.sessionsQuota) > 0.9 ? 'bg-rose-500' : sc.sessionsUsed / Math.max(1, sc.sessionsQuota) > 0.7 ? 'bg-amber-500' : 'bg-brand-500'"
                        :style="{ width: Math.min(100, (sc.sessionsUsed / Math.max(1, sc.sessionsQuota)) * 100) + '%' }"
                      />
                    </div>
                  </template>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>
</template>
