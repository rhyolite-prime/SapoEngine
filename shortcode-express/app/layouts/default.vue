<script setup lang="ts">
import {
  ChartBarIcon, Squares2X2Icon, SignalIcon, CreditCardIcon, UsersIcon,
  ArrowLeftOnRectangleIcon, BuildingOffice2Icon,
} from '@heroicons/vue/24/outline'

const route = useRoute()
const { user, logout } = useAuth()

const nav = [
  { to: '/dashboard', label: 'Dashboard', icon: ChartBarIcon },
  { to: '/flows', label: 'USSD flows', icon: Squares2X2Icon },
  { to: '/shortcodes', label: 'Short codes', icon: SignalIcon },
  { to: '/billing', label: 'Billing & quotas', icon: CreditCardIcon },
  { to: '/team', label: 'Team', icon: UsersIcon },
]
</script>

<template>
  <div class="min-h-screen">
    <aside class="fixed inset-y-0 left-0 z-40 flex w-64 flex-col bg-ink-950 text-slate-300">
      <NuxtLink to="/" class="flex items-center gap-3 px-5 pb-5 pt-6">
        <div class="flex h-10 w-10 items-center justify-center rounded-xl bg-gradient-to-br from-brand-500 to-fuchsia-600 shadow-lg shadow-brand-900/40">
          <svg viewBox="0 0 24 24" class="h-6 w-6 text-white" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
            <path d="M4 7h16M4 12h10M4 17h7"></path><circle cx="18.5" cy="15.5" r="2.5"></circle>
          </svg>
        </div>
        <div>
          <div class="text-[15px] font-bold leading-tight text-white">ShortCode<span class="text-brand-400">Express</span></div>
          <div class="text-[11px] text-slate-500">Powered by Sapo Engine</div>
        </div>
      </NuxtLink>

      <div class="mx-5 mb-4 flex items-center gap-2 rounded-lg bg-ink-900 px-3 py-2 text-xs text-slate-400 ring-1 ring-white/5">
        <BuildingOffice2Icon class="h-4 w-4 text-brand-400" />
        <span class="truncate font-medium text-slate-200">Rhyolite Prime</span>
        <span class="ml-auto rounded bg-emerald-500/10 px-1.5 py-0.5 text-[10px] font-semibold text-emerald-400">Ghana</span>
      </div>

      <nav class="flex-1 space-y-1 px-3">
        <NuxtLink
          v-for="item in nav" :key="item.to" :to="item.to"
          class="flex items-center gap-3 rounded-lg px-3 py-2.5 text-sm font-medium transition"
          :class="route.path.startsWith(item.to) ? 'bg-brand-600/20 text-white ring-1 ring-brand-500/40' : 'text-slate-400 hover:bg-white/5 hover:text-slate-100'"
        >
          <component :is="item.icon" class="h-5 w-5"></component>
          {{ item.label }}
        </NuxtLink>
      </nav>

      <div v-if="user" class="border-t border-white/5 p-4">
        <div class="flex items-center gap-3">
          <div class="flex h-9 w-9 items-center justify-center rounded-full text-sm font-bold text-white" :style="{ background: `hsl(${user.avatarHue} 65% 45%)` }">
            {{ user.name.split(' ').map((n: string) => n[0]).join('').slice(0, 2) }}
          </div>
          <div class="min-w-0 flex-1">
            <div class="truncate text-sm font-semibold text-slate-100">{{ user.name }}</div>
            <div class="truncate text-[11px] text-slate-500">{{ user.title }}</div>
          </div>
          <button title="Sign out" class="rounded-lg p-1.5 text-slate-500 hover:bg-white/5 hover:text-slate-200" @click="logout()">
            <ArrowLeftOnRectangleIcon class="h-5 w-5"></ArrowLeftOnRectangleIcon>
          </button>
        </div>
      </div>
    </aside>

    <main class="ml-64 flex-1">
      <slot></slot>
    </main>
  </div>
</template>
