<script setup lang="ts">
import {
  ArrowLeftIcon, BoltSlashIcon, DevicePhoneMobileIcon, ShieldCheckIcon, ArrowsRightLeftIcon, SignalIcon,
} from '@heroicons/vue/24/outline'

// The standalone "buy a short code" flow — for users who have already been
// through onboarding. No checklist, no account step: pick a code (system,
// your own, or port from Hubtel / Nalo / Africa's Talking), optionally add a
// session pack, and pay.
definePageMeta({ middleware: 'auth' })
useHead({ title: 'Get a short code · ShortCodeExpress' })

const me = await useRequestFetch()<{ id: string; useCase?: 'merchant' | 'aggregator' | null } | null>('/api/auth/me').catch(() => null)
const isAggregator = computed(() => me?.useCase === 'aggregator')
const useCaseKnown = computed(() => !!me?.useCase)
</script>

<template>
  <div class="mx-auto max-w-4xl p-8">
    <!-- header -->
    <div class="mb-6 flex flex-wrap items-end justify-between gap-4">
      <div>
        <NuxtLink to="/shortcodes" class="mb-2 inline-flex items-center gap-1.5 text-xs font-semibold text-slate-400 hover:text-slate-600">
          <ArrowLeftIcon class="h-3.5 w-3.5" /> Short codes
        </NuxtLink>
        <h1 class="text-2xl font-bold text-slate-900">{{ isAggregator ? "Add a client's short code" : 'Get a short code' }}</h1>
        <p class="mt-1 text-sm text-slate-500">
          {{ isAggregator
            ? 'Each purchase provisions one client code with its own flow, quota and webhooks.'
            : 'Your own USSD code — provisioned instantly, porting supported, live in minutes.' }}
        </p>
      </div>
      <div class="flex items-center gap-1.5 rounded-full bg-brand-50 px-3 py-1.5 text-[11px] font-bold text-brand-700 ring-1 ring-brand-100">
        <ShieldCheckIcon class="h-4 w-4" /> Secure webcheckout · instant provisioning
      </div>
    </div>

    <!-- gentle nudge if the use case was never picked -->
    <div v-if="!useCaseKnown" class="mb-5 flex flex-wrap items-center gap-2 rounded-xl bg-amber-50 px-4 py-3 text-xs text-amber-800 ring-1 ring-amber-100">
      <BoltSlashIcon class="h-4 w-4 shrink-0" />
      <span>Heads up — you haven't told us how you'll use ShortCodeExpress yet.</span>
      <NuxtLink to="/onboarding" class="font-bold text-amber-900 underline decoration-amber-300 hover:text-amber-700">Pick merchant or aggregator</NuxtLink>
      <span class="text-amber-600/70">(takes one click — you can keep buying either way)</span>
    </div>

    <!-- the wizard -->
    <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
      <div class="mb-4 flex items-center gap-2 text-[11px] font-bold uppercase tracking-widest text-slate-400">
        <SignalIcon class="h-4 w-4 text-brand-600" /> Step 1 — choose how you get your code
      </div>
      <ShortcodesPurchaseWizard :aggregator="isAggregator" source="buy" />
    </div>

    <!-- what happens after payment -->
    <div class="mt-4 grid gap-3 sm:grid-cols-2">
      <div class="rounded-xl bg-white p-4 ring-1 ring-slate-200">
        <div class="flex items-center gap-2 text-xs font-bold text-slate-700"><DevicePhoneMobileIcon class="h-4 w-4 text-success-600" /> New &amp; custom codes</div>
        <p class="mt-1 text-[11.5px] leading-relaxed text-slate-500">
          Live the moment payment clears — starter flow built and released, sessions active, dialable immediately.
        </p>
      </div>
      <div class="rounded-xl bg-white p-4 ring-1 ring-slate-200">
        <div class="flex items-center gap-2 text-xs font-bold text-slate-700"><ArrowsRightLeftIcon class="h-4 w-4 text-brand-600" /> Ported codes</div>
        <p class="mt-1 text-[11.5px] leading-relaxed text-slate-500">
          We issue a private link to send your current provider — the code goes live the instant they approve it. Flat rate, unlimited sessions.
        </p>
      </div>
    </div>
  </div>
</template>
