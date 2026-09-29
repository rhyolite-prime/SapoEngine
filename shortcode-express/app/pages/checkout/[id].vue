<script setup lang="ts">
import { ShieldCheckIcon, DevicePhoneMobileIcon, CreditCardIcon, CheckCircleIcon, XCircleIcon, SignalIcon, ArrowPathIcon } from '@heroicons/vue/24/outline'
import type { CheckoutSession } from '~/../shared/types'

definePageMeta({ middleware: 'auth' })

const route = useRoute()
const co = ref<CheckoutSession | null>(null)
const loadError = ref('')
const method = ref<'momo' | 'card'>('momo')
const momoNetwork = ref('MTN')
const phone = ref('')
const cardNumber = ref('')
const cardExp = ref('')
const cardCvc = ref('')

// payment lifecycle
const phase = ref<'form' | 'processing' | 'success' | 'failed'>('form')
const payError = ref('')
const result = ref<Record<string, unknown> | null>(null)
const processingNote = ref('')

async function load() {
  try {
    co.value = await $fetch<CheckoutSession>(`/api/checkout/${route.params.id}`)
  } catch (e: unknown) {
    loadError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Checkout not found'
  }
}
await load()
useHead({ title: co.value ? `Pay GHS ${co.value.total} · ShortCodeExpress Checkout` : 'Checkout · ShortCodeExpress' })

const processingNotesMomo = [
  'Sending the MoMo prompt…',
  'Check your phone — approve the prompt',
  'Confirming with the mobile money rail…',
]
let noteTimer: ReturnType<typeof setInterval> | undefined

async function pay() {
  if (!co.value) return
  payError.value = ''
  phase.value = 'processing'
  let i = 0
  processingNote.value = method.value === 'momo' ? processingNotesMomo[0] : 'Authorising card…'
  clearInterval(noteTimer)
  if (method.value === 'momo') {
    noteTimer = setInterval(() => { i = Math.min(i + 1, processingNotesMomo.length - 1); processingNote.value = processingNotesMomo[i] }, 1300)
  }
  try {
    const res = await $fetch<{ result: Record<string, unknown> }>(`/api/checkout/${co.value.id}/pay`, {
      method: 'POST',
      body: {
        method: method.value,
        momoNetwork: momoNetwork.value,
        phone: phone.value,
        cardNumber: cardNumber.value,
      },
    })
    result.value = res.result
    phase.value = 'success'
  } catch (e: unknown) {
    payError.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Payment failed'
    phase.value = 'failed'
  } finally {
    clearInterval(noteTimer)
  }
}

function retry() { phase.value = 'form'; payError.value = '' }

const ghs = (n: number) => `GHS ${n.toLocaleString('en-GH', { minimumFractionDigits: 2 })}`
const boughtCode = computed(() => String(co.value?.meta.code ?? ''))
const resultShortcode = computed(() => (result.value?.shortcode ?? null) as Record<string, unknown> | null)

onUnmounted(() => clearInterval(noteTimer))
</script>

<template>
  <div class="flex min-h-screen items-center justify-center bg-slate-100 px-4 py-10">
    <div class="w-full max-w-4xl">
      <!-- header -->
      <div class="mb-6 flex items-center justify-center gap-3">
        <div class="flex h-10 w-10 items-center justify-center rounded-xl bg-gradient-to-br from-brand-500 to-fuchsia-600 shadow-lg shadow-brand-200">
          <svg viewBox="0 0 24 24" class="h-6 w-6 text-white" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 7h16M4 12h10M4 17h7" /><circle cx="18.5" cy="15.5" r="2.5" /></svg>
        </div>
        <div>
          <div class="text-lg font-bold text-slate-900">ShortCodeExpress <span class="text-slate-400">Checkout</span></div>
          <div class="flex items-center gap-1 text-[11px] text-slate-500"><ShieldCheckIcon class="h-3.5 w-3.5 text-emerald-500" /> PCI-DSS sandbox · secured payment session</div>
        </div>
      </div>

      <div v-if="loadError" class="rounded-2xl bg-white p-10 text-center shadow-sm ring-1 ring-slate-200">
        <XCircleIcon class="mx-auto h-10 w-10 text-rose-400" />
        <p class="mt-3 font-semibold text-slate-800">{{ loadError }}</p>
        <NuxtLink to="/onboarding" class="mt-4 inline-block rounded-lg bg-brand-600 px-4 py-2 text-sm font-semibold text-white hover:bg-brand-500">Back to onboarding</NuxtLink>
      </div>

      <div v-else-if="co" class="grid gap-6 lg:grid-cols-[1fr_400px]">
        <!-- order summary -->
        <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
          <h2 class="text-sm font-bold uppercase tracking-wide text-slate-400">Order summary</h2>
          <div class="mt-4 space-y-3">
            <div v-for="(item, i) in co.items" :key="i" class="flex items-start justify-between gap-4 border-b border-slate-100 pb-3">
              <div>
                <div class="text-sm font-semibold text-slate-800">{{ item.label }}</div>
                <div v-if="item.detail" class="text-xs text-slate-500">{{ item.detail }}</div>
              </div>
              <div class="whitespace-nowrap text-sm font-bold text-slate-900">{{ ghs(item.amount) }}</div>
            </div>
          </div>
          <div class="mt-4 flex items-center justify-between">
            <span class="text-sm font-bold text-slate-900">Total due today</span>
            <span class="text-2xl font-extrabold text-slate-900">{{ ghs(co.total) }}</span>
          </div>
          <div v-if="co.kind === 'shortcode'" class="mt-5 rounded-xl bg-brand-50 p-4 ring-1 ring-brand-100">
            <div class="flex items-center gap-2 text-xs font-bold uppercase tracking-wide text-brand-700">
              <SignalIcon class="h-4 w-4" /> Included instantly after payment
            </div>
            <ul class="mt-2 space-y-1 text-sm text-slate-700">
              <li>· Short code <span class="font-mono font-bold">{{ boughtCode }}</span> live on the network</li>
              <li>· Starter USSD flow built &amp; released (dynamic menus + payment webhook)</li>
              <li>· Session quota activated — dial it the moment you're done here</li>
            </ul>
          </div>
        </div>

        <!-- payment -->
        <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
          <!-- FORM -->
          <template v-if="phase === 'form'">
            <div class="mb-4 grid grid-cols-2 gap-2 rounded-xl bg-slate-100 p-1">
              <button class="flex items-center justify-center gap-2 rounded-lg px-3 py-2 text-sm font-semibold transition"
                :class="method === 'momo' ? 'bg-white text-slate-900 shadow' : 'text-slate-500'" @click="method = 'momo'">
                <DevicePhoneMobileIcon class="h-4 w-4" /> Mobile Money
              </button>
              <button class="flex items-center justify-center gap-2 rounded-lg px-3 py-2 text-sm font-semibold transition"
                :class="method === 'card' ? 'bg-white text-slate-900 shadow' : 'text-slate-500'" @click="method = 'card'">
                <CreditCardIcon class="h-4 w-4" /> Card
              </button>
            </div>

            <div v-if="method === 'momo'" class="space-y-4">
              <div>
                <span class="mb-1.5 block text-xs font-semibold text-slate-600">Network</span>
                <div class="grid grid-cols-3 gap-2">
                  <button v-for="n in ['MTN', 'Vodafone', 'AirtelTigo']" :key="n"
                    class="rounded-xl border-2 px-2 py-2.5 text-sm font-semibold transition"
                    :class="momoNetwork === n ? 'border-brand-500 bg-brand-50 text-brand-700' : 'border-slate-200 text-slate-500 hover:border-slate-300'"
                    @click="momoNetwork = n">{{ n === 'AirtelTigo' ? 'AT' : n }}</button>
                </div>
              </div>
              <label class="block">
                <span class="mb-1.5 block text-xs font-semibold text-slate-600">MoMo number</span>
                <input v-model="phone" placeholder="024 123 4567" inputmode="tel"
                  class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
                <span class="mt-1.5 block text-[11px] text-slate-400">You'll get a prompt on this number. Sandbox: any number works, ones ending 000 decline.</span>
              </label>
            </div>

            <div v-else class="space-y-4">
              <label class="block">
                <span class="mb-1.5 block text-xs font-semibold text-slate-600">Card number</span>
                <input v-model="cardNumber" placeholder="4242 4242 4242 4242" inputmode="numeric"
                  class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 font-mono text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
              </label>
              <div class="grid grid-cols-2 gap-3">
                <label class="block">
                  <span class="mb-1.5 block text-xs font-semibold text-slate-600">Expiry</span>
                  <input v-model="cardExp" placeholder="12/28" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 font-mono text-sm outline-none focus:border-brand-500" />
                </label>
                <label class="block">
                  <span class="mb-1.5 block text-xs font-semibold text-slate-600">CVC</span>
                  <input v-model="cardCvc" placeholder="123" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 font-mono text-sm outline-none focus:border-brand-500" />
                </label>
              </div>
              <span class="block text-[11px] text-slate-400">Sandbox cards: any valid-length number approves; cards ending 0000 decline.</span>
            </div>

            <button :disabled="co.status !== 'pending'"
              class="mt-6 flex w-full items-center justify-center gap-2 rounded-xl bg-slate-900 px-4 py-3.5 text-sm font-bold text-white shadow-lg transition hover:bg-slate-800 disabled:opacity-50"
              @click="pay">
              <ShieldCheckIcon class="h-4 w-4" /> Pay {{ ghs(co.total) }}
            </button>
            <p v-if="co.status !== 'pending'" class="mt-3 text-center text-xs font-semibold text-amber-600">This checkout is {{ co.status }}.</p>
          </template>

          <!-- PROCESSING -->
          <div v-else-if="phase === 'processing'" class="py-8 text-center">
            <div class="relative mx-auto h-16 w-16">
              <div class="absolute inset-0 animate-ping rounded-full bg-brand-400/30"></div>
              <div class="absolute inset-0 flex items-center justify-center">
                <svg class="h-10 w-10 animate-spin text-brand-600" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
              </div>
            </div>
            <p class="mt-6 font-semibold text-slate-800">{{ processingNote }}</p>
            <p v-if="method === 'momo'" class="mt-1 text-sm text-slate-500">Enter your MoMo PIN on the prompt to authorise {{ ghs(co.total) }}.</p>
            <p v-else class="mt-1 text-sm text-slate-500">3-D Secure…</p>
          </div>

          <!-- SUCCESS -->
          <div v-else-if="phase === 'success'" class="py-4 text-center">
            <CheckCircleIcon class="mx-auto h-14 w-14 text-emerald-500" />
            <h3 class="mt-3 text-lg font-extrabold text-slate-900">Payment received!</h3>
            <p class="mt-1 text-sm text-slate-500">{{ ghs(co.total) }} · invoice issued · webhook fired</p>

            <div v-if="co.kind === 'shortcode' && resultShortcode" class="mt-5 rounded-2xl bg-gradient-to-br from-brand-600 to-fuchsia-600 p-5 text-left text-white shadow-xl">
              <div class="text-[11px] font-bold uppercase tracking-widest text-white/70">Your short code is LIVE</div>
              <div class="mt-1 font-mono text-3xl font-extrabold tracking-wide">{{ resultShortcode.code }}</div>
              <div class="mt-2 text-sm text-white/80">
                Starter flow released ({{ (result?.flow as Record<string, unknown>)?.release }}) ·
                {{ Number(resultShortcode.quota ?? 0).toLocaleString() }} sessions ready
              </div>
            </div>
            <div v-else-if="resultShortcode" class="mt-5 rounded-2xl bg-emerald-50 p-5 text-left ring-1 ring-emerald-100">
              <div class="text-sm font-bold text-emerald-800">{{ resultShortcode.code }} topped up</div>
              <div class="text-sm text-emerald-700">+{{ Number(resultShortcode.added ?? 0).toLocaleString() }} sessions → {{ Number(resultShortcode.quota ?? 0).toLocaleString() }} total</div>
            </div>

            <div class="mt-6 space-y-2">
              <NuxtLink v-if="co.kind === 'shortcode'" :to="`/dial?code=${resultShortcode?.code ?? ''}`"
                class="flex items-center justify-center gap-2 rounded-xl bg-brand-600 px-4 py-3 text-sm font-bold text-white shadow-lg shadow-brand-200 hover:bg-brand-500">
                <DevicePhoneMobileIcon class="h-4 w-4" /> Dial {{ resultShortcode?.code }} now — it's live
              </NuxtLink>
              <NuxtLink v-else to="/shortcodes" class="flex items-center justify-center gap-2 rounded-xl bg-brand-600 px-4 py-3 text-sm font-bold text-white shadow-lg shadow-brand-200 hover:bg-brand-500">
                Back to short codes
              </NuxtLink>
              <NuxtLink :to="co.kind === 'shortcode' ? '/onboarding' : '/billing'" class="block text-center text-xs font-semibold text-slate-500 hover:text-slate-800">
                {{ co.kind === 'shortcode' ? 'Continue onboarding' : 'View invoices' }}
              </NuxtLink>
            </div>
          </div>

          <!-- FAILED -->
          <div v-else class="py-6 text-center">
            <XCircleIcon class="mx-auto h-12 w-12 text-rose-400" />
            <h3 class="mt-3 font-bold text-slate-900">Payment didn't go through</h3>
            <p class="mt-1 text-sm text-slate-500">{{ payError }}</p>
            <button class="mt-5 inline-flex items-center gap-2 rounded-xl bg-slate-900 px-5 py-2.5 text-sm font-bold text-white hover:bg-slate-800" @click="retry">
              <ArrowPathIcon class="h-4 w-4" /> Try another way
            </button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
