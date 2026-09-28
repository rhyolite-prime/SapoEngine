<script setup lang="ts">
import { WrenchScrewdriverIcon, RocketLaunchIcon, ArrowUturnLeftIcon, CheckIcon, XMarkIcon, ClockIcon } from '@heroicons/vue/24/outline'
import type { Build, Release } from '~/../shared/types'

const ctx = useBuilder()

// The page injects builds/releases through the flow object it owns; expose locally
const flow = ctx.flow
const builds = computed<Build[]>(() => [...(flow.value?.builds ?? [])].sort((a, b) => b.number - a.number))
const releases = computed<Release[]>(() => [...(flow.value?.releases ?? [])].sort((a, b) => b.releasedAt.localeCompare(a.releasedAt)))
const activeRelease = computed(() => releases.value.find((r) => r.status === 'active'))
const latestGood = computed(() => builds.value.find((b) => b.status === 'succeeded'))

const building = ref(false)
const releasing = ref(false)
const releaseTag = ref('')
const releaseNotes = ref('')
const releaseError = ref('')

async function runBuild() {
  building.value = true
  releaseError.value = ''
  try {
    const b = await ctx.build()
    if (b.status === 'succeeded' && flow.value) {
      flow.value.builds = [...(flow.value.builds ?? []), b]
    } else if (flow.value) {
      flow.value.builds = [...(flow.value.builds ?? []), b]
    }
  } catch (e: unknown) {
    releaseError.value = (e as Error).message
  } finally {
    building.value = false
  }
}

async function promote(buildId: string) {
  releasing.value = true
  releaseError.value = ''
  try {
    await ctx.release(buildId, releaseTag.value, releaseNotes.value)
    releaseTag.value = ''
    releaseNotes.value = ''
  } catch (e: unknown) {
    releaseError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? (e as Error).message
  } finally {
    releasing.value = false
  }
}

async function doRollback(releaseId: string) {
  if (!confirm('Roll the live short code back to this release?')) return
  try {
    const rel = await ctx.rollback(releaseId)
    if (flow.value) flow.value.releases = [rel, ...(flow.value.releases ?? []).map((r) => (r.status === 'active' ? { ...r, status: 'rolled-back' as const } : r))]
  } catch (e: unknown) {
    alert((e as { data?: { statusMessage?: string } }).data?.statusMessage ?? (e as Error).message)
  }
}

const timeAgo = (iso: string) => {
  const s = (Date.now() - new Date(iso).getTime()) / 1000
  if (s < 60) return 'just now'
  if (s < 3600) return `${Math.floor(s / 60)}m ago`
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`
  return `${Math.floor(s / 86400)}d ago`
}
const relStyle: Record<string, string> = {
  active: 'bg-emerald-50 text-emerald-700 ring-emerald-200',
  superseded: 'bg-slate-100 text-slate-500 ring-slate-200',
  'rolled-back': 'bg-rose-50 text-rose-600 ring-rose-200',
}
</script>

<template>
  <div class="p-5">
    <h3 class="text-sm font-bold text-slate-800">Build &amp; release pipeline</h3>
    <p class="mt-1 text-xs text-slate-500">Every build serialises the canvas to a Sapo blueprint and validates it like the engine's parser. Promote a green build to make it live — roll back any time.</p>

    <!-- active state -->
    <div class="mt-4 rounded-xl bg-gradient-to-r from-brand-600 to-fuchsia-600 p-4 text-white shadow-md shadow-brand-200">
      <div class="flex items-center justify-between">
        <div>
          <div class="text-[10px] font-bold uppercase tracking-wider text-brand-100">Currently live</div>
          <div class="mt-0.5 text-xl font-extrabold">{{ activeRelease?.tag ?? '— not released —' }}</div>
          <div v-if="activeRelease" class="text-[11px] text-brand-100">build {{ activeRelease.buildRunId }} · deployed {{ timeAgo(activeRelease.releasedAt) }}</div>
        </div>
        <RocketLaunchIcon class="h-8 w-8 text-brand-200" />
      </div>
    </div>

    <button
      class="mt-4 flex w-full items-center justify-center gap-2 rounded-xl bg-slate-900 py-3 text-sm font-bold text-white hover:bg-slate-700 disabled:opacity-60"
      :disabled="building" @click="runBuild()"
    >
      <WrenchScrewdriverIcon class="h-4 w-4" />
      {{ building ? 'Building…' : 'Build & validate now' }}
    </button>
    <p v-if="releaseError" class="mt-2 text-xs text-rose-600">{{ releaseError }}</p>

    <!-- promote box -->
    <div v-if="latestGood" class="mt-3 rounded-xl border border-slate-200 p-3.5">
      <div class="flex items-center justify-between">
        <div class="text-xs font-bold text-slate-700">Promote latest green build</div>
        <span class="rounded-full bg-emerald-50 px-2 py-0.5 font-mono text-[10px] font-bold text-emerald-700">{{ latestGood.runId }}</span>
      </div>
      <div class="mt-2 grid grid-cols-2 gap-2">
        <input v-model="releaseTag" :placeholder="`auto (next patch)`" class="rounded-lg border border-slate-200 px-2.5 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" />
        <input v-model="releaseNotes" placeholder="Release notes" class="rounded-lg border border-slate-200 px-2.5 py-2 text-xs focus:border-brand-400 focus:outline-none" />
      </div>
      <button
        class="mt-2 flex w-full items-center justify-center gap-2 rounded-lg bg-gradient-to-r from-brand-600 to-fuchsia-600 py-2 text-xs font-bold text-white hover:from-brand-500 hover:to-fuchsia-500 disabled:opacity-60"
        :disabled="releasing" @click="promote(latestGood.id)"
      >
        <RocketLaunchIcon class="h-4 w-4" /> {{ releasing ? 'Deploying…' : 'Release to short code' }}
      </button>
    </div>

    <!-- build history -->
    <div class="mt-6">
      <div class="mb-2 text-[11px] font-bold uppercase tracking-wider text-slate-400">Build history</div>
      <div v-if="!builds.length" class="rounded-xl border border-dashed border-slate-200 p-4 text-center text-xs text-slate-400">No builds yet — hit “Build &amp; validate”.</div>
      <div v-for="b in builds" :key="b.id" class="mb-2 rounded-xl border border-slate-100 p-3">
        <div class="flex items-center gap-2">
          <span class="flex h-5 w-5 items-center justify-center rounded-full" :class="b.status === 'succeeded' ? 'bg-emerald-100 text-emerald-600' : 'bg-rose-100 text-rose-600'">
            <CheckIcon v-if="b.status === 'succeeded'" class="h-3.5 w-3.5" />
            <XMarkIcon v-else class="h-3.5 w-3.5" />
          </span>
          <span class="font-mono text-xs font-bold text-slate-700">#{{ b.number }} · {{ b.runId }}</span>
          <span class="ml-auto flex items-center gap-1 text-[10.5px] text-slate-400"><ClockIcon class="h-3 w-3" /> {{ b.durationMs }}ms · {{ timeAgo(b.createdAt) }}</span>
        </div>
        <div v-if="b.errors.length" class="mt-2 space-y-1">
          <div v-for="(err, i) in b.errors.slice(0, 4)" :key="i" class="rounded-lg bg-rose-50 px-2.5 py-1.5 text-[11px] leading-snug text-rose-700">
            <button v-if="err.nodeId" class="font-mono font-bold underline decoration-dotted" @click="ctx.selectedId.value = err.nodeId">{{ err.nodeId }}</button>
            <span class="ml-1">{{ err.message }}</span>
          </div>
          <div v-if="b.errors.length > 4" class="px-2 text-[10.5px] text-rose-500">+{{ b.errors.length - 4 }} more…</div>
        </div>
        <div v-else class="mt-1.5 text-[11px] text-slate-500">
          Blueprint valid · {{ (b.blueprint.nodes ?? []).length }} nodes
          <span v-if="b.warnings.length" class="text-amber-600"> · {{ b.warnings.length }} warning{{ b.warnings.length > 1 ? 's' : '' }}</span>
        </div>
        <button
          v-if="b.status === 'succeeded' && b.id !== activeRelease?.buildId"
          class="mt-2 inline-flex items-center gap-1 rounded-lg border border-slate-200 px-2.5 py-1 text-[11px] font-semibold text-slate-600 hover:border-brand-300 hover:text-brand-700"
          @click="promote(b.id)"
        >Promote this build</button>
      </div>
    </div>

    <!-- release timeline -->
    <div class="mt-6">
      <div class="mb-2 text-[11px] font-bold uppercase tracking-wider text-slate-400">Release &amp; deployment history</div>
      <div v-if="!releases.length" class="rounded-xl border border-dashed border-slate-200 p-4 text-center text-xs text-slate-400">No releases yet.</div>
      <div class="relative space-y-3 border-l-2 border-slate-100 pl-4">
        <div v-for="r in releases" :key="r.id" class="relative">
          <span class="absolute -left-[22px] top-1.5 h-2.5 w-2.5 rounded-full ring-2 ring-white" :class="r.status === 'active' ? 'bg-emerald-500' : r.status === 'rolled-back' ? 'bg-rose-400' : 'bg-slate-300'" />
          <div class="rounded-xl border p-3" :class="r.status === 'active' ? 'border-emerald-200 bg-emerald-50/40' : 'border-slate-100'">
            <div class="flex flex-wrap items-center gap-2">
              <span class="text-sm font-extrabold text-slate-800">{{ r.tag }}</span>
              <span class="rounded-full px-2 py-0.5 text-[10px] font-bold uppercase ring-1" :class="relStyle[r.status]">{{ r.status.replace('-', ' ') }}</span>
              <span v-if="r.rollbackOf" class="rounded-full bg-amber-50 px-2 py-0.5 text-[10px] font-bold text-amber-600 ring-1 ring-amber-200">rollback</span>
              <span class="ml-auto font-mono text-[10px] text-slate-400">{{ new Date(r.releasedAt).toLocaleString() }}</span>
            </div>
            <div class="mt-1 text-[11px] text-slate-500">
              build <span class="font-mono">{{ r.buildRunId }}</span> · by {{ r.releasedBy }}
              <span v-if="r.notes"> · {{ r.notes }}</span>
            </div>
            <button
              v-if="r.status !== 'active'"
              class="mt-2 inline-flex items-center gap-1 rounded-lg bg-slate-900 px-2.5 py-1 text-[11px] font-semibold text-white hover:bg-slate-700"
              @click="doRollback(r.id)"
            >
              <ArrowUturnLeftIcon class="h-3.5 w-3.5" /> Roll back here
            </button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
