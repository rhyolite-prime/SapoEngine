<script setup lang="ts">
import {
  BuildingStorefrontIcon, UsersIcon, SignalIcon, LightBulbIcon, CheckCircleIcon,
  KeyIcon, ShieldCheckIcon, ArrowPathIcon, ArrowLeftOnRectangleIcon,
} from '@heroicons/vue/24/outline'
import type { User } from '~/../shared/types'

definePageMeta({ layout: 'default' })
useHead({ title: 'Settings · ShortCodeExpress' })

const { logout } = useAuth()
const me = ref<User | null>(null)
await (async () => { me.value = await useRequestFetch()<User>('/api/auth/me') })()

// --- use case (same source of truth as onboarding step 1) --------------------
const useCase = ref<'merchant' | 'aggregator' | null>(me.value?.useCase ?? null)
const savingUseCase = ref(false)
const useCaseError = ref('')

async function chooseUseCase(kind: 'merchant' | 'aggregator') {
  useCaseError.value = ''
  savingUseCase.value = true
  try {
    const r = await $fetch<{ useCase: 'merchant' | 'aggregator' }>('/api/onboarding/use-case', { method: 'POST', body: { useCase: kind } })
    useCase.value = r.useCase
    if (me.value) me.value.useCase = r.useCase
  } catch (e: unknown) {
    useCaseError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not save — try again'
  } finally { savingUseCase.value = false }
}

// --- default network (prefills the purchase wizard) ---------------------------
const NETWORKS: Array<{ value: string; label: string }> = [
  { value: 'all', label: 'All networks' },
  { value: 'MTN', label: 'MTN' },
  { value: 'Vodafone', label: 'Vodafone' },
  { value: 'AirtelTigo', label: 'AirtelTigo' },
]
const networkLabel = (v: string) => NETWORKS.find(n => n.value === v)?.label ?? v
const defaultNetwork = ref(me.value?.settings?.defaultNetwork ?? 'all')
const savingNetwork = ref(false)
const networkMsg = ref('')

async function saveNetwork(n: string) {
  defaultNetwork.value = n
  savingNetwork.value = true
  networkMsg.value = ''
  try {
    await $fetch('/api/account/settings', { method: 'PATCH', body: { defaultNetwork: n } })
    networkMsg.value = `New purchases will default to ${networkLabel(n)}`
    setTimeout(() => (networkMsg.value = ''), 2500)
  } finally { savingNetwork.value = false }
}

// --- builder tips (client preference, mirrors the canvas banner) ---------------
const showTips = ref(true)
onMounted(() => { showTips.value = localStorage.getItem('sce.builder.tips') !== 'off' })
function toggleTips() {
  showTips.value = !showTips.value
  localStorage.setItem('sce.builder.tips', showTips.value ? 'on' : 'off')
}
</script>

<template>
  <div class="mx-auto max-w-3xl p-8">
    <div class="mb-8">
      <h1 class="text-2xl font-bold text-slate-900">Settings</h1>
      <p class="mt-1 text-sm text-slate-500">Workspace defaults and preferences — they follow you across the app.</p>
    </div>

    <div class="space-y-5">
      <!-- use case -->
      <section class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
        <h2 class="text-sm font-bold text-slate-900">How you use ShortCodeExpress</h2>
        <p class="mt-0.5 text-xs text-slate-500">Shapes the wording across the app and how new short codes are framed.</p>
        <div class="mt-4 grid gap-3 sm:grid-cols-2">
          <button :disabled="savingUseCase"
            class="rounded-xl border-2 p-4 text-left transition disabled:opacity-50"
            :class="useCase === 'merchant' ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 hover:border-brand-300'"
            @click="chooseUseCase('merchant')">
            <div class="flex items-center gap-2.5">
              <span class="flex h-8 w-8 items-center justify-center rounded-lg bg-brand-100 text-brand-700"><BuildingStorefrontIcon class="h-4 w-4" /></span>
              <span class="text-sm font-bold text-slate-900">Business merchant</span>
              <CheckCircleIcon v-if="useCase === 'merchant'" class="ml-auto h-5 w-5 text-success-500" />
            </div>
            <p class="mt-2 text-xs text-slate-500">USSD for my own services under my own short codes.</p>
          </button>
          <button :disabled="savingUseCase"
            class="rounded-xl border-2 p-4 text-left transition disabled:opacity-50"
            :class="useCase === 'aggregator' ? 'border-brand-500 bg-brand-50/60' : 'border-slate-200 hover:border-brand-300'"
            @click="chooseUseCase('aggregator')">
            <div class="flex items-center gap-2.5">
              <span class="flex h-8 w-8 items-center justify-center rounded-lg bg-brand-100 text-brand-700"><UsersIcon class="h-4 w-4" /></span>
              <span class="text-sm font-bold text-slate-900">Aggregator</span>
              <CheckCircleIcon v-if="useCase === 'aggregator'" class="ml-auto h-5 w-5 text-success-500" />
            </div>
            <p class="mt-2 text-xs text-slate-500">Build and manage USSD services for client businesses.</p>
          </button>
        </div>
        <p v-if="useCaseError" class="mt-3 rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700">{{ useCaseError }}</p>
      </section>

      <!-- default network -->
      <section class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
        <h2 class="flex items-center gap-2 text-sm font-bold text-slate-900"><SignalIcon class="h-4 w-4 text-brand-600" /> Default network</h2>
        <p class="mt-0.5 text-xs text-slate-500">Pre-selected whenever you buy or port a short code.</p>
        <div class="mt-3 flex flex-wrap gap-2">
          <button v-for="n in NETWORKS" :key="n.value" :disabled="savingNetwork"
            class="rounded-xl border-2 px-4 py-2.5 text-sm font-bold transition disabled:opacity-50"
            :class="defaultNetwork === n.value ? 'border-brand-500 bg-brand-50/60 text-brand-700' : 'border-slate-200 text-slate-600 hover:border-brand-300'"
            @click="saveNetwork(n)">{{ n.label }}</button>
        </div>
        <p v-if="networkMsg" class="mt-2.5 flex items-center gap-1.5 text-xs font-semibold text-success-600"><CheckCircleIcon class="h-4 w-4" /> {{ networkMsg }}</p>
      </section>

      <!-- builder preferences -->
      <section class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
        <h2 class="flex items-center gap-2 text-sm font-bold text-slate-900"><LightBulbIcon class="h-4 w-4 text-brand-600" /> Builder</h2>
        <div class="mt-3 flex items-center justify-between gap-4 rounded-xl bg-slate-50 px-4 py-3 ring-1 ring-slate-100">
          <div>
            <div class="text-sm font-semibold text-slate-800">Show quick tips on the canvas</div>
            <div class="text-xs text-slate-500">The dismissible guidance banner above the builder.</div>
          </div>
          <button type="button" role="switch" :aria-checked="showTips"
            class="relative h-6 w-11 shrink-0 rounded-full transition"
            :class="showTips ? 'bg-brand-600' : 'bg-slate-300'"
            @click="toggleTips">
            <span class="absolute top-0.5 h-5 w-5 rounded-full bg-white shadow transition-all" :class="showTips ? 'left-[22px]' : 'left-0.5'"></span>
          </button>
        </div>
      </section>

      <!-- security shortcuts -->
      <section class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
        <h2 class="flex items-center gap-2 text-sm font-bold text-slate-900"><ShieldCheckIcon class="h-4 w-4 text-brand-600" /> Security &amp; access</h2>
        <div class="mt-3 grid gap-2.5 sm:grid-cols-2">
          <NuxtLink to="/profile" class="flex items-center gap-2.5 rounded-xl bg-slate-50 px-4 py-3 ring-1 ring-slate-100 transition hover:ring-brand-300">
            <KeyIcon class="h-4 w-4 text-brand-600" />
            <span class="text-sm font-semibold text-slate-800">Reset your password</span>
          </NuxtLink>
          <NuxtLink to="/developers" class="flex items-center gap-2.5 rounded-xl bg-slate-50 px-4 py-3 ring-1 ring-slate-100 transition hover:ring-brand-300">
            <ArrowPathIcon class="h-4 w-4 text-brand-600" />
            <span class="text-sm font-semibold text-slate-800">Rotate API keys &amp; webhooks</span>
          </NuxtLink>
        </div>
        <button
          class="mt-4 flex w-full items-center justify-center gap-2 rounded-xl bg-white px-4 py-2.5 text-sm font-bold text-rose-600 ring-1 ring-rose-200 transition hover:bg-rose-50"
          @click="logout()">
          <ArrowLeftOnRectangleIcon class="h-4 w-4" /> Sign out of this device
        </button>
      </section>
    </div>
  </div>
</template>
