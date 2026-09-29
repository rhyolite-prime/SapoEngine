<script setup lang="ts">
import { RocketLaunchIcon, KeyIcon, SignalIcon, BoltIcon, CheckBadgeIcon } from '@heroicons/vue/24/outline'
import type { User } from '~/../shared/types'

definePageMeta({ layout: 'default' })
useHead({ title: 'Create your developer account · ShortCodeExpress' })

const form = reactive({ name: '', email: '', company: '' })
const busy = ref(false)
const error = ref('')

const perks = [
  { icon: KeyIcon, title: 'API keys in seconds', text: 'A live sk_ key is generated with your account — dial, integrate, automate.' },
  { icon: SignalIcon, title: 'Your own short code', text: 'Pick a code or let us choose. Checkout with MoMo or card, live instantly.' },
  { icon: BoltIcon, title: 'Live in 5 minutes', text: 'A starter flow with dynamic menus and a payment webhook ships with every code.' },
]

async function submit() {
  error.value = ''
  busy.value = true
  try {
    await $fetch<User>('/api/auth/signup', { method: 'POST', body: { ...form } })
    navigateTo('/onboarding')
  } catch (e: unknown) {
    error.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not create your account'
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="flex min-h-screen items-center justify-center bg-gradient-to-br from-ink-950 via-ink-900 to-brand-900/60 px-4 py-10">
    <div class="grid w-full max-w-5xl gap-10 lg:grid-cols-[1fr_420px] lg:items-center">
      <div class="hidden lg:block">
        <div class="mb-6 inline-flex items-center gap-2 rounded-full bg-white/5 px-4 py-1.5 text-xs font-semibold text-brand-300 ring-1 ring-white/10">
          <RocketLaunchIcon class="h-4 w-4" /> Instant USSD provisioning
        </div>
        <h1 class="text-4xl font-extrabold leading-tight text-white">
          Go from signup to a<br>
          <span class="bg-gradient-to-r from-brand-400 to-brand-300 bg-clip-text text-transparent">dialable short code</span> in 5 minutes.
        </h1>
        <p class="mt-4 max-w-md text-slate-400">
          ShortCodeExpress provisions real USSD short codes with webcheckout, session packs, API keys and signed payment webhooks — all on the Sapo Engine.
        </p>
        <div class="mt-8 space-y-4">
          <div v-for="p in perks" :key="p.title" class="flex gap-3">
            <div class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl bg-brand-600/20 text-brand-300 ring-1 ring-brand-500/30">
              <component :is="p.icon" class="h-5 w-5" />
            </div>
            <div>
              <div class="text-sm font-semibold text-slate-100">{{ p.title }}</div>
              <div class="text-sm text-slate-400">{{ p.text }}</div>
            </div>
          </div>
        </div>
      </div>

      <div class="rounded-2xl bg-white p-7 shadow-2xl">
        <div class="mb-6 flex items-center gap-3">
          <div class="flex h-11 w-11 items-center justify-center rounded-xl bg-gradient-to-br from-brand-600 to-brand-400">
            <svg viewBox="0 0 24 24" class="h-6 w-6 text-white" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
              <path d="M4 7h16M4 12h10M4 17h7" /><circle cx="18.5" cy="15.5" r="2.5" />
            </svg>
          </div>
          <div>
            <h2 class="text-lg font-bold text-slate-900">Create your account</h2>
            <p class="text-xs text-slate-500">Free to start — pay only when you provision a code.</p>
          </div>
        </div>

        <form class="space-y-4" @submit.prevent="submit">
          <label class="block">
            <span class="mb-1 block text-xs font-semibold text-slate-600">Your name</span>
            <input v-model="form.name" required placeholder="Kofi Mensah" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
          </label>
          <label class="block">
            <span class="mb-1 block text-xs font-semibold text-slate-600">Work email</span>
            <input v-model="form.email" required type="email" placeholder="kofi@myfintech.gh" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
          </label>
          <label class="block">
            <span class="mb-1 block text-xs font-semibold text-slate-600">Company <span class="font-normal text-slate-400">(optional)</span></span>
            <input v-model="form.company" placeholder="MyFinTech Ltd" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
          </label>

          <p v-if="error" class="rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700 ring-1 ring-rose-100">{{ error }}</p>

          <button type="submit" :disabled="busy || !form.name || !form.email"
            class="flex w-full items-center justify-center gap-2 rounded-xl bg-brand-600 px-4 py-3 text-sm font-semibold text-white shadow-lg shadow-brand-200 transition hover:bg-brand-500 disabled:opacity-50">
            <svg v-if="busy" class="h-4 w-4 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
            <CheckBadgeIcon v-else class="h-4 w-4" />
            {{ busy ? 'Creating workspace…' : 'Create account & get API keys' }}
          </button>
          <p class="text-center text-xs text-slate-500">
            Already have an account?
            <NuxtLink to="/login" class="font-semibold text-brand-600 hover:underline">Sign in</NuxtLink>
          </p>
        </form>
      </div>
    </div>
  </div>
</template>
