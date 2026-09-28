<script setup lang="ts">
import { Handle, Position } from '@vue-flow/core'
import { paletteByKind } from '~/../shared/utils/sapo'
import { computed } from 'vue'
import { KIND_ICONS, kindVisual } from '~/utils/nodeVisuals'

const props = defineProps<{ id: string; selected?: boolean }>()

const ctx = useBuilder()
const node = computed(() => ctx.nodes.value.find((n) => n.id === props.id))
const kind = computed(() => node.value?.data.kind ?? 'assign')
const config = computed(() => node.value?.data.config ?? {})
const def = computed(() => paletteByKind[kind.value])
const visual = computed(() => kindVisual(kind.value))
const icon = computed(() => KIND_ICONS[def.value?.icon ?? 'variable'])
const isEntry = computed(() => ctx.entryId.value === props.id)

const outputs = computed(() => (def.value?.outputs?.(node.value as never) ?? []))

const summary = computed<string>(() => {
  const d = config.value as Record<string, unknown>
  switch (kind.value) {
    case 'menu': return String(d.autoMessage !== false ? '' : (d.message ?? '')).slice(0, 90)
    case 'input': case 'pin': case 'display': return String(d.message ?? '').slice(0, 90)
    case 'await_event': return `event: ${d.eventName ?? ''}`
    case 'http': return `${String(d.verb ?? 'post').toUpperCase()} ${String(d.url ?? '').replace(/^https?:\/\//, '').slice(0, 42)}`
    case 'subflow': return `→ ${d.workflow ?? ''}`
    case 'event': return String(d.eventName ?? '')
    case 'if': return String(d.condition ?? '')
    case 'choice': return String(d.expression ?? '')
    case 'try': return `catch → $${d.errorVariable ?? 'error'}`
    case 'loop': return d.loopMode === 'collection' ? `each ${d.iterator ?? 'item'} in ${d.collection ?? ''}` : d.loopMode === 'count' ? `${d.count}×` : `while ${d.while ?? ''}`
    case 'parallel': return `${d.mergePolicy ?? 'wait_all'}`
    case 'script': return String(d.scriptCode ?? '').split('\n')[0].slice(0, 60)
    case 'break': return d.breakAction === 'continue' ? 'continue loop' : 'break loop'
    case 'assign': return (d.assignments as { key: string }[] | undefined)?.map((a) => a.key).join(', ') || 'no variables'
    case 'transform': return `${d.operation} → ${d.outputKey ?? 'result'}`
    case 'query': return `${d.source ?? ''} → ${d.outputKey ?? 'rows'}`
    case 'wait': return `wait ${d.duration ?? ''}`
    case 'schedule': return String(d.cron ?? '')
    case 'end_success': case 'end_failure': return String(d.endMessage ?? '').slice(0, 90)
    default: return ''
  }
})

// handles that should render as a compact vertical stack on the right edge
const stackHandles = computed(() => outputs.value.filter((o) => !o.handle.startsWith('option:') && !o.handle.startsWith('case:')))
const rowHandles = computed(() => outputs.value.filter((o) => o.handle.startsWith('option:') || o.handle.startsWith('case:')))

const handleColor = (hk: string) =>
  hk === 'error' || hk === 'catch' ? '#f43f5e'
    : hk === 'then' || hk === 'body' ? '#10b981'
      : hk === 'else' || hk === 'default' ? '#f59e0b'
        : hk.startsWith('option:') || hk.startsWith('case:') || hk.startsWith('branch:') ? '#7c3aed'
          : '#64748b'

const typeLabel = computed(() => {
  const map: Record<string, string> = {
    menu: 'Menu', input: 'Input', pin: 'PIN', display: 'Display', await_event: 'Await event',
    http: 'HTTP API', subflow: 'Subflow', event: 'Event', if: 'Condition', choice: 'Router',
    try: 'Try/Catch', loop: 'Loop', parallel: 'Parallel', script: 'Script', break: 'Loop control',
    assign: 'Assign', transform: 'Transform', query: 'Query', wait: 'Wait', schedule: 'Schedule',
    end_success: 'End ✓', end_failure: 'End ✗',
  }
  return map[kind.value] ?? kind.value
})
</script>

<template>
  <div
    v-if="node"
    class="w-[248px] rounded-xl bg-white shadow-md transition-shadow"
    :class="[selected ? `ring-2 ${visual.ring} shadow-lg` : 'ring-1 ring-slate-200/80 hover:shadow-lg']"
  >
    <Handle type="target" :position="Position.Left" class="!h-3.5 !w-3.5 !bg-white !border-slate-300" />

    <!-- header -->
    <div class="flex items-center gap-2 rounded-t-xl px-3 py-2" :class="visual.head">
      <component :is="icon" class="h-4 w-4 shrink-0" :class="visual.icon" />
      <span class="text-[10px] font-extrabold uppercase tracking-wider" :class="visual.text">{{ typeLabel }}</span>
      <span class="ml-auto truncate text-[10px] font-semibold text-slate-400">{{ node.id }}</span>
      <span v-if="isEntry" title="Entry node — first node in the blueprint" class="flex h-4 w-4 shrink-0 items-center justify-center rounded-full bg-amber-400 text-[8px] font-black text-white">▶</span>
    </div>

    <!-- body -->
    <div class="px-3 py-2.5">
      <div class="text-[12.5px] font-bold leading-snug text-slate-800">{{ config.label || node.id }}</div>
      <div v-if="summary" class="mt-1 break-words font-mono text-[10px] leading-relaxed text-slate-500">{{ summary }}</div>

      <!-- menu options / choice cases with per-row handles -->
      <div v-if="rowHandles.length" class="mt-2 space-y-1">
        <div v-for="o in rowHandles" :key="o.handle" class="relative flex items-center justify-between rounded-md bg-slate-50 px-2 py-1 text-[10.5px]">
          <span class="font-bold text-slate-700">{{ o.label }}</span>
          <span class="max-w-[120px] truncate pl-2 text-slate-400">{{ rowText(o.handle) }}</span>
          <Handle
            :id="o.handle" type="source" :position="Position.Right"
            class="!h-3 !w-3 !border-white"
            :style="{ position: 'absolute', right: '-13.5px', top: '50%', transform: 'translateY(-50%)', background: handleColor(o.handle) }"
          />
        </div>
      </div>
    </div>

    <!-- stacked handles -->
    <div class="relative h-0">
      <Handle
        v-for="(o, i) in stackHandles" :key="o.handle"
        :id="o.handle" type="source" :position="Position.Right"
        class="!h-3 !w-3 !border-white"
        :style="{ position: 'absolute', right: '-13.5px', top: `${-14 - (stackHandles.length - 1 - i) * 22}px`, background: handleColor(o.handle) }"
      />
    </div>

    <!-- handle legend -->
    <div v-if="stackHandles.length" class="flex flex-wrap gap-1 border-t border-slate-100 px-3 py-1.5">
      <span v-for="o in stackHandles" :key="o.handle" class="inline-flex items-center gap-1 text-[9px] font-semibold uppercase tracking-wide text-slate-400">
        <span class="h-1.5 w-1.5 rounded-full" :style="{ background: handleColor(o.handle) }" />{{ o.label }}
      </span>
    </div>
  </div>
</template>

