<script setup lang="ts">
import { XMarkIcon, ClipboardDocumentIcon, ClipboardDocumentCheckIcon, ArrowDownTrayIcon, ArrowUpTrayIcon } from '@heroicons/vue/24/outline'
import type { SapoBlueprint } from '~/../shared/types'
import { validateBlueprint } from '~/../shared/utils/sapo'

const emit = defineEmits<{ (e: 'close'): void }>()
const ctx = useBuilder()

const blueprint = computed<SapoBlueprint>(() => ctx.toBlueprint())
const json = computed(() => JSON.stringify(blueprint.value, null, 2))
const copied = ref(false)
const importText = ref('')
const importError = ref('')
const showImport = ref(false)

async function copy() {
  await navigator.clipboard?.writeText(json.value)
  copied.value = true
  setTimeout(() => (copied.value = false), 1600)
}

function download() {
  const name = (blueprint.value.name ?? 'blueprint').replace(/\./g, '_')
  const blob = new Blob([json.value], { type: 'application/json' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = `${name}.json`
  a.click()
  URL.revokeObjectURL(a.href)
}

function doImport() {
  try {
    const parsed = JSON.parse(importText.value)
    if (!Array.isArray(parsed.nodes)) throw new Error('blueprint must contain a "nodes" array')
    if (!confirm('Replace the current canvas with the imported blueprint?')) return
    ctx.importBlueprint(parsed as SapoBlueprint)
    importError.value = ''
    importText.value = ''
    showImport.value = false
    emit('close')
  } catch (e) {
    importError.value = (e as Error).message
  }
}

const validation = computed(() => validateBlueprint(blueprint.value, ctx.toGraph()))
</script>

<template>
  <div class="fixed inset-0 z-50 flex items-end justify-center bg-slate-900/40 p-4 backdrop-blur-sm" @click.self="emit('close')">
    <div class="flex h-[78vh] w-full max-w-5xl flex-col overflow-hidden rounded-2xl bg-white shadow-2xl">
      <header class="flex h-12 shrink-0 items-center gap-3 border-b border-slate-200 px-5">
        <span class="text-sm font-bold text-slate-800">Sapo DSL blueprint</span>
        <span class="rounded-full bg-brand-50 px-2.5 py-0.5 font-mono text-[11px] font-bold text-brand-700">{{ blueprint.name }}</span>
        <span v-if="validation.ok" class="rounded-full bg-emerald-50 px-2.5 py-0.5 text-[11px] font-bold text-emerald-700">✓ valid</span>
        <span v-else class="rounded-full bg-rose-50 px-2.5 py-0.5 text-[11px] font-bold text-rose-600">{{ validation.errors.length }} error(s)</span>
        <div class="ml-auto flex items-center gap-1.5">
          <button class="inline-flex items-center gap-1.5 rounded-lg border border-slate-200 px-2.5 py-1.5 text-xs font-semibold text-slate-600 hover:border-slate-300" @click="copy()">
            <component :is="copied ? ClipboardDocumentCheckIcon : ClipboardDocumentIcon" class="h-4 w-4" :class="copied ? 'text-emerald-500' : ''" />
            {{ copied ? 'Copied' : 'Copy' }}
          </button>
          <button class="inline-flex items-center gap-1.5 rounded-lg border border-slate-200 px-2.5 py-1.5 text-xs font-semibold text-slate-600 hover:border-slate-300" @click="download()">
            <ArrowDownTrayIcon class="h-4 w-4" /> Download
          </button>
          <button class="inline-flex items-center gap-1.5 rounded-lg border border-slate-200 px-2.5 py-1.5 text-xs font-semibold text-slate-600 hover:border-slate-300" @click="showImport = !showImport">
            <ArrowUpTrayIcon class="h-4 w-4" /> {{ showImport ? 'Cancel import' : 'Import' }}
          </button>
          <button class="rounded-lg p-1.5 text-slate-400 hover:bg-slate-100" @click="emit('close')"><XMarkIcon class="h-5 w-5" /></button>
        </div>
      </header>

      <div v-if="showImport" class="shrink-0 border-b border-slate-200 bg-slate-50 p-4">
        <textarea v-model="importText" rows="5" placeholder='Paste a blueprint JSON here — it will be imported onto the canvas' class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none" />
        <div class="mt-2 flex items-center gap-2">
          <button class="rounded-lg bg-brand-600 px-4 py-1.5 text-xs font-bold text-white hover:bg-brand-500" @click="doImport()">Import to canvas</button>
          <p v-if="importError" class="text-xs text-rose-600">{{ importError }}</p>
        </div>
      </div>

      <div class="grid min-h-0 flex-1 grid-cols-[1fr_300px]">
        <pre class="sce-json overflow-auto bg-ink-950 p-5 leading-relaxed text-slate-200">{{ json }}</pre>
        <div class="overflow-y-auto border-l border-slate-200 p-4">
          <div class="text-[11px] font-bold uppercase tracking-wider text-slate-400">Validation</div>
          <div v-if="!validation.errors.length && !validation.warnings.length" class="mt-3 rounded-xl bg-emerald-50 p-3 text-xs text-emerald-700">
            ✓ Blueprint is valid — this exact JSON runs on the Sapo VM (<code class="font-mono">sapoc run blueprint.json</code>).
          </div>
          <div v-for="(e, i) in validation.errors" :key="'e' + i" class="mt-2 rounded-xl bg-rose-50 p-2.5 text-[11px] leading-snug text-rose-700">
            <b class="font-mono">{{ e.nodeId ?? 'blueprint' }}</b> — {{ e.message }}
          </div>
          <div v-for="(w, i) in validation.warnings" :key="'w' + i" class="mt-2 rounded-xl bg-amber-50 p-2.5 text-[11px] leading-snug text-amber-700">
            <b class="font-mono">{{ w.nodeId ?? 'blueprint' }}</b> — {{ w.message }}
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
