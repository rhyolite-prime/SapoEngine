<script setup lang="ts">
import {
  ArrowRightIcon, ArrowDownIcon, ArrowUturnLeftIcon, ArrowsRightLeftIcon,
  BoltIcon, BriefcaseIcon, BuildingLibraryIcon, CheckCircleIcon, CodeBracketIcon,
  CpuChipIcon, CursorArrowRaysIcon, DevicePhoneMobileIcon, PuzzlePieceIcon,
  RocketLaunchIcon, ServerStackIcon, SignalIcon, UserGroupIcon,
} from '@heroicons/vue/24/outline'

definePageMeta({ layout: 'public' })
useHead({ title: 'ShortCodeExpress — Build, deploy, and scale USSD apps in minutes' })

// signed-in visitors go straight to their workspace
const authed = computed(() => !!useCookie('sce_user').value)

// core capabilities, in priority order (landing page spec)
const capabilities = [
  {
    icon: CursorArrowRaysIcon, span: true, num: '01',
    title: 'Visual Drag-and-Drop Builder & Free Hosting',
    text: 'Compose and deploy full-featured USSD applications visually using intuitive drag-and-drop tools. Enjoy free, zero-setup hosting so you can launch from concept to live production in minutes without managing servers.',
  },
  {
    icon: CpuChipIcon, num: '02',
    title: 'Predictable Engine: Powered by SAPO DSL',
    text: 'Every USSD application is automatically translated into SAPO DSL and executed on a dedicated virtual machine. This architecture ensures standardized output, high execution speed, and completely predictable, deterministic behavior across all telecom networks.',
  },
  {
    icon: ArrowsRightLeftIcon, span: true, num: '03',
    title: 'Instant Upstream Integration & Transformation Cogs',
    text: 'Seamlessly connect to your existing upstream APIs and databases. Query, retrieve, and transform live backend data into any desired USSD screen format using built-in visual transformation cogs.',
    pipeline: true,
  },
  {
    icon: ArrowUturnLeftIcon, num: '04',
    title: 'Safe Release Management & Instant Rollbacks',
    text: 'Deploy updates with confidence using complete version control. If an issue occurs in production, restore stability immediately with 1-click version rollbacks.',
  },
  {
    icon: SignalIcon, num: '05',
    title: 'Flexible Short Code Onboarding',
    text: '',
    options: [
      { label: 'Bring your own code', text: 'Easily port existing USSD short codes over from your current telecom providers.' },
      { label: 'Start fresh', text: 'Purchase and provision brand-new short code extensions directly within the platform.' },
    ],
  },
  {
    icon: PuzzlePieceIcon, num: '06', anchor: 'plugins',
    title: 'Plug-and-Play Plugin Marketplace',
    text: 'Discover a rich repository of pre-built integrations from major network and service providers. Instantly add payment gateways, messaging pipelines, and third-party tools with zero custom code.',
  },
  {
    icon: UserGroupIcon, num: '07',
    title: 'Real-Time Team Collaboration',
    text: 'Invite developers, product managers, and external partners to co-create, test logic, and manage releases together within a shared project workspace.',
  },
]

// benefits by audience (spec table)
const audiences = [
  {
    icon: CodeBracketIcon, title: 'Developers & Engineers',
    points: [
      'Standardized output via SAPO DSL Virtual Machine.',
      'Built-in visual transformation cogs eliminate custom middleware.',
      'Safe release versioning with 1-click instant rollbacks.',
      'Plug-and-play SDKs and plugins from major providers.',
    ],
  },
  {
    icon: BriefcaseIcon, title: 'Business Owners',
    points: [
      'Fast time-to-market with code-free visual creation.',
      'Zero initial infrastructure costs with included free hosting.',
      'Flexible short code options (port existing codes or buy new extensions).',
      'Reduced operational costs and overhead.',
    ],
  },
  {
    icon: BuildingLibraryIcon, title: 'Financial Institutions',
    points: [
      'Highly predictable and deterministic transaction execution.',
      'Enterprise-grade API and upstream database connectivity.',
      'Fine-grained multi-user collaboration and permission controls.',
      'Safe release controls to safeguard live financial channels.',
    ],
  },
]

// data flow architecture (spec diagram)
const flow = [
  { icon: ServerStackIcon, title: 'Upstream APIs', note: 'Your backend services & databases' },
  { icon: ArrowsRightLeftIcon, title: 'Transformation Cogs', note: 'Data Mapping & Formatting Engine' },
  { icon: CpuChipIcon, title: 'SAPO DSL Engine', note: 'Executes on Dedicated Virtual Machine' },
  { icon: DevicePhoneMobileIcon, title: 'USSD Screen Output', note: 'Deterministic screens on every network' },
]

// upstream → cogs → screen pipeline inside capability 03
const pipeline = [
  { title: 'Upstream Services', note: 'APIs / Databases' },
  { title: 'Transformation Cogs', note: 'Data Mapping & Logic' },
  { title: 'Formatted USSD App', note: 'Output Screen' },
]
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
          Start free
        </NuxtLink>
        <NuxtLink to="/login" class="rounded-lg px-4 py-2 text-sm font-semibold text-slate-300 hover:text-white">
          Sign in
        </NuxtLink>
      </template>
    </header>

    <!-- ============ 1 · hero ============ -->
    <section class="relative mx-auto max-w-6xl px-6 pb-20 pt-16 text-center">
      <div class="pointer-events-none absolute inset-x-0 -top-24 mx-auto h-72 w-[42rem] rounded-full bg-brand-600/25 blur-3xl" />
      <div class="relative">
        <div class="mx-auto mb-6 inline-flex items-center gap-2 rounded-full border border-brand-500/30 bg-brand-500/10 px-4 py-1.5 text-xs font-medium text-brand-300">
          <BoltIcon class="h-3.5 w-3.5" /> Visual builder for the SAPO DSL Engine
        </div>
        <h1 class="mx-auto max-w-4xl text-5xl font-extrabold leading-tight tracking-tight text-white">
          Build, Deploy, and Scale
          <span class="bg-gradient-to-r from-brand-400 to-brand-300 bg-clip-text text-transparent">USSD Apps</span>
          in Minutes
        </h1>
        <p class="mx-auto mt-6 max-w-2xl text-lg text-slate-400">
          Connect your backend services, build visual workflows, and run on a deterministic SAPO DSL virtual machine.
          ShortCodeExpress powers seamless USSD integration with zero hosting hassle.
        </p>
        <div class="mt-8 flex flex-wrap items-center justify-center gap-3">
          <NuxtLink :to="authed ? '/dashboard' : '/signup'" class="inline-flex items-center gap-2 rounded-xl bg-brand-600 px-6 py-3 text-sm font-semibold text-white shadow-lg shadow-brand-900/50 hover:bg-brand-500">
            {{ authed ? 'Open dashboard' : 'Start Building Free' }} <ArrowRightIcon class="h-4 w-4" />
          </NuxtLink>
          <a href="#plugins" class="inline-flex items-center gap-2 rounded-xl border border-white/10 px-6 py-3 text-sm font-semibold text-slate-300 hover:border-white/25 hover:text-white">
            <PuzzlePieceIcon class="h-4 w-4 text-brand-400" /> Explore Plugin Marketplace
          </a>
        </div>
        <p v-if="!authed" class="mt-4 text-xs text-slate-500">Free hosting included · No servers to manage · Live in minutes</p>
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

    <!-- ============ 2 · core capabilities ============ -->
    <section id="capabilities" class="border-t border-white/5 bg-ink-900/50 py-20">
      <div class="mx-auto max-w-6xl px-6">
        <div class="mx-auto max-w-2xl text-center">
          <div class="text-xs font-bold uppercase tracking-[0.2em] text-brand-400">Core capabilities</div>
          <h2 class="mt-3 text-3xl font-extrabold tracking-tight text-white">Everything you need to ship USSD</h2>
          <p class="mt-3 text-sm text-slate-400">One platform — from the first drag on the canvas to deterministic execution on live short codes.</p>
        </div>

        <div class="mt-12 grid gap-5 lg:grid-cols-3">
          <article
            v-for="c in capabilities" :id="c.anchor" :key="c.num"
            class="group relative flex flex-col rounded-2xl border border-white/5 bg-ink-950/60 p-6 transition hover:border-brand-500/30"
            :class="c.span ? 'lg:col-span-2' : ''"
          >
            <div class="mb-4 flex items-center justify-between">
              <div class="flex h-10 w-10 items-center justify-center rounded-xl bg-brand-500/15 text-brand-400">
                <component :is="c.icon" class="h-5 w-5" />
              </div>
              <span class="font-mono text-xs font-semibold text-slate-600">{{ c.num }}</span>
            </div>
            <h3 class="mb-2 font-semibold text-white">{{ c.title }}</h3>
            <p v-if="c.text" class="text-sm leading-relaxed text-slate-400">{{ c.text }}</p>

            <!-- 03 · upstream → cogs → screen pipeline -->
            <div v-if="c.pipeline" class="mt-5 grid items-stretch gap-2 sm:grid-cols-3">
              <div v-for="(p, i) in pipeline" :key="p.title" class="relative flex items-center gap-2">
                <div class="flex-1 rounded-xl border border-white/10 bg-ink-900/70 px-3.5 py-3 text-center">
                  <div class="text-xs font-bold text-white">{{ p.title }}</div>
                  <div class="mt-0.5 text-[11px] text-slate-500">{{ p.note }}</div>
                </div>
                <ArrowRightIcon v-if="i < pipeline.length - 1" class="hidden h-4 w-4 shrink-0 text-brand-400 sm:block" />
                <ArrowDownIcon v-if="i < pipeline.length - 1" class="mx-auto h-4 w-4 shrink-0 text-brand-400 sm:hidden" />
              </div>
            </div>

            <!-- 05 · port or buy -->
            <ul v-if="c.options" class="mt-1 space-y-3">
              <li v-for="o in c.options" :key="o.label" class="rounded-xl border border-white/10 bg-ink-900/70 px-4 py-3">
                <div class="text-sm font-bold text-brand-300">{{ o.label }}</div>
                <div class="mt-0.5 text-xs leading-relaxed text-slate-400">{{ o.text }}</div>
              </li>
            </ul>
          </article>
        </div>
      </div>
    </section>

    <!-- ============ 4 · data flow architecture ============ -->
    <section id="architecture" class="border-t border-white/5 py-20">
      <div class="mx-auto max-w-6xl px-6">
        <div class="mx-auto max-w-2xl text-center">
          <div class="text-xs font-bold uppercase tracking-[0.2em] text-brand-400">Under the hood</div>
          <h2 class="mt-3 text-3xl font-extrabold tracking-tight text-white">Data Flow Architecture</h2>
          <p class="mt-3 text-sm text-slate-400">How live backend data becomes a deterministic USSD screen.</p>
        </div>

        <div class="mx-auto mt-12 max-w-lg rounded-2xl border border-white/10 bg-ink-900/40 p-6 sm:p-8">
          <template v-for="(step, i) in flow" :key="step.title">
            <div class="flex items-center gap-4 rounded-xl border border-white/10 bg-ink-950/70 px-5 py-4">
              <div class="flex h-11 w-11 shrink-0 items-center justify-center rounded-xl bg-gradient-to-br from-brand-600/40 to-brand-400/20 text-brand-300">
                <component :is="step.icon" class="h-5 w-5" />
              </div>
              <div>
                <div class="text-sm font-bold text-white">{{ step.title }}</div>
                <div class="text-xs text-slate-500">{{ step.note }}</div>
              </div>
            </div>
            <div v-if="i < flow.length - 1" class="flex flex-col items-center py-1.5">
              <ArrowDownIcon class="h-5 w-5 text-brand-400" />
            </div>
          </template>
        </div>
      </div>
    </section>

    <!-- ============ 3 · benefits by audience ============ -->
    <section id="audiences" class="border-t border-white/5 bg-ink-900/50 py-20">
      <div class="mx-auto max-w-6xl px-6">
        <div class="mx-auto max-w-2xl text-center">
          <div class="text-xs font-bold uppercase tracking-[0.2em] text-brand-400">Benefits by audience</div>
          <h2 class="mt-3 text-3xl font-extrabold tracking-tight text-white">Built for everyone at the table</h2>
        </div>

        <div class="mt-12 grid gap-5 lg:grid-cols-3">
          <div v-for="a in audiences" :key="a.title" class="rounded-2xl border border-white/5 bg-ink-950/60 p-6">
            <div class="mb-5 flex items-center gap-3">
              <div class="flex h-10 w-10 items-center justify-center rounded-xl bg-brand-500/15 text-brand-400">
                <component :is="a.icon" class="h-5 w-5" />
              </div>
              <h3 class="font-semibold text-white">{{ a.title }}</h3>
            </div>
            <ul class="space-y-3">
              <li v-for="p in a.points" :key="p" class="flex items-start gap-2.5 text-sm leading-relaxed text-slate-400">
                <CheckCircleIcon class="mt-0.5 h-4 w-4 shrink-0 text-success-500" />
                <span>{{ p }}</span>
              </li>
            </ul>
          </div>
        </div>
      </div>
    </section>

    <!-- ============ final CTA ============ -->
    <section class="border-t border-white/5 py-20">
      <div class="mx-auto max-w-4xl px-6">
        <div class="relative overflow-hidden rounded-3xl bg-gradient-to-br from-brand-700 via-brand-600 to-brand-400 px-8 py-14 text-center shadow-2xl shadow-brand-900/40">
          <div class="pointer-events-none absolute -top-24 left-1/2 h-56 w-[36rem] -translate-x-1/2 rounded-full bg-white/15 blur-3xl" />
          <div class="relative">
            <RocketLaunchIcon class="mx-auto mb-4 h-8 w-8 text-white/90" />
            <h2 class="text-3xl font-extrabold tracking-tight text-white">Your first USSD app is minutes away</h2>
            <p class="mx-auto mt-3 max-w-xl text-sm text-white/80">
              Drag, drop, and deploy — with free hosting, deterministic execution, and 1-click rollbacks from day one.
            </p>
            <p v-if="!authed" class="mt-3 text-xs text-white/70">Free hosting included · No credit card to start</p>
            <NuxtLink :to="authed ? '/dashboard' : '/signup'" class="mt-8 inline-flex items-center gap-2 rounded-xl bg-white px-6 py-3 text-sm font-bold text-brand-700 shadow-lg hover:bg-white/90">
              {{ authed ? 'Open dashboard' : 'Start Building Free' }} <ArrowRightIcon class="h-4 w-4" />
            </NuxtLink>
          </div>
        </div>
      </div>
    </section>

    <footer class="border-t border-white/5 py-8 text-center text-xs text-slate-600">
      ShortCodeExpress · the visual platform for the SAPO DSL Engine · Ghana 🇬🇭
    </footer>
  </div>
</template>
