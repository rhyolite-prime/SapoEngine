<script setup lang="ts">
import { XMarkIcon, PlusIcon, StarIcon, DocumentDuplicateIcon, TrashIcon, ClipboardDocumentIcon, ClipboardDocumentCheckIcon } from '@heroicons/vue/24/outline'
import { uid, paletteByKind } from '~/../shared/utils/sapo'
import { KIND_ICONS, kindVisual } from '~/utils/nodeVisuals'

const ctx = useBuilder()
const selected = computed(() => ctx.nodes.value.find((n) => n.id === ctx.selectedId.value) ?? null)
const kind = computed(() => selected.value?.data.kind)
const config = computed(() => selected.value?.data.config ?? ({} as Record<string, any>))

const set = (patch: Record<string, unknown>) => selected.value && ctx.updateConfig(selected.value.id, patch as never)

// node reference ids are system-assigned (6 digits) — visible, read-only, copyable
const idCopied = ref(false)
function copyNodeId() {
  if (!selected.value) return
  navigator.clipboard?.writeText(selected.value.id)
  idCopied.value = true
  setTimeout(() => { idCopied.value = false }, 1500)
}

function addRow(list: 'options' | 'assignments' | 'outputs' | 'headers' | 'queryParams' | 'cases' | 'endOutputs') {
  const cur = [...(config.value[list] ?? [])]
  if (list === 'options') cur.push({ id: uid('o'), label: `Option ${cur.length + 1}`, value: String(cur.length + 1) })
  else if (list === 'cases') cur.push({ id: uid('c'), label: `Case ${cur.length + 1}`, value: String(cur.length + 1) })
  else cur.push({ id: uid('k'), key: '', value: '' })
  set({ [list]: cur })
}
function updateRow(list: string, id: string, field: string, value: string) {
  const cur = (config.value[list] ?? []).map((r: any) => (r.id === id ? { ...r, [field]: value } : r))
  set({ [list]: cur })
}
function removeRow(list: string, id: string) {
  ctx.removeEdgeById // noop reference
  const cur = (config.value[list] ?? []).filter((r: any) => r.id !== id)
  // also drop edges leaving that handle
  const handle = `${list === 'options' ? 'option' : 'case'}:${id}`
  if (list === 'options' || list === 'cases') {
    for (const e of [...ctx.edges.value]) if (e.source === selected.value?.id && e.sourceHandle === handle) ctx.removeEdgeById(e.id)
  }
  set({ [list]: cur })
}
function moveRow(list: string, id: string, dir: -1 | 1) {
  const cur = [...(config.value[list] ?? [])]
  const i = cur.findIndex((r: any) => r.id === id)
  const j = i + dir
  if (i < 0 || j < 0 || j >= cur.length) return
  ;[cur[i], cur[j]] = [cur[j], cur[i]]
  set({ [list]: cur })
}

const outgoing = computed(() =>
  ctx.edges.value.filter((e) => e.source === selected.value?.id),
)

const def = computed(() => (kind.value ? paletteByKind[kind.value] : null))
const icon = computed(() => KIND_ICONS[def.value?.icon ?? 'variable'])
const visual = computed(() => kindVisual(kind.value ?? 'assign'))

// flow-level settings (no node selected)
const defaultsJson = computed({
  get: () => JSON.stringify(ctx.meta.defaults ?? {}, null, 2),
  set: (v: string) => { try { ctx.meta.defaults = JSON.parse(v || '{}') } catch {} },
})
const dslName = computed({
  get: () => ctx.meta.name,
  set: (v: string) => { ctx.meta.name = v },
})
const dslVersion = computed({
  get: () => ctx.meta.version,
  set: (v: string) => { ctx.meta.version = v },
})

const validationQuick = [
  { label: '4-digit PIN', expr: "matches(input, '^[0-9]{4}$')" },
  { label: 'Amount (GHS)', expr: "matches(input, '^[0-9]+(\\.[0-9]{1,2})?$')" },
  { label: 'Phone number', expr: "matches(input, '^0[0-9]{9}$')" },
  { label: 'Yes / No', expr: "matches(input, '^(1|2)$')" },
]
</script>

<template>
  <!-- ===================== FLOW SETTINGS ===================== -->
  <div v-if="!selected" class="p-5">
    <h3 class="text-sm font-bold text-slate-800">Flow settings</h3>
    <p class="mt-1 text-xs text-slate-500">These fields map to the top level of the Sapo blueprint.</p>

    <div class="mt-4 space-y-4">
      <div>
        <label class="mb-1 block text-xs font-semibold text-slate-600">Blueprint name (workflow id)</label>
        <input v-model="dslName" placeholder="examples.my_service" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" />
      </div>
      <div>
        <label class="mb-1 block text-xs font-semibold text-slate-600">Version</label>
        <input v-model="dslVersion" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" />
      </div>
      <div>
        <label class="mb-1 block text-xs font-semibold text-slate-600">Defaults (seed context variables)</label>
        <textarea v-model="defaultsJson" rows="6" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none"></textarea>
        <p class="mt-1 text-[10.5px] text-slate-400">e.g. <code class="font-mono">{ "currency": "GHS", "bank_name": "Sapo Bank" }</code></p>
      </div>
      <div>
        <label class="mb-1 block text-xs font-semibold text-slate-600">Entry node</label>
        <select
          :value="ctx.entryId.value ?? ''"
          class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-xs focus:border-brand-400"
          @change="ctx.setEntry(($event.target as HTMLSelectElement).value)"
        >
          <option v-for="n in ctx.nodes.value" :key="n.id" :value="n.id">{{ n.id }} ({{ n.data.kind }})</option>
        </select>
        <p class="mt-1 text-[10.5px] text-slate-400">Execution is jump-based: the entry node is the first node in the blueprint's node list.</p>
      </div>
    </div>
  </div>

  <!-- ===================== NODE SETTINGS ===================== -->
  <div v-else class="p-5">
    <div class="flex items-center gap-2.5">
      <span class="flex h-9 w-9 items-center justify-center rounded-lg" :class="[visual.head, visual.icon]">
        <component :is="icon" class="h-5 w-5" />
      </span>
      <div class="min-w-0 flex-1">
        <div class="text-sm font-bold text-slate-800">{{ def?.name }}</div>
        <div class="font-mono text-[10px] text-slate-400">type: {{ def?.sapoType }}</div>
      </div>
      <!-- (hint rendered below the header row) -->
      <button
        v-if="ctx.entryId.value !== selected.id" title="Set as entry node"
        class="rounded-lg p-1.5 text-slate-300 hover:bg-amber-50 hover:text-amber-500" @click="ctx.setEntry(selected.id)"
      >
        <StarIcon class="h-4 w-4" />
      </button>
      <button title="Duplicate" class="rounded-lg p-1.5 text-slate-300 hover:bg-slate-100 hover:text-slate-600" @click="ctx.duplicateNode(selected.id)">
        <DocumentDuplicateIcon class="h-4 w-4" />
      </button>
      <button title="Delete node" class="rounded-lg p-1.5 text-slate-300 hover:bg-rose-50 hover:text-rose-600" @click="ctx.removeNode(selected.id)">
        <TrashIcon class="h-4 w-4" />
      </button>
    </div>

    <p v-if="def?.hint" class="mt-2 rounded-lg bg-brand-50/70 px-3 py-2 text-[11px] leading-relaxed text-brand-800 ring-1 ring-brand-100">
      {{ def.hint }}
    </p>

    <div class="mt-4 space-y-4">
      <!-- identity -->
      <div class="grid grid-cols-[1fr_auto] gap-3">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Label</label>
          <input :value="config.label ?? ''" placeholder="Human label" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs focus:border-brand-400 focus:outline-none" @input="set({ label: ($event.target as HTMLInputElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Node id</label>
          <div
            class="flex items-center gap-1.5 rounded-lg border border-slate-200 bg-slate-50 px-3 py-2"
            title="System-assigned reference — used as the node id in the Sapo blueprint"
          >
            <span class="font-mono text-xs font-bold tracking-wider text-slate-700">#{{ selected.id }}</span>
            <button class="rounded p-0.5 text-slate-400 hover:bg-white hover:text-slate-700" title="Copy node id" @click="copyNodeId">
              <ClipboardDocumentCheckIcon v-if="idCopied" class="h-3.5 w-3.5 text-success-500" />
              <ClipboardDocumentIcon v-else class="h-3.5 w-3.5" />
            </button>
          </div>
        </div>
      </div>

      <!-- MENU -->
      <template v-if="kind === 'menu'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Menu options</label>
          <p class="mb-2 text-[10.5px] text-slate-400">Each option becomes a numbered line in the prompt and a <code class="font-mono">next_tasks</code> route. Connect its dot on the canvas.</p>
          <div class="space-y-1.5">
            <div v-for="(o, i) in config.options ?? []" :key="o.id" class="flex items-center gap-1.5">
              <input :value="o.value" class="w-11 rounded-md border border-slate-200 px-2 py-1.5 text-center font-mono text-xs" @change="updateRow('options', o.id, 'value', ($event.target as HTMLInputElement).value)" />
              <input :value="o.label" class="min-w-0 flex-1 rounded-md border border-slate-200 px-2 py-1.5 text-xs" @input="updateRow('options', o.id, 'label', ($event.target as HTMLInputElement).value)" />
              <button class="rounded p-1 text-slate-300 hover:text-slate-600" title="Move up" @click="moveRow('options', o.id, -1)">↑</button>
              <button class="rounded p-1 text-slate-300 hover:text-slate-600" title="Move down" @click="moveRow('options', o.id, 1)">↓</button>
              <button class="rounded p-1 text-slate-300 hover:text-rose-500" @click="removeRow('options', o.id)"><XMarkIcon class="h-4 w-4" /></button>
              <span class="w-4 text-center font-mono text-[9px] text-slate-300">{{ i + 1 }}</span>
            </div>
          </div>
          <button class="mt-2 inline-flex items-center gap-1 rounded-lg bg-slate-100 px-2.5 py-1.5 text-[11px] font-semibold text-slate-600 hover:bg-slate-200" @click="addRow('options')">
            <PlusIcon class="h-3.5 w-3.5" /> Add option
          </button>
        </div>
        <div>
          <div class="flex items-center justify-between">
            <label class="text-xs font-semibold text-slate-600">Message</label>
            <label class="flex cursor-pointer items-center gap-1.5 text-[10.5px] text-slate-500">
              <input type="checkbox" :checked="config.autoMessage !== false" class="accent-brand-600" @change="set({ autoMessage: ($event.target as HTMLInputElement).checked })" />
              auto-generate from options
            </label>
          </div>
          <textarea
            v-if="config.autoMessage === false" :value="config.message" rows="4"
            placeholder="Welcome to Sapo Bank&#10;1. Deposit&#10;2. Withdrawal"
            class="mt-1 w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none"
            @input="set({ message: ($event.target as HTMLTextAreaElement).value })"
          />
          <div v-else class="mt-1 rounded-lg bg-slate-50 px-3 py-2 font-mono text-[11px] leading-relaxed text-slate-500">
            {{ (config.options ?? []).map((o: any, i: number) => `${o.value || i + 1}. ${o.label}`).join('\n') }}
          </div>
        </div>
        <div class="grid grid-cols-2 gap-3">
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Input variable</label>
            <input :value="config.inputVariable ?? 'menu_choice'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ inputVariable: ($event.target as HTMLInputElement).value })" />
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Timeout</label>
            <input :value="config.timeout ?? '2m'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ timeout: ($event.target as HTMLInputElement).value })" />
          </div>
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Input validation</label>
          <select :value="config.validationMode ?? 'off'" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-xs" @change="set({ validationMode: ($event.target as HTMLSelectElement).value })">
            <option value="off">Off — unmatched input falls to the "no match" edge</option>
            <option value="auto">Auto — only listed option values</option>
            <option value="custom">Custom SEL predicate</option>
          </select>
          <input
            v-if="config.validationMode === 'custom'" :value="config.customValidation ?? ''"
            placeholder="matches(input, '^[1-5]$')" class="mt-1.5 w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none"
            @input="set({ customValidation: ($event.target as HTMLInputElement).value })"
          />
        </div>
      </template>

      <!-- INPUT / PIN -->
      <template v-else-if="kind === 'input' || kind === 'pin'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Prompt message</label>
          <textarea :value="config.message" rows="3" placeholder="Enter your 4-digit PIN" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs focus:border-brand-400 focus:outline-none" @input="set({ message: ($event.target as HTMLTextAreaElement).value })" />
        </div>
        <div class="grid grid-cols-2 gap-3">
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Input variable</label>
            <input :value="config.inputVariable ?? 'user_input'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ inputVariable: ($event.target as HTMLInputElement).value })" />
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Timeout</label>
            <input :value="config.timeout ?? '2m'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ timeout: ($event.target as HTMLInputElement).value })" />
          </div>
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Validation</label>
          <select :value="config.validationMode ?? 'off'" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-xs" @change="set({ validationMode: ($event.target as HTMLSelectElement).value })">
            <option value="off">Off — accept anything</option>
            <option value="custom">SEL predicate (invalid input → error edge)</option>
          </select>
          <div v-if="config.validationMode === 'custom'" class="mt-1.5 space-y-1.5">
            <input :value="config.customValidation ?? ''" placeholder="matches(input, '^[0-9]{4}$')" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none" @input="set({ customValidation: ($event.target as HTMLInputElement).value })" />
            <div class="flex flex-wrap gap-1">
              <button v-for="q in validationQuick" :key="q.label" class="rounded-full bg-slate-100 px-2 py-0.5 text-[10px] font-semibold text-slate-600 hover:bg-brand-100 hover:text-brand-700" @click="set({ validationMode: 'custom', customValidation: q.expr })">
                {{ q.label }}
              </button>
            </div>
          </div>
        </div>
      </template>

      <!-- DISPLAY -->
      <template v-else-if="kind === 'display'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Message (supports ${templates})</label>
          <textarea :value="config.message" rows="3" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs focus:border-brand-400 focus:outline-none" @input="set({ message: ($event.target as HTMLTextAreaElement).value })" />
          <p class="mt-1 text-[10.5px] text-slate-400">Templates like <code class="font-mono">Hello ${msisdn}</code> are resolved by the VM.</p>
        </div>
      </template>

      <!-- AWAIT EVENT -->
      <template v-else-if="kind === 'await_event'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Event name (exact or prefix)</label>
          <input :value="config.eventName ?? ''" placeholder="payments.confirmed" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ eventName: ($event.target as HTMLInputElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Trigger condition (optional)</label>
          <input :value="config.triggerCondition ?? ''" placeholder="amount > 0" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ triggerCondition: ($event.target as HTMLInputElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Timeout</label>
          <input :value="config.timeout ?? '30m'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ timeout: ($event.target as HTMLInputElement).value })" />
        </div>
      </template>

      <!-- HTTP -->
      <template v-else-if="kind === 'http'">
        <div class="grid grid-cols-[110px_1fr] gap-2">
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Method</label>
            <select :value="config.verb ?? 'post'" class="w-full rounded-lg border border-slate-200 bg-white px-2 py-2 text-xs font-bold" @change="set({ verb: ($event.target as HTMLSelectElement).value })">
              <option v-for="v in ['get', 'post', 'put', 'patch', 'delete']" :key="v" :value="v">{{ v.toUpperCase() }}</option>
            </select>
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">URL (${templates} allowed)</label>
            <input :value="config.url ?? ''" placeholder="https://api.example.com/v1/${msisdn}" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none" @input="set({ url: ($event.target as HTMLInputElement).value })" />
          </div>
        </div>

        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Headers</label>
          <div v-for="h in config.headers ?? []" :key="h.id" class="mb-1.5 flex items-center gap-1.5">
            <input :value="h.key" placeholder="Header" class="w-28 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('headers', h.id, 'key', ($event.target as HTMLInputElement).value)" />
            <input :value="h.value" placeholder="value / $variable" class="min-w-0 flex-1 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('headers', h.id, 'value', ($event.target as HTMLInputElement).value)" />
            <button class="rounded p-1 text-slate-300 hover:text-rose-500" @click="removeRow('headers', h.id)"><XMarkIcon class="h-4 w-4" /></button>
          </div>
          <button class="inline-flex items-center gap-1 rounded-lg bg-slate-100 px-2.5 py-1.5 text-[11px] font-semibold text-slate-600 hover:bg-slate-200" @click="addRow('headers')"><PlusIcon class="h-3.5 w-3.5" /> Add header</button>
        </div>

        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Query params</label>
          <div v-for="q in config.queryParams ?? []" :key="q.id" class="mb-1.5 flex items-center gap-1.5">
            <input :value="q.key" placeholder="param" class="w-28 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('queryParams', q.id, 'key', ($event.target as HTMLInputElement).value)" />
            <input :value="q.value" placeholder="value" class="min-w-0 flex-1 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('queryParams', q.id, 'value', ($event.target as HTMLInputElement).value)" />
            <button class="rounded p-1 text-slate-300 hover:text-rose-500" @click="removeRow('queryParams', q.id)"><XMarkIcon class="h-4 w-4" /></button>
          </div>
          <button class="inline-flex items-center gap-1 rounded-lg bg-slate-100 px-2.5 py-1.5 text-[11px] font-semibold text-slate-600 hover:bg-slate-200" @click="addRow('queryParams')"><PlusIcon class="h-3.5 w-3.5" /> Add param</button>
        </div>

        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Body (JSON, ${templates} allowed)</label>
          <textarea :value="config.bodyJson" rows="5" placeholder='{ "msisdn": "${msisdn}", "amount": "${amount}" }' class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none" @input="set({ bodyJson: ($event.target as HTMLTextAreaElement).value })" />
        </div>

        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Output mapping → context keys</label>
          <div v-for="m in config.outputs ?? []" :key="m.id" class="mb-1.5 flex items-center gap-1.5">
            <input :value="m.key" placeholder="api_response" class="w-32 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('outputs', m.id, 'key', ($event.target as HTMLInputElement).value)" />
            <span class="text-slate-300">=</span>
            <input :value="m.value" placeholder="$.body.data" class="min-w-0 flex-1 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('outputs', m.id, 'value', ($event.target as HTMLInputElement).value)" />
            <button class="rounded p-1 text-slate-300 hover:text-rose-500" @click="removeRow('outputs', m.id)"><XMarkIcon class="h-4 w-4" /></button>
          </div>
          <button class="inline-flex items-center gap-1 rounded-lg bg-slate-100 px-2.5 py-1.5 text-[11px] font-semibold text-slate-600 hover:bg-slate-200" @click="addRow('outputs')"><PlusIcon class="h-3.5 w-3.5" /> Add mapping</button>
          <p class="mt-1 text-[10.5px] text-slate-400">Paths start at the response envelope: <code class="font-mono">$.body.*</code>, <code class="font-mono">$.status</code>.</p>
        </div>

        <div class="rounded-xl border border-slate-100 p-3">
          <label class="flex cursor-pointer items-center gap-2 text-xs font-semibold text-slate-700">
            <input type="checkbox" :checked="config.retryEnabled !== false" class="accent-brand-600" @change="set({ retryEnabled: ($event.target as HTMLInputElement).checked })" />
            Retry policy
          </label>
          <div v-if="config.retryEnabled !== false" class="mt-2 grid grid-cols-2 gap-2">
            <div><label class="text-[10px] font-semibold text-slate-500">Max attempts</label><input type="number" min="1" :value="config.retry?.max_attempts ?? 3" class="w-full rounded-md border border-slate-200 px-2 py-1.5 text-xs" @input="set({ retry: { ...(config.retry ?? {}), max_attempts: +($event.target as HTMLInputElement).value } })" /></div>
            <div><label class="text-[10px] font-semibold text-slate-500">Backoff ms</label><input type="number" min="0" :value="config.retry?.backoff_ms ?? 250" class="w-full rounded-md border border-slate-200 px-2 py-1.5 text-xs" @input="set({ retry: { ...(config.retry ?? {}), backoff_ms: +($event.target as HTMLInputElement).value } })" /></div>
            <div><label class="text-[10px] font-semibold text-slate-500">Multiplier</label><input type="number" step="0.1" min="1" :value="config.retry?.multiplier ?? 2" class="w-full rounded-md border border-slate-200 px-2 py-1.5 text-xs" @input="set({ retry: { ...(config.retry ?? {}), multiplier: +($event.target as HTMLInputElement).value } })" /></div>
            <div><label class="text-[10px] font-semibold text-slate-500">Jitter</label><input type="number" step="0.05" min="0" max="1" :value="config.retry?.jitter ?? 0.2" class="w-full rounded-md border border-slate-200 px-2 py-1.5 text-xs" @input="set({ retry: { ...(config.retry ?? {}), jitter: +($event.target as HTMLInputElement).value } })" /></div>
          </div>
        </div>
      </template>

      <!-- SUBFLOW -->
      <template v-else-if="kind === 'subflow'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Workflow id</label>
          <input :value="config.workflow ?? ''" placeholder="examples.hello_world" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ workflow: ($event.target as HTMLInputElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Inputs (JSON)</label>
          <textarea :value="config.inputsJson" rows="3" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px]" @input="set({ inputsJson: ($event.target as HTMLTextAreaElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Return mapping (JSON)</label>
          <textarea :value="config.returnJson" rows="3" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px]" @input="set({ returnJson: ($event.target as HTMLTextAreaElement).value })" />
        </div>
      </template>

      <!-- EVENT -->
      <template v-else-if="kind === 'event'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Event name</label>
          <input :value="config.eventName ?? ''" placeholder="payments.initiated" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ eventName: ($event.target as HTMLInputElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Payload (JSON)</label>
          <textarea :value="config.payloadJson" rows="4" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px]" @input="set({ payloadJson: ($event.target as HTMLTextAreaElement).value })" />
        </div>
      </template>

      <!-- IF -->
      <template v-else-if="kind === 'if'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Condition (SEL predicate)</label>
          <textarea :value="config.condition ?? ''" rows="2" placeholder="pin != null && len(pin) == 4" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none" @input="set({ condition: ($event.target as HTMLTextAreaElement).value })" />
          <p class="mt-1 text-[10.5px] text-slate-400">No <code class="font-mono">${}</code> wrapper — this is a raw predicate. Functions: <code class="font-mono">len() matches() starts_with() coalesce()</code></p>
        </div>
      </template>

      <!-- CHOICE -->
      <template v-else-if="kind === 'choice'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Expression (selector)</label>
          <input :value="config.expression ?? ''" placeholder="$menu_choice" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ expression: ($event.target as HTMLInputElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Cases</label>
          <div class="space-y-1.5">
            <div v-for="c in config.cases ?? []" :key="c.id" class="flex items-center gap-1.5">
              <span class="text-[10px] font-bold text-slate-400">when</span>
              <input :value="c.value" class="w-16 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-xs" @input="updateRow('cases', c.id, 'value', ($event.target as HTMLInputElement).value)" />
              <input :value="c.label" placeholder="note" class="min-w-0 flex-1 rounded-md border border-slate-200 px-2 py-1.5 text-xs" @input="updateRow('cases', c.id, 'label', ($event.target as HTMLInputElement).value)" />
              <button class="rounded p-1 text-slate-300 hover:text-rose-500" @click="removeRow('cases', c.id)"><XMarkIcon class="h-4 w-4" /></button>
            </div>
          </div>
          <button class="mt-2 inline-flex items-center gap-1 rounded-lg bg-slate-100 px-2.5 py-1.5 text-[11px] font-semibold text-slate-600 hover:bg-slate-200" @click="addRow('cases')"><PlusIcon class="h-3.5 w-3.5" /> Add case</button>
        </div>
      </template>

      <!-- TRY -->
      <template v-else-if="kind === 'try'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Error variable</label>
          <input :value="config.errorVariable ?? 'error'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ errorVariable: ($event.target as HTMLInputElement).value })" />
          <p class="mt-1 text-[10.5px] text-slate-400">The caught error <code class="font-mono">{code, message, node, data}</code> is bound to this variable in the catch branch.</p>
        </div>
      </template>

      <!-- LOOP -->
      <template v-else-if="kind === 'loop'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Loop over</label>
          <div class="grid grid-cols-3 gap-1.5">
            <label v-for="m in ['collection', 'count', 'while']" :key="m" class="cursor-pointer rounded-lg border px-2 py-1.5 text-center text-[11px] font-semibold" :class="(config.loopMode ?? 'collection') === m ? 'border-brand-400 bg-brand-50 text-brand-700' : 'border-slate-200 text-slate-500'">
              <input type="radio" :value="m" :checked="(config.loopMode ?? 'collection') === m" class="hidden" @change="set({ loopMode: m })" />{{ m }}
            </label>
          </div>
          <input
            :value="config[config.loopMode ?? 'collection'] ?? ''" :placeholder="config.loopMode === 'count' ? '5' : config.loopMode === 'while' ? 'attempts < 3' : '$items'"
            class="mt-1.5 w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none"
            @input="set({ [config.loopMode ?? 'collection']: ($event.target as HTMLInputElement).value } as never)"
          />
        </div>
        <div class="grid grid-cols-2 gap-3">
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Iterator</label><input :value="config.iterator ?? 'item'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ iterator: ($event.target as HTMLInputElement).value })" /></div>
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Index var</label><input :value="config.indexVar ?? 'index'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ indexVar: ($event.target as HTMLInputElement).value })" /></div>
        </div>
        <div class="grid grid-cols-2 gap-3">
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Max iterations</label><input type="number" :value="config.maxIterations ?? 1000" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs" @input="set({ maxIterations: +($event.target as HTMLInputElement).value })" /></div>
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">On item error</label><select :value="config.onItemError ?? 'fail'" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-xs" @change="set({ onItemError: ($event.target as HTMLSelectElement).value })"><option value="fail">fail</option><option value="continue">continue</option></select></div>
        </div>
      </template>

      <!-- PARALLEL -->
      <template v-else-if="kind === 'parallel'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Merge policy</label>
          <select :value="config.mergePolicy ?? 'wait_all'" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-xs" @change="set({ mergePolicy: ($event.target as HTMLSelectElement).value })">
            <option value="wait_all">wait_all</option><option value="fail_fast">fail_fast</option><option value="quorum">quorum</option>
          </select>
        </div>
        <label class="flex cursor-pointer items-center gap-2 text-xs font-semibold text-slate-700">
          <input type="checkbox" :checked="config.failFast ?? false" class="accent-brand-600" @change="set({ failFast: ($event.target as HTMLInputElement).checked })" />
          Fail fast when a branch errors
        </label>
      </template>

      <!-- SCRIPT -->
      <template v-else-if="kind === 'script'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">SEL program</label>
          <textarea :value="config.scriptCode" rows="6" placeholder="total = to_number(price) * to_number(qty)" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px] focus:border-brand-400 focus:outline-none" @input="set({ scriptCode: ($event.target as HTMLTextAreaElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Bindings (JSON)</label>
          <textarea :value="config.bindingsJson" rows="3" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px]" @input="set({ bindingsJson: ($event.target as HTMLTextAreaElement).value })" />
        </div>
      </template>

      <!-- BREAK -->
      <template v-else-if="kind === 'break'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Action</label>
          <div class="grid grid-cols-2 gap-1.5">
            <label v-for="a in ['break', 'continue']" :key="a" class="cursor-pointer rounded-lg border px-2 py-1.5 text-center text-[11px] font-semibold" :class="(config.breakAction ?? 'break') === a ? 'border-brand-400 bg-brand-50 text-brand-700' : 'border-slate-200 text-slate-500'">
              <input type="radio" :value="a" :checked="(config.breakAction ?? 'break') === a" class="hidden" @change="set({ breakAction: a })" />{{ a }}
            </label>
          </div>
        </div>
      </template>

      <!-- ASSIGN -->
      <template v-else-if="kind === 'assign'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Variables</label>
          <div v-for="a in config.assignments ?? []" :key="a.id" class="mb-1.5 flex items-center gap-1.5">
            <input :value="a.key" placeholder="key" class="w-28 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('assignments', a.id, 'key', ($event.target as HTMLInputElement).value)" />
            <span class="text-slate-300">=</span>
            <input :value="a.value" placeholder="value / ${template}" class="min-w-0 flex-1 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('assignments', a.id, 'value', ($event.target as HTMLInputElement).value)" />
            <button class="rounded p-1 text-slate-300 hover:text-rose-500" @click="removeRow('assignments', a.id)"><XMarkIcon class="h-4 w-4" /></button>
          </div>
          <button class="inline-flex items-center gap-1 rounded-lg bg-slate-100 px-2.5 py-1.5 text-[11px] font-semibold text-slate-600 hover:bg-slate-200" @click="addRow('assignments')"><PlusIcon class="h-3.5 w-3.5" /> Add variable</button>
        </div>
      </template>

      <!-- TRANSFORM -->
      <template v-else-if="kind === 'transform'">
        <div class="grid grid-cols-2 gap-3">
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Operation</label>
            <select :value="config.operation ?? 'assign'" class="w-full rounded-lg border border-slate-200 bg-white px-3 py-2 text-xs" @change="set({ operation: ($event.target as HTMLSelectElement).value })">
              <option v-for="op in ['assign', 'set', 'copy', 'filter', 'map', 'project', 'merge', 'group', 'sort', 'flatten', 'reduce']" :key="op" :value="op">{{ op }}</option>
            </select>
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Output key</label>
            <input :value="config.outputKey ?? 'result'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ outputKey: ($event.target as HTMLInputElement).value })" />
          </div>
        </div>
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Input expression</label><input :value="config.inputExpr ?? ''" placeholder="$rows" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ inputExpr: ($event.target as HTMLInputElement).value })" /></div>
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Mapping (JSON)</label><textarea :value="config.mappingJson" rows="3" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px]" @input="set({ mappingJson: ($event.target as HTMLTextAreaElement).value })" /></div>
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Where (predicate)</label><input :value="config.where ?? ''" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ where: ($event.target as HTMLInputElement).value })" /></div>
      </template>

      <!-- QUERY -->
      <template v-else-if="kind === 'query'">
        <div class="grid grid-cols-2 gap-3">
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Data source</label><input :value="config.source ?? ''" placeholder="profiles" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ source: ($event.target as HTMLInputElement).value })" /></div>
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Output key</label><input :value="config.outputKey ?? 'rows'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ outputKey: ($event.target as HTMLInputElement).value })" /></div>
        </div>
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Filter (object or predicate)</label><input :value="config.filter ?? ''" placeholder="status == 'active'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ filter: ($event.target as HTMLInputElement).value })" /></div>
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Query statement (SQL etc.)</label><textarea :value="config.queryStatement ?? ''" rows="2" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-[11px]" @input="set({ queryStatement: ($event.target as HTMLTextAreaElement).value })" /></div>
        <div class="grid grid-cols-2 gap-3">
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Limit</label><input type="number" :value="config.limit ?? 0" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs" @input="set({ limit: +($event.target as HTMLInputElement).value })" /></div>
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Offset</label><input type="number" :value="config.offset ?? 0" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs" @input="set({ offset: +($event.target as HTMLInputElement).value })" /></div>
        </div>
      </template>

      <!-- WAIT -->
      <template v-else-if="kind === 'wait'">
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Duration</label><input :value="config.duration ?? '10m'" placeholder="30s / 5m / 1200ms" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ duration: ($event.target as HTMLInputElement).value })" /></div>
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Until (optional predicate)</label><input :value="config.until ?? ''" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ until: ($event.target as HTMLInputElement).value })" /></div>
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Poll interval</label><input :value="config.pollInterval ?? ''" placeholder="5s" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ pollInterval: ($event.target as HTMLInputElement).value })" /></div>
      </template>

      <!-- SCHEDULE -->
      <template v-else-if="kind === 'schedule'">
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Cron (5-field)</label><input :value="config.cron ?? '0 2 * * *'" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs focus:border-brand-400 focus:outline-none" @input="set({ cron: ($event.target as HTMLInputElement).value })" /></div>
        <div class="grid grid-cols-2 gap-3">
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Timezone</label><input :value="config.timezone ?? 'UTC'" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs" @input="set({ timezone: ($event.target as HTMLInputElement).value })" /></div>
          <div><label class="mb-1 block text-xs font-semibold text-slate-600">Job id</label><input :value="config.jobId ?? ''" class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs" @input="set({ jobId: ($event.target as HTMLInputElement).value })" /></div>
        </div>
        <div><label class="mb-1 block text-xs font-semibold text-slate-600">Workflow to start</label><input :value="config.workflow ?? ''" placeholder="examples.nightly_sweep" class="w-full rounded-lg border border-slate-200 px-3 py-2 font-mono text-xs" @input="set({ workflow: ($event.target as HTMLInputElement).value })" /></div>
      </template>

      <!-- TERMINATE -->
      <template v-else-if="kind === 'end_success' || kind === 'end_failure'">
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Final message</label>
          <textarea :value="config.endMessage" rows="3" placeholder="Thank you for banking with us." class="w-full rounded-lg border border-slate-200 px-3 py-2 text-xs focus:border-brand-400 focus:outline-none" @input="set({ endMessage: ($event.target as HTMLTextAreaElement).value })" />
        </div>
        <div>
          <label class="mb-1 block text-xs font-semibold text-slate-600">Output payload</label>
          <div v-for="o in config.endOutputs ?? []" :key="o.id" class="mb-1.5 flex items-center gap-1.5">
            <input :value="o.key" placeholder="key" class="w-28 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('endOutputs', o.id, 'key', ($event.target as HTMLInputElement).value)" />
            <span class="text-slate-300">=</span>
            <input :value="o.value" placeholder="value / $variable" class="min-w-0 flex-1 rounded-md border border-slate-200 px-2 py-1.5 font-mono text-[11px]" @input="updateRow('endOutputs', o.id, 'value', ($event.target as HTMLInputElement).value)" />
            <button class="rounded p-1 text-slate-300 hover:text-rose-500" @click="removeRow('endOutputs', o.id)"><XMarkIcon class="h-4 w-4" /></button>
          </div>
          <button class="inline-flex items-center gap-1 rounded-lg bg-slate-100 px-2.5 py-1.5 text-[11px] font-semibold text-slate-600 hover:bg-slate-200" @click="addRow('endOutputs')"><PlusIcon class="h-3.5 w-3.5" /> Add field</button>
        </div>
      </template>
    </div>

    <!-- outgoing connections -->
    <div v-if="outgoing.length" class="mt-6 border-t border-slate-100 pt-4">
      <div class="mb-2 text-[11px] font-bold uppercase tracking-wider text-slate-400">Connections</div>
      <div v-for="e in outgoing" :key="e.id" class="mb-1.5 flex items-center gap-2 rounded-lg bg-slate-50 px-2.5 py-1.5 text-[11px]">
        <span class="rounded bg-white px-1.5 py-0.5 font-mono text-[9.5px] font-bold text-slate-500 ring-1 ring-slate-200">{{ e.sourceHandle }}</span>
        <span class="text-slate-300">→</span>
        <span class="font-mono font-semibold text-slate-700">{{ e.target }}</span>
        <button class="ml-auto rounded p-0.5 text-slate-300 hover:text-rose-500" @click="ctx.removeEdgeById(e.id)"><XMarkIcon class="h-3.5 w-3.5" /></button>
      </div>
    </div>
  </div>
</template>
