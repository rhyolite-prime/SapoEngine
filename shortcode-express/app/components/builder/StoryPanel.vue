<script setup lang="ts">
import { SparklesIcon, ExclamationTriangleIcon, StarIcon } from '@heroicons/vue/24/outline'
import { KIND_ICONS, kindVisual } from '~/utils/nodeVisuals'
import { flowStory } from '~/utils/flowExplain'
import { paletteByKind } from '~/../shared/utils/sapo'
import type { NodeKind } from '~/../shared/types'

// Chronological outline of the flow: walk from the entry node following the
// routing order (options, branches, next). Click any step to jump to it.
const ctx = useBuilder()
const emit = defineEmits<{ (e: 'focus', id: string): void; (e: 'tidy'): void }>()

const story = computed(() => flowStory(ctx.nodes.value as never, ctx.edges.value, ctx.entryId.value))
const nodeCount = computed(() => ctx.nodes.value.length)

const iconFor = (kind: NodeKind) => KIND_ICONS[paletteByKind[kind]?.icon ?? 'variable']
const visualOf = (kind: NodeKind) => kindVisual(kind)
</script>

<template>
  <div class="p-4">
    <div class="mb-3 flex items-center justify-between">
      <div>
        <h3 class="text-sm font-bold text-slate-800">Flow story</h3>
        <p class="text-[11px] text-slate-500">Execution order from the entry node — click a step to jump to it.</p>
      </div>
      <button
        class="inline-flex shrink-0 items-center gap-1.5 rounded-lg bg-white px-2.5 py-1.5 text-[11px] font-bold text-slate-700 ring-1 ring-slate-200 hover:bg-slate-50"
        title="Re-arrange the canvas in chronological order"
        @click="$emit('tidy')"
      >
        <SparklesIcon class="h-3.5 w-3.5 text-brand-600" /> Tidy
      </button>
    </div>

    <div v-if="!story.entries.length" class="rounded-xl bg-slate-50 px-4 py-6 text-center text-xs text-slate-400">
      <template v-if="!nodeCount">The canvas is empty — drag a node in from the palette.</template>
      <template v-else>No entry node set — pick one in Flow settings to see the execution order.</template>
    </div>

    <ol v-else class="relative space-y-0.5">
      <!-- spine -->
      <span class="absolute bottom-2 left-[13px] top-2 w-px bg-slate-200"></span>
      <li
        v-for="e in story.entries" :key="e.id + e.n"
        class="group relative flex cursor-pointer items-start gap-2.5 rounded-lg py-1.5 pl-1 pr-2 transition hover:bg-brand-50/60"
        :class="ctx.selectedId.value === e.id ? 'bg-brand-50 ring-1 ring-brand-200' : ''"
        @click="$emit('focus', e.id)"
      >
        <span class="relative z-10 mt-0.5 flex h-6 w-6 shrink-0 items-center justify-center rounded-full text-[10px] font-black"
          :class="ctx.selectedId.value === e.id ? 'bg-brand-600 text-white' : 'bg-white text-slate-500 ring-1 ring-slate-200 group-hover:ring-brand-300'">
          {{ e.n }}
        </span>
        <div class="min-w-0 flex-1">
          <div class="flex items-center gap-1.5">
            <component :is="iconFor(e.kind)" class="h-3.5 w-3.5 shrink-0" :class="visualOf(e.kind).icon" />
            <span class="truncate text-[12.5px] font-bold text-slate-800">{{ e.label }}</span>
            <StarIcon v-if="ctx.entryId.value === e.id" class="h-3 w-3 shrink-0 text-amber-400" title="Entry node" />
            <span class="ml-auto shrink-0 font-mono text-[9.5px] text-slate-400">#{{ e.id }}</span>
          </div>
          <div v-if="e.n > 1" class="truncate text-[10.5px] text-slate-400">via {{ e.via }}</div>
        </div>
      </li>
    </ol>

    <div v-if="story.unreachable.length" class="mt-4 rounded-xl bg-amber-50 p-3 ring-1 ring-amber-100">
      <div class="flex items-center gap-1.5 text-[11px] font-bold text-amber-700">
        <ExclamationTriangleIcon class="h-4 w-4" /> Not reachable from the entry ({{ story.unreachable.length }})
      </div>
      <p class="mt-0.5 text-[10.5px] text-amber-600/80">These nodes never run — connect them or remove them before releasing.</p>
      <div class="mt-2 flex flex-wrap gap-1.5">
        <button
          v-for="n in story.unreachable" :key="n.id"
          class="rounded-full bg-white px-2 py-0.5 font-mono text-[10px] font-bold text-amber-700 ring-1 ring-amber-200 hover:bg-amber-100"
          @click="$emit('focus', n.id)"
        >#{{ n.id }} {{ (n.data.config as Record<string, unknown>).label || n.data.kind }}</button>
      </div>
    </div>
  </div>
</template>
