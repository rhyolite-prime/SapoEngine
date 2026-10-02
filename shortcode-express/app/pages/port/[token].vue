<script setup lang="ts">
definePageMeta({ layout: 'public' })
import {
  SignalIcon, ShieldCheckIcon, CheckCircleIcon, XCircleIcon, ArrowsRightLeftIcon,
  BuildingOffice2Icon, ClockIcon, ArrowPathIcon, DocumentDuplicateIcon,
} from '@heroicons/vue/24/outline'

// Public, token-gated page — this is the "interaction URL" a porting customer
// shares with their current short code provider. The provider reviews the
// port request and releases (or declines) the code here. No login required:
// the 24-character token in the URL is the authorization.
interface PortInfo {
  code: string
  label: string
  network: string
  donorProvider: string
  customer: { name: string; company: string | null }
  flatMonthly: number | null
  requestedAt: string
  status: 'pending' | 'approved' | 'rejected'
  approvedAt: string | null
  rejectedAt: string | null
  rejectedReason: string | null
}

const route = useRoute()
const info = ref<PortInfo | null>(null)
const loadError = ref('')
const acting = ref(false)
const actionError = ref('')
const rejectMode = ref(false)
const reason = ref('')
const copied = ref(false)

async function load() {
  try {
    info.value = await $fetch<PortInfo>(`/api/port/${route.params.token}`)
  } catch (e: unknown) {
    loadError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Porting link not found'
  }
}
await load()
useHead({ title: info.value ? `Porting request · ${info.value.code}` : 'Porting request · ShortCodeExpress' })

const requested = computed(() => {
  if (!info.value) return ''
  return new Date(info.value.requestedAt).toLocaleString('en-GB', { dateStyle: 'medium', timeStyle: 'short' })
})

async function act(action: 'approve' | 'reject') {
  if (!info.value) return
  acting.value = true
  actionError.value = ''
  try {
    await $fetch(`/api/port/${route.params.token}`, { method: 'POST', body: { action, reason: reason.value } })
    await load()
    rejectMode.value = false
  } catch (e: unknown) {
    actionError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Something went wrong'
  } finally { acting.value = false }
}

async function copyUrl() {
  try { await navigator.clipboard.writeText(window.location.href); copied.value = true; setTimeout(() => (copied.value = false), 1500) } catch {}
}
</script>

<template>
  <div class="flex min-h-screen items-center justify-center bg-slate-100 px-4 py-10">
    <div class="w-full max-w-2xl">
      <!-- header -->
      <div class="mb-6 flex items-center justify-center gap-3">
        <div class="flex h-10 w-10 items-center justify-center rounded-xl bg-gradient-to-br from-brand-600 to-brand-400 shadow-lg shadow-brand-200">
          <SignalIcon class="h-6 w-6 text-white" />
        </div>
        <div>
          <div class="text-lg font-bold text-slate-900">ShortCodeExpress <span class="text-slate-400">Porting Desk</span></div>
          <div class="flex items-center gap-1 text-[11px] text-slate-500"><ShieldCheckIcon class="h-3.5 w-3.5 text-success-500" /> Secure provider interaction link</div>
        </div>
      </div>

      <!-- invalid link -->
      <div v-if="loadError" class="rounded-2xl bg-white p-10 text-center shadow-sm ring-1 ring-slate-200">
        <XCircleIcon class="mx-auto h-10 w-10 text-rose-400" />
        <p class="mt-3 font-semibold text-slate-800">{{ loadError }}</p>
        <p class="mt-1 text-sm text-slate-500">Ask the customer to re-share their porting link from the ShortCodeExpress dashboard.</p>
      </div>

      <template v-else-if="info">
        <!-- request card -->
        <div class="overflow-hidden rounded-2xl bg-white shadow-sm ring-1 ring-slate-200">
          <div class="border-b border-slate-100 bg-slate-50/70 px-6 py-4">
            <div class="flex flex-wrap items-center justify-between gap-3">
              <div class="flex items-center gap-2 text-[11px] font-bold uppercase tracking-widest text-slate-500">
                <ArrowsRightLeftIcon class="h-4 w-4 text-brand-600" /> Short code porting request
              </div>
              <span class="flex items-center gap-1.5 text-[11px] text-slate-400"><ClockIcon class="h-3.5 w-3.5" /> {{ requested }}</span>
            </div>
            <div class="mt-2 flex flex-wrap items-center gap-3">
              <span class="font-mono text-3xl font-extrabold tracking-wide text-slate-900">{{ info.code }}</span>
              <span class="rounded-full bg-brand-50 px-2.5 py-1 text-[11px] font-bold text-brand-700 ring-1 ring-brand-100">{{ info.network }}</span>
              <span class="text-sm text-slate-500">{{ info.label }}</span>
            </div>
          </div>

          <div class="space-y-4 px-6 py-5">
            <!-- who is asking -->
            <div class="grid gap-3 sm:grid-cols-2">
              <div class="rounded-xl bg-slate-50 p-4 ring-1 ring-slate-100">
                <div class="flex items-center gap-1.5 text-[10px] font-bold uppercase tracking-wider text-slate-400"><BuildingOffice2Icon class="h-3.5 w-3.5" /> Current provider (you)</div>
                <div class="mt-1 text-sm font-bold text-slate-800">{{ info.donorProvider }}</div>
                <div class="text-xs text-slate-500">holds routing for {{ info.code }} today</div>
              </div>
              <div class="rounded-xl bg-brand-50/60 p-4 ring-1 ring-brand-100">
                <div class="flex items-center gap-1.5 text-[10px] font-bold uppercase tracking-wider text-brand-500"><BuildingOffice2Icon class="h-3.5 w-3.5" /> Requested by</div>
                <div class="mt-1 text-sm font-bold text-slate-800">{{ info.customer.name }}</div>
                <div class="text-xs text-slate-500">{{ info.customer.company ?? 'moving to ShortCodeExpress' }}</div>
              </div>
            </div>

            <!-- what approving means -->
            <div class="rounded-xl bg-white p-4 text-sm leading-relaxed text-slate-600 ring-1 ring-slate-200">
              <b class="text-slate-800">What this means for {{ info.donorProvider }}:</b>
              approving releases <span class="font-mono font-bold">{{ info.code }}</span> to ShortCodeExpress.
              The customer's service ({{ info.label }}) continues under their own USSD flows on the ShortCodeExpress platform —
              {{ info.flatMonthly ? `billed to them at a flat GHS ${info.flatMonthly}/month with unmetered sessions` : 'flat-rated, unmetered sessions' }}.
              No further action is needed from you once approved.
            </div>

            <!-- PENDING: decision -->
            <template v-if="info.status === 'pending'">
              <div v-if="!rejectMode" class="flex flex-wrap gap-3">
                <button :disabled="acting"
                  class="flex flex-1 items-center justify-center gap-2 rounded-xl bg-success-600 px-5 py-3.5 text-sm font-bold text-white shadow-lg shadow-success-100 hover:bg-success-500 disabled:opacity-50"
                  @click="act('approve')">
                  <svg v-if="acting" class="h-4 w-4 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
                  <CheckCircleIcon v-else class="h-4 w-4" />
                  Approve &amp; release {{ info.code }}
                </button>
                <button :disabled="acting"
                  class="rounded-xl bg-white px-5 py-3.5 text-sm font-bold text-slate-600 ring-1 ring-slate-200 hover:bg-slate-50 disabled:opacity-50"
                  @click="rejectMode = true">
                  Decline
                </button>
              </div>

              <!-- reject flow -->
              <div v-else class="rounded-xl bg-rose-50 p-4 ring-1 ring-rose-100">
                <label class="text-xs font-bold text-rose-700">Reason for declining (shared with the customer)</label>
                <input v-model="reason" placeholder="e.g. Outstanding balance on the code" maxlength="200"
                  class="mt-1.5 w-full rounded-lg border border-rose-200 bg-white px-3 py-2 text-sm outline-none focus:border-rose-400" />
                <div class="mt-3 flex gap-2">
                  <button :disabled="acting" class="rounded-lg bg-rose-600 px-4 py-2 text-xs font-bold text-white hover:bg-rose-500 disabled:opacity-50" @click="act('reject')">
                    Send decline
                  </button>
                  <button class="rounded-lg bg-white px-4 py-2 text-xs font-bold text-slate-600 ring-1 ring-slate-200 hover:bg-slate-50" @click="rejectMode = false">Cancel</button>
                </div>
              </div>
            </template>

            <!-- APPROVED -->
            <div v-else-if="info.status === 'approved'" class="flex items-start gap-3 rounded-xl bg-success-50 p-4 ring-1 ring-success-100">
              <CheckCircleIcon class="mt-0.5 h-6 w-6 shrink-0 text-success-600" />
              <div>
                <div class="text-sm font-bold text-success-800">Released — thank you!</div>
                <p class="mt-0.5 text-xs leading-relaxed text-success-700">
                  <span class="font-mono font-bold">{{ info.code }}</span> now routes to ShortCodeExpress. The customer has been notified automatically
                  and their service is live. You can close this page.
                </p>
              </div>
            </div>

            <!-- REJECTED -->
            <div v-else class="flex items-start gap-3 rounded-xl bg-rose-50 p-4 ring-1 ring-rose-100">
              <XCircleIcon class="mt-0.5 h-6 w-6 shrink-0 text-rose-500" />
              <div>
                <div class="text-sm font-bold text-rose-700">Port declined</div>
                <p class="mt-0.5 text-xs leading-relaxed text-rose-600">Reason: {{ info.rejectedReason ?? 'not given' }}. The customer has been notified and can follow up with you directly.</p>
              </div>
            </div>

            <p v-if="actionError" class="rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700">{{ actionError }}</p>
          </div>

          <div class="flex items-center justify-between gap-3 border-t border-slate-100 bg-slate-50/70 px-6 py-3">
            <p class="flex items-center gap-1.5 text-[10.5px] text-slate-400">
              <ShieldCheckIcon class="h-3.5 w-3.5" /> This private link authorises this single port request only.
            </p>
            <button class="flex shrink-0 items-center gap-1 text-[10.5px] font-semibold text-slate-400 hover:text-slate-600" @click="copyUrl">
              <DocumentDuplicateIcon class="h-3.5 w-3.5" /> {{ copied ? 'Copied!' : 'Copy link' }}
            </button>
          </div>
        </div>

        <p class="mt-4 text-center text-[11px] text-slate-400">
          <ArrowPathIcon class="mr-1 inline h-3 w-3" />Requests don't expire — approve when your internal release process allows.
        </p>
      </template>
    </div>
  </div>
</template>
