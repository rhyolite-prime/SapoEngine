<script setup lang="ts">
import { ArrowRightIcon, ExclamationTriangleIcon, StarIcon } from '@heroicons/vue/24/outline'
import { KIND_ICONS, kindVisual } from '~/utils/nodeVisuals'
import { explainNode, isTerminal, nextSteps } from '~/utils/flowExplain'
import { paletteByKind } from '~/../shared/utils/sapo'

// Floating "what happens here / what happens next" card. The page tracks the
// hovered node + cursor position; this renders the explanation.
const props = defineProps<{ nodeId: string; x: number; y: number }>()

const ctx = useBuilder()
const node = computed(() => ctx.nodes.value.find((n) => n.id === props.nodeId) ?? null)
const kind = computed(() => node.value?.data.kind ?? 'assign')
const config = computed(() => (node.value?.data.config ?? {}) as Record<string, unknown>)
const def = computed(() => paletteByKind[kind.value])
const visual = computed(() => kindVisual(kind.value))
const icon = computed(() => KIND_ICONS[def.value?.icon ?? 'variable'])
const steps = computed(() => (node.value ? nextSteps(node.value.id, ctx.nodes.value as never, ctx.edges.value) : []))
const isEntry = computed(() => ctx.entryId.value === props.nodeId)
const deadEnd = computed(() => !!node.value && !isTerminal(kind.value) && steps.value.length === 0)

const FLIP_W = 320, FLIP_H = 260
const posStyle = computed(() => {
  // flip when we'd run off the right/bottom of the viewport
  const flipX = props.x + FLIP_W + 24 > (typeof window !== 'undefined' ? window.innerWidth : 9999)
  const flipY = props.y + FLIP_H + 24 > (typeof window !== 'undefined' ? window.innerHeight : 9999)
  return {
    left: `${flipX ? props.x - FLIP_W - 16 : props.x + 16}px`,
    top: `${flipY ? props.y - 12 - Math.min(FLIP_H, 240) : props.y + 16}px`,
  }
})
</script>

<template>
  <div
    v-if="node"
    class="pointer-events-none fixed z-50 w-[320px] overflow-hidden rounded-xl bg-ink-950/95 text-left shadow-2xl ring-1 ring-white/10 backdrop-blur"
    :style="posStyle"
  >
    <!-- header -->
    <div class="flex items-center gap-2.5 px-4 py-3" :class="visual.head">
      <component :is="icon" class="h-4 w-4 shrink-0" :class="visual.icon" />
      <span class="text-[10px] font-extrabold uppercase tracking-wider" :class="visual.text">{{ def?.name ?? kind }}</span>
      <span v-if="isEntry" class="ml-auto flex items-center gap-1 rounded-full bg-amber-400/90 px-2 py-0.5 text-[9px] font-black uppercase text-white">
        <StarIcon class="h-3 w-3" /> entry
      </span>
      <span v-else class="ml-auto font-mono text-[10px] font-bold text-slate-500">#{{ node.id }}</span>
    </div>

    <div class="space-y-3 px-4 py-3">
      <div class="text-[13px] font-bold leading-snug text-slate-100">{{ config.label || node.id }}</div>

      <!-- what happens -->
      <div>
        <div class="mb-1 text-[9px] font-bold uppercase tracking-widest text-brand-300">What happens here</div>
        <p class="text-[11.5px] leading-relaxed text-slate-300">{{ explainNode(kind, config as never) }}</p>
      </div>

      <!-- what's next -->
      <div v-if="steps.length">
        <div class="mb-1 text-[9px] font-bold uppercase tracking-widest text-brand-300">What happens next</div>
        <div class="space-y-1">
          <div v-for="s in steps" :key="s.targetId + s.label" class="flex items-start gap-1.5 text-[11px] leading-snug">
            <ArrowRightIcon class="mt-0.5 h-3 w-3 shrink-0 text-brand-400" />
            <div class="min-w-0">
              <span class="text-slate-400">{{ s.label }}</span>
              <span class="font-semibold text-slate-100"> {{ s.targetLabel }}</span>
              <span class="font-mono text-[9.5px] text-slate-500"> #{{ s.targetId }}</span>
            </div>
          </div>
        </div>
      </div>

      <div v-else-if="isTerminal(kind)" class="flex items-center gap-1.5 rounded-lg bg-white/5 px-2.5 py-2 text-[11px] font-semibold text-slate-300">
        <span class="inline-block h-2 w-2 rounded-full" :class="kind === 'end_success' ? 'bg-success-400' : 'bg-rose-400'" />
        Session ends here — the subscriber sees the message above.
      </div>

      <div v-else-if="deadEnd" class="flex items-start gap-1.5 rounded-lg bg-amber-500/10 px-2.5 py-2 text-[11px] font-medium text-amber-300 ring-1 ring-amber-500/20">
        <ExclamationTriangleIcon class="mt-0.5 h-3.5 w-3.5 shrink-0" />
        Nothing comes after this node — execution stops here. Connect its “next” handle if the session should continue.
      </div>
    </div>
  </div>
</template>
