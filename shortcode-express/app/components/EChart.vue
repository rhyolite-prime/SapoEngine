<script setup lang="ts">
import { use, type EChartsOption } from 'echarts/core'
import { CanvasRenderer } from 'echarts/renderers'
import { LineChart, BarChart, PieChart, GaugeChart } from 'echarts/charts'
import {
  GridComponent, TooltipComponent, LegendComponent, TitleComponent,
  DataZoomComponent, MarkLineComponent,
} from 'echarts/components'
import VChart from 'vue-echarts'
import { watch } from 'vue'

use([CanvasRenderer, LineChart, BarChart, PieChart, GaugeChart, GridComponent, TooltipComponent, LegendComponent, TitleComponent, DataZoomComponent, MarkLineComponent])

const props = defineProps<{ option: EChartsOption; height?: string }>()

const themed = ref<EChartsOption>({})
const dark = ref(false)

function applyTheme(option: EChartsOption): EChartsOption {
  const clone = JSON.parse(JSON.stringify(option))
  const axes = (clone as Record<string, unknown>).xAxis ?? (clone as Record<string, unknown>).yAxis
  const tint = (axis: unknown) => {
    if (Array.isArray(axis)) axis.forEach(tint)
    else if (axis && typeof axis === 'object') {
      const a = axis as Record<string, unknown>
      a.axisLine = { lineStyle: { color: '#cbd5e1' }, ...(a.axisLine as object ?? {}) }
      a.axisLabel = { color: '#64748b', ...(a.axisLabel as object ?? {}) }
      a.splitLine = { lineStyle: { color: '#e2e8f0' } }
    }
  }
  tint(axes)
  return clone
}

watch(() => props.option, (o) => { themed.value = applyTheme(o) }, { immediate: true, deep: true })
void dark
</script>

<template>
  <ClientOnly>
    <VChart class="w-full" :style="{ height: height ?? '300px' }" :option="themed" autoresize />
    <template #fallback>
      <div :style="{ height: height ?? '300px' }" class="flex items-center justify-center text-sm text-slate-400">
        <svg class="mr-2 h-4 w-4 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" class="opacity-90" /></svg>
        Loading chart…
      </div>
    </template>
  </ClientOnly>
</template>
