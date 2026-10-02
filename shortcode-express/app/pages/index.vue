<script setup lang="ts">
import { ArrowRightIcon, BoltIcon, CubeTransparentIcon, ClockIcon, BanknotesIcon, UsersIcon } from '@heroicons/vue/24/outline'

definePageMeta({ layout: 'public' })
useHead({ title: 'ShortCodeExpress — Build USSD services visually' })

// signed-in visitors go straight to their workspace
const authed = computed(() => !!useCookie('sce_user').value)
</script>

<template>
  <div class="min-h-screen bg-ink-950 text-slate-200">
    <header class="mx-auto flex max-w-6xl items-center justify-between px-6 py-5">
      <div class="flex items-center gap-3">
        <div class="flex h-9 w-9 items-center justify-center rounded-xl bg-gradient-to-br from-brand-600 to-brand-400">
          <svg viewBox="0 0 24 24" class="h-5 w-5 text-white" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 7h16M4 12h10M4 17h7" /><circle cx="18.5" cy="15.5" r="2.5" /></svg>
        </div>
        <span class="text-lg font-bold text-white">ShortCode<span class="text-brand-400">Express</span></span>
      </div>
      <template v-if="authed">
        <NuxtLink to="/dashboard" class="inline-flex items-center gap-1.5 rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500">
          Open dashboard <ArrowRightIcon class="h-4 w-4" />
        </NuxtLink>
      </template>
      <template v-else>
        <NuxtLink to="/signup" class="rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500">
          Start free — live in 5 min
        </NuxtLink>
        <NuxtLink to="/login" class="rounded-lg px-4 py-2 text-sm font-semibold text-slate-300 hover:text-white">
          Sign in
        </NuxtLink>
      </template>
    </header>

    <section class="relative mx-auto max-w-6xl px-6 pb-20 pt-16 text-center">
      <div class="pointer-events-none absolute inset-x-0 -top-24 mx-auto h-72 w-[42rem] rounded-full bg-brand-600/25 blur-3xl" />
      <div class="relative">
        <div class="mx-auto mb-6 inline-flex items-center gap-2 rounded-full border border-brand-500/30 bg-brand-500/10 px-4 py-1.5 text-xs font-medium text-brand-300">
          <BoltIcon class="h-3.5 w-3.5" /> Visual builder for the Sapo DSL Engine
        </div>
        <h1 class="mx-auto max-w-3xl text-5xl font-extrabold leading-tight tracking-tight text-white">
          Compose USSD services by
          <span class="bg-gradient-to-r from-brand-400 to-brand-300 bg-clip-text text-transparent">drag &amp; drop</span>.
          Ship what the VM runs.
        </h1>
        <p class="mx-auto mt-6 max-w-2xl text-lg text-slate-400">
          Sign up, buy a short code with mobile money, and dial it live in five minutes — then design menus, plug in APIs and collaborate,
          while ShortCodeExpress compiles your canvas into the exact Sapo DSL workflow blueprint that executes on the Sapo virtual machine.
        </p>
        <div class="mt-8 flex items-center justify-center gap-3">
          <NuxtLink to="/signup" class="inline-flex items-center gap-2 rounded-xl bg-brand-600 px-6 py-3 text-sm font-semibold text-white shadow-lg shadow-brand-900/50 hover:bg-brand-500">
            Get a short code <ArrowRightIcon class="h-4 w-4" />
          </NuxtLink>
          <a href="#how" class="rounded-xl border border-white/10 px-6 py-3 text-sm font-semibold text-slate-300 hover:border-white/25 hover:text-white">How it works</a>
        </div>
      </div>

      <div class="relative mx-auto mt-16 max-w-4xl">
        <div class="rounded-2xl border border-white/10 bg-ink-900/80 p-2 shadow-2xl">
          <div class="rounded-xl bg-ink-950 p-5 text-left font-mono text-[12.5px] leading-relaxed">
            <div class="mb-3 flex items-center gap-1.5">
              <span class="h-3 w-3 rounded-full bg-rose-500/70"></span><span class="h-3 w-3 rounded-full bg-amber-500/70"></span><span class="h-3 w-3 rounded-full bg-success-500/70"></span>
              <span class="ml-3 text-slate-500">daccu_ussd_service.json — generated from the canvas</span>
            </div>
            <pre class="overflow-x-auto text-slate-300"><code>{
  <span class="text-sky-400">"name"</span>: <span class="text-success-400">"examples.daccu_ussd_service"</span>,
  <span class="text-sky-400">"nodes"</span>: [
    {
      <span class="text-sky-400">"id"</span>: <span class="text-success-400">"main_menu"</span>,
      <span class="text-sky-400">"type"</span>: <span class="text-success-400">"action"</span>,
      <span class="text-sky-400">"prompt_config"</span>: {
        <span class="text-sky-400">"message"</span>: <span class="text-success-400">"Welcome to Sapo Bank\n1. Deposit\n2. Withdrawal\n3. Funds transfer…"</span>,
        <span class="text-sky-400">"interaction_type"</span>: <span class="text-success-400">"menu"</span>,
        <span class="text-sky-400">"timeout"</span>: <span class="text-success-400">"2m"</span>
      },
      <span class="text-sky-400">"next"</span>: <span class="text-success-400">"route_main_menu"</span>
    },
    {
      <span class="text-sky-400">"id"</span>: <span class="text-success-400">"call_deposit_api"</span>,
      <span class="text-sky-400">"type"</span>: <span class="text-success-400">"command"</span>,
      <span class="text-sky-400">"command"</span>: <span class="text-success-400">"http.post"</span>,
      <span class="text-sky-400">"http_request"</span>: { <span class="text-sky-400">"url"</span>: <span class="text-success-400">"https://api.daccu.com/v1/deposits"</span> }
    }
  ]
}</code></pre>
          </div>
        </div>
      </div>
    </section>

    <section id="how" class="border-t border-white/5 bg-ink-900/50 py-20">
      <div class="mx-auto grid max-w-6xl gap-8 px-6 sm:grid-cols-2 lg:grid-cols-4">
        <div v-for="f in [
          { icon: CubeTransparentIcon, title: '1:1 blueprint fidelity', text: 'Menus, prompts, HTTP commands, retries, try/catch, loops — every node maps to the Sapo DSL grammar. Export and run it with sapoc.' },
          { icon: ClockIcon, title: 'Build & release like Azure', text: 'Every save builds and validates a versioned artifact. Promote, release, and roll back in one click with a full audit trail.' },
          { icon: BanknotesIcon, title: 'Quotas & billing', text: 'Session quotas per business or short code, overage rates, invoices and revenue analytics out of the box.' },
          { icon: UsersIcon, title: 'Collaborate by email', text: 'Invite engineers and analysts by email with owner, editor or viewer roles.' },
        ]" :key="f.title" class="rounded-2xl border border-white/5 bg-ink-950/60 p-6">
          <div class="mb-4 flex h-10 w-10 items-center justify-center rounded-xl bg-brand-500/15 text-brand-400">
            <component :is="f.icon" class="h-5 w-5" />
          </div>
          <h3 class="mb-2 font-semibold text-white">{{ f.title }}</h3>
          <p class="text-sm leading-relaxed text-slate-400">{{ f.text }}</p>
        </div>
      </div>
    </section>

    <footer class="border-t border-white/5 py-8 text-center text-xs text-slate-600">
      ShortCodeExpress · a frontend for the Sapo DSL Engine · Ghana 🇬🇭
    </footer>
  </div>
</template>
