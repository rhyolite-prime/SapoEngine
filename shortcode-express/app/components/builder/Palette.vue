<script setup lang="ts">
import { PALETTE } from '~/../shared/utils/sapo'
import { KIND_ICONS, kindVisual } from '~/utils/nodeVisuals'

const groups = computed(() => {
  const map = new Map<string, typeof PALETTE>()
  for (const p of PALETTE) {
    if (!map.has(p.group)) map.set(p.group, [])
    map.get(p.group)!.push(p)
  }
  return [...map.entries()]
})

const ctx = useBuilder()
const dragKind = ref<string | null>(null)

function onDragStart(e: DragEvent, kind: string) {
  dragKind.value = kind
  e.dataTransfer?.setData('application/sapo-node', kind)
  e.dataTransfer!.effectAllowed = 'move'
}

function addAtCenter(kind: string) {
  // place new node near the middle of the current view
  const cx = (window.innerWidth - 360 - 216) / 2 - 124
  const cy = (window.innerHeight - 56) / 2 - 60
  const jitter = (Math.random() - 0.5) * 80
  ctx.addNode(kind as never, { x: cx + jitter, y: cy + jitter })
}
</script>

<template>
  <aside class="flex w-60 shrink-0 flex-col overflow-y-auto border-r border-slate-200 bg-white">
    <div class="px-4 pb-2 pt-4">
      <div class="text-[11px] font-bold uppercase tracking-wider text-slate-400">Node palette</div>
      <p class="mt-1 text-[10.5px] leading-snug text-slate-400">Drag onto the canvas or click to add. Each node is a real Sapo DSL node.</p>
    </div>
    <div v-for="[group, items] in groups" :key="group" class="px-3 pb-3">
      <div class="mb-1.5 px-1 text-[10px] font-bold uppercase tracking-wider text-slate-400">{{ group }}</div>
      <div class="space-y-1">
        <button
          v-for="p in items" :key="p.kind"
          draggable="true"
          class="group flex w-full cursor-grab items-center gap-2.5 rounded-lg border border-slate-100 bg-white px-2.5 py-2 text-left transition hover:border-brand-300 hover:bg-brand-50/60 active:cursor-grabbing"
          :title="p.hint"
          @dragstart="onDragStart($event, p.kind)"
          @click="addAtCenter(p.kind)"
        >
          <span class="flex h-7 w-7 shrink-0 items-center justify-center rounded-lg" :class="[kindVisual(p.kind).head, kindVisual(p.kind).icon]">
            <component :is="KIND_ICONS[p.icon]" class="h-4 w-4" />
          </span>
          <span class="min-w-0 flex-1">
            <span class="block truncate text-[12px] font-semibold text-slate-700 group-hover:text-slate-900">{{ p.name }}</span>
            <span class="block truncate font-mono text-[9.5px] text-slate-400">{{ p.sapoType }}</span>
          </span>
        </button>
      </div>
    </div>
    <div class="mt-auto border-t border-slate-100 p-4">
      <div class="rounded-xl bg-slate-50 p-3 text-[10.5px] leading-relaxed text-slate-500">
        <b class="text-slate-600">Tip:</b> connect the colored dots —
        <span class="font-bold text-success-600">green</span> = body/then,
        <span class="font-bold text-brand-600">plum</span> = menu options,
        <span class="font-bold text-rose-600">red</span> = errors.
      </div>
    </div>
  </aside>
</template>
