<script setup lang="ts">
import { PlusIcon, PencilSquareIcon, DocumentDuplicateIcon, TrashIcon, ArrowUpTrayIcon } from '@heroicons/vue/24/outline'
import type { SapoBlueprint } from '~/../shared/types'
import { blueprintToGraph } from '~/../shared/utils/sapo'

useHead({ title: 'USSD flows · ShortCodeExpress' })

interface FlowRow {
  id: string; name: string; description: string; shortcode: string | null; nodeCount: number
  updatedAt: string; activeRelease: { tag: string; releasedAt: string } | null
  buildCount: number; lastBuildStatus: string | null; lastBuildRunId: string | null
}

const { data: flows, refresh } = await useFetch<FlowRow[]>('/api/flows')
const showNew = ref(false)
const newName = ref('')
const newDesc = ref('')
const newCode = ref('')
const shortcodes = await useFetch<{ id: string; code: string; label: string }[]>('/api/shortcodes')
const importJson = ref('')
const importError = ref('')

async function createFlow() {
  let blueprint: SapoBlueprint | undefined
  if (importJson.value.trim()) {
    try {
      const parsed = JSON.parse(importJson.value)
      if (!Array.isArray(parsed.nodes)) throw new Error('blueprint must have a "nodes" array')
      blueprint = parsed
    } catch (e) {
      importError.value = (e as Error).message
      return
    }
  }
  importError.value = ''
  const flow = await $fetch<{ id: string }>('/api/flows', {
    method: 'POST',
    body: { name: newName.value, description: newDesc.value, shortcodeId: newCode.value || undefined, blueprint },
  })
  showNew.value = false
  newName.value = ''; newDesc.value = ''; importJson.value = ''
  await refresh()
  navigateTo(`/builder/${flow.id}`)
}

async function duplicate(row: FlowRow) {
  const full = await $fetch<{ graph: unknown; description: string; shortcodeId?: string; meta: unknown }>(`/api/flows/${row.id}`)
  const flow = await $fetch<{ id: string }>('/api/flows', {
    method: 'POST',
    body: { name: `${row.name} (copy)`, description: full.description, shortcodeId: full.shortcodeId },
  })
  await $fetch(`/api/flows/${flow.id}`, { method: 'PUT', body: { graph: full.graph, meta: (full as { meta?: unknown }).meta } })
  await refresh()
}

async function remove(row: FlowRow) {
  if (!confirm(`Delete "${row.name}" and its build history? This cannot be undone.`)) return
  await $fetch(`/api/flows/${row.id}`, { method: 'DELETE' })
  await refresh()
}

const ago = (iso: string) => {
  const s = (Date.now() - new Date(iso).getTime()) / 1000
  if (s < 60) return 'just now'
  if (s < 3600) return `${Math.floor(s / 60)}m ago`
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`
  return `${Math.floor(s / 86400)}d ago`
}
</script>

<template>
  <div class="p-8">
    <div class="mb-6 flex items-end justify-between">
      <div>
        <h1 class="text-2xl font-bold text-slate-900">USSD flows</h1>
        <p class="mt-1 text-sm text-slate-500">Every flow compiles to a Sapo DSL blueprint that runs on the Sapo VM — no code required.</p>
      </div>
      <button class="inline-flex items-center gap-2 rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500" @click="showNew = true">
        <PlusIcon class="h-4 w-4" /> New flow
      </button>
    </div>

    <div class="grid gap-4 md:grid-cols-2 xl:grid-cols-3">
      <NuxtLink
        v-for="f in flows ?? []" :key="f.id" :to="`/builder/${f.id}`"
        class="group relative flex flex-col rounded-2xl bg-white p-5 shadow-sm ring-1 ring-slate-200/70 transition hover:-translate-y-0.5 hover:shadow-lg hover:ring-brand-300"
      >
        <div class="flex items-start justify-between">
          <div class="flex h-10 w-10 items-center justify-center rounded-xl bg-brand-50 text-brand-600 ring-1 ring-brand-100">
            <svg viewBox="0 0 24 24" class="h-5 w-5" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 7h16M4 12h10M4 17h7" /><circle cx="18.5" cy="15.5" r="2.5" /></svg>
          </div>
          <span v-if="f.activeRelease" class="rounded-full bg-success-50 px-2.5 py-1 text-[11px] font-bold text-success-700 ring-1 ring-success-200">
            ● {{ f.activeRelease.tag }}
          </span>
          <span v-else class="rounded-full bg-amber-50 px-2.5 py-1 text-[11px] font-bold text-amber-700 ring-1 ring-amber-200">draft</span>
        </div>
        <h3 class="mt-3 font-bold text-slate-900 group-hover:text-brand-700">{{ f.name }}</h3>
        <p class="mt-1 line-clamp-2 flex-1 text-[13px] leading-relaxed text-slate-500">{{ f.description || 'No description yet.' }}</p>
        <div class="mt-4 flex flex-wrap items-center gap-x-4 gap-y-1 border-t border-slate-100 pt-3 text-[11px] text-slate-400">
          <span v-if="f.shortcode" class="font-mono font-semibold text-slate-600">{{ f.shortcode }}</span>
          <span>{{ f.nodeCount }} nodes</span>
          <span>{{ f.buildCount }} builds</span>
          <span v-if="f.lastBuildRunId" :class="f.lastBuildStatus === 'succeeded' ? 'text-success-600' : 'text-rose-600'">{{ f.lastBuildStatus === 'succeeded' ? '✓' : '✗' }} {{ f.lastBuildRunId }}</span>
          <span class="ml-auto">{{ ago(f.updatedAt) }}</span>
        </div>
        <div class="absolute bottom-4 right-4 flex gap-1 opacity-0 transition group-hover:opacity-100">
          <button class="pointer-events-auto rounded-md bg-white/90 p-1.5 text-slate-400 shadow-sm ring-1 ring-slate-200 hover:text-slate-700" title="Duplicate" @click.prevent.stop="duplicate(f)"><DocumentDuplicateIcon class="h-4 w-4" /></button>
          <button class="pointer-events-auto rounded-md bg-white/90 p-1.5 text-slate-400 shadow-sm ring-1 ring-slate-200 hover:text-rose-600" title="Delete" @click.prevent.stop="remove(f)"><TrashIcon class="h-4 w-4" /></button>
        </div>
      </NuxtLink>
    </div>

    <!-- New flow modal -->
    <div v-if="showNew" class="fixed inset-0 z-50 flex items-center justify-center bg-slate-900/40 p-4 backdrop-blur-sm" @click.self="showNew = false">
      <div class="w-full max-w-lg rounded-2xl bg-white p-6 shadow-2xl">
        <h2 class="text-lg font-bold text-slate-900">New USSD flow</h2>
        <p class="mt-1 text-xs text-slate-500">Start from a blank canvas or paste an existing Sapo blueprint to import it.</p>
        <div class="mt-4 space-y-3">
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Name</label>
            <input v-model="newName" placeholder="e.g. Airtime Top-up Service" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-sm focus:border-brand-400 focus:outline-none focus:ring-2 focus:ring-brand-100" />
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Description</label>
            <textarea v-model="newDesc" rows="2" placeholder="What does this service do?" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-sm focus:border-brand-400 focus:outline-none focus:ring-2 focus:ring-brand-100" />
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Short code (optional)</label>
            <select v-model="newCode" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-sm focus:border-brand-400 focus:outline-none">
              <option value="">Not linked yet</option>
              <option v-for="sc in shortcodes.data.value ?? []" :key="sc.id" :value="sc.id">{{ sc.code }} — {{ sc.label }}</option>
            </select>
          </div>
          <div>
            <label class="mb-1 flex items-center gap-1.5 text-xs font-semibold text-slate-600">
              <ArrowUpTrayIcon class="h-3.5 w-3.5" /> Import blueprint JSON (optional)
            </label>
            <textarea v-model="importJson" rows="4" placeholder='{ "name": "examples.hello_world", "nodes": [ … ] }' class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none" />
            <p v-if="importError" class="mt-1 text-xs text-rose-600">{{ importError }}</p>
          </div>
        </div>
        <div class="mt-5 flex justify-end gap-2">
          <button class="rounded-lg px-4 py-2 text-sm font-semibold text-slate-600 hover:bg-slate-100" @click="showNew = false">Cancel</button>
          <button class="rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500" @click="createFlow()">Create &amp; open builder</button>
        </div>
      </div>
    </div>
  </div>
</template>
