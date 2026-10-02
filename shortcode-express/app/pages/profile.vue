<script setup lang="ts">
import {
  UserCircleIcon, EnvelopeIcon, BuildingOffice2Icon, ShieldCheckIcon, KeyIcon,
  CheckCircleIcon, LockClosedIcon, CheckBadgeIcon, SignalIcon, BoltIcon,
} from '@heroicons/vue/24/outline'
import type { User } from '~/../shared/types'

definePageMeta({ layout: 'default' })
useHead({ title: 'Your profile · ShortCodeExpress' })

const me = ref<User | null>(null)
await (async () => { me.value = await useRequestFetch()<User>('/api/auth/me') })()

// --- profile editing ---------------------------------------------------------
const form = reactive({ name: me.value?.name ?? '', title: me.value?.title ?? '', company: me.value?.company ?? '' })
const savingProfile = ref(false)
const profileMsg = ref('')
const profileErr = ref('')

async function saveProfile() {
  profileErr.value = ''; profileMsg.value = ''
  savingProfile.value = true
  try {
    me.value = await $fetch<User>('/api/account/profile', { method: 'PATCH', body: { ...form } })
    profileMsg.value = 'Profile updated'
    setTimeout(() => (profileMsg.value = ''), 2500)
  } catch (e: unknown) {
    profileErr.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not save your profile'
  } finally { savingProfile.value = false }
}

// --- password reset (ERP) ------------------------------------------------------
const pw = reactive({ currentPassword: '', newPassword: '', confirm: '' })
const changingPw = ref(false)
const pwMsg = ref('')
const pwErr = ref('')
const showPw = ref(false)

const pwMismatch = computed(() => !!pw.confirm && pw.newPassword !== pw.confirm)
const canChangePw = computed(() => !!pw.currentPassword && pw.newPassword.length >= 6 && !pwMismatch.value)

async function changePassword() {
  pwErr.value = ''; pwMsg.value = ''
  changingPw.value = true
  try {
    await $fetch('/api/account/password', {
      method: 'POST',
      body: { currentPassword: pw.currentPassword, newPassword: pw.newPassword },
    })
    pwMsg.value = 'Password changed — use it next time you sign in'
    pw.currentPassword = ''; pw.newPassword = ''; pw.confirm = ''
  } catch (e: unknown) {
    pwErr.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not change your password'
  } finally { changingPw.value = false }
}

const memberSince = computed(() => me.value?.createdAt
  ? new Date(me.value.createdAt).toLocaleDateString('en-GB', { month: 'long', year: 'numeric' })
  : '—')
const tokenExpiry = computed(() => me.value?.erp?.expiresAt
  ? new Date(me.value.erp.expiresAt).toLocaleString('en-GB', { dateStyle: 'medium', timeStyle: 'short' })
  : null)
const initials = computed(() => (me.value?.name ?? '?').split(' ').map((n) => n[0]).join('').slice(0, 2).toUpperCase())
</script>

<template>
  <div class="mx-auto max-w-4xl p-8">
    <div class="mb-8">
      <h1 class="text-2xl font-bold text-slate-900">Your profile</h1>
      <p class="mt-1 text-sm text-slate-500">Who you are across this workspace — and the ERP account behind it.</p>
    </div>

    <div v-if="me" class="space-y-5">
      <!-- identity card -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
        <div class="flex flex-wrap items-center gap-4">
          <div class="flex h-16 w-16 items-center justify-center rounded-2xl text-xl font-black text-white" :style="{ background: `hsl(${me.avatarHue} 65% 45%)` }">
            {{ initials }}
          </div>
          <div class="min-w-0 flex-1">
            <div class="flex flex-wrap items-center gap-2">
              <h2 class="text-lg font-bold text-slate-900">{{ me.name }}</h2>
              <span class="rounded-full bg-brand-50 px-2 py-0.5 text-[10px] font-bold uppercase tracking-wide text-brand-700">{{ me.role }}</span>
              <span v-if="me.useCase === 'aggregator'" class="rounded-full bg-sky-50 px-2 py-0.5 text-[10px] font-bold uppercase tracking-wide text-sky-700">Aggregator</span>
              <span v-else class="rounded-full bg-success-50 px-2 py-0.5 text-[10px] font-bold uppercase tracking-wide text-success-700">Merchant</span>
            </div>
            <div class="mt-0.5 flex flex-wrap items-center gap-x-4 gap-y-1 text-xs text-slate-500">
              <span class="flex items-center gap-1"><EnvelopeIcon class="h-3.5 w-3.5" /> {{ me.email }}</span>
              <span class="flex items-center gap-1"><BuildingOffice2Icon class="h-3.5 w-3.5" /> {{ me.company || '—' }}</span>
              <span>member since {{ memberSince }}</span>
            </div>
          </div>
        </div>
      </div>

      <div class="grid gap-5 lg:grid-cols-2">
        <!-- editable profile -->
        <form class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200" @submit.prevent="saveProfile">
          <div class="mb-4 flex items-center gap-2">
            <UserCircleIcon class="h-5 w-5 text-brand-600" />
            <h3 class="text-sm font-bold text-slate-900">Profile details</h3>
          </div>
          <div class="space-y-3.5">
            <label class="block">
              <span class="mb-1 block text-xs font-semibold text-slate-600">Display name</span>
              <input v-model="form.name" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
            </label>
            <label class="block">
              <span class="mb-1 block text-xs font-semibold text-slate-600">Job title</span>
              <input v-model="form.title" placeholder="Integration Engineer" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
            </label>
            <label class="block">
              <span class="mb-1 block text-xs font-semibold text-slate-600">Company</span>
              <input v-model="form.company" placeholder="Cedi Save Ltd" class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
            </label>
            <div class="rounded-xl bg-slate-50 px-3.5 py-2.5 ring-1 ring-slate-100">
              <span class="block text-xs font-semibold text-slate-600">Email</span>
              <div class="mt-0.5 flex items-center justify-between gap-2">
                <span class="truncate text-sm text-slate-500">{{ me.email }}</span>
                <span class="flex shrink-0 items-center gap-1 text-[10px] font-bold uppercase tracking-wide text-slate-400"><LockClosedIcon class="h-3 w-3" /> managed by ERP</span>
              </div>
            </div>
          </div>
          <p v-if="profileErr" class="mt-3 rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700">{{ profileErr }}</p>
          <p v-if="profileMsg" class="mt-3 flex items-center gap-1.5 rounded-lg bg-success-50 px-3 py-2 text-xs font-semibold text-success-700"><CheckCircleIcon class="h-4 w-4" /> {{ profileMsg }}</p>
          <button type="submit" :disabled="savingProfile || form.name.trim().length < 2"
            class="mt-4 w-full rounded-xl bg-brand-600 px-4 py-2.5 text-sm font-bold text-white shadow-lg shadow-brand-200 transition hover:bg-brand-500 disabled:opacity-50">
            {{ savingProfile ? 'Saving…' : 'Save profile' }}
          </button>
        </form>

        <div class="space-y-5">
          <!-- password reset -->
          <form class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200" @submit.prevent="changePassword">
            <div class="mb-4 flex items-center justify-between gap-2">
              <div class="flex items-center gap-2">
                <KeyIcon class="h-5 w-5 text-brand-600" />
                <h3 class="text-sm font-bold text-slate-900">Password</h3>
              </div>
              <button type="button" class="text-[11px] font-semibold text-slate-400 hover:text-slate-600" @click="showPw = !showPw">
                {{ showPw ? 'Hide' : 'Show' }}
              </button>
            </div>
            <div class="space-y-3.5">
              <label class="block">
                <span class="mb-1 block text-xs font-semibold text-slate-600">Current password</span>
                <input v-model="pw.currentPassword" :type="showPw ? 'text' : 'password'" autocomplete="current-password"
                  class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
              </label>
              <label class="block">
                <span class="mb-1 block text-xs font-semibold text-slate-600">New password</span>
                <input v-model="pw.newPassword" :type="showPw ? 'text' : 'password'" autocomplete="new-password" placeholder="At least 6 characters"
                  class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
              </label>
              <label class="block">
                <span class="mb-1 block text-xs font-semibold text-slate-600">Confirm new password</span>
                <input v-model="pw.confirm" :type="showPw ? 'text' : 'password'" autocomplete="new-password"
                  :class="pwMismatch ? 'border-rose-300' : 'border-slate-200'"
                  class="w-full rounded-xl border px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
              </label>
            </div>
            <p v-if="pwMismatch" class="mt-2 text-xs font-medium text-rose-600">Passwords don't match</p>
            <p v-if="pwErr" class="mt-3 rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700">{{ pwErr }}</p>
            <p v-if="pwMsg" class="mt-3 flex items-center gap-1.5 rounded-lg bg-success-50 px-3 py-2 text-xs font-semibold text-success-700"><CheckCircleIcon class="h-4 w-4" /> {{ pwMsg }}</p>
            <button type="submit" :disabled="changingPw || !canChangePw"
              class="mt-4 flex w-full items-center justify-center gap-2 rounded-xl bg-slate-900 px-4 py-2.5 text-sm font-bold text-white transition hover:bg-slate-800 disabled:opacity-50">
              <svg v-if="changingPw" class="h-4 w-4 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
              <ShieldCheckIcon v-else class="h-4 w-4" />
              {{ changingPw ? 'Changing…' : 'Reset password' }}
            </button>
            <p class="mt-2.5 text-center text-[10.5px] text-slate-400">Resets your Rhyolite Prime ERP password — the one you sign in with.</p>
          </form>

          <!-- ERP account info -->
          <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200">
            <div class="mb-4 flex items-center gap-2">
              <CheckBadgeIcon class="h-5 w-5 text-brand-600" />
              <h3 class="text-sm font-bold text-slate-900">ERP account</h3>
            </div>
            <dl class="space-y-2.5 text-xs" v-if="me.erp">
              <div class="flex items-center justify-between gap-3">
                <dt class="text-slate-500">Tenant / workspace</dt>
                <dd class="truncate font-bold text-slate-800">{{ me.erp.tenant || me.company || '—' }}</dd>
              </div>
              <div class="flex items-center justify-between gap-3">
                <dt class="text-slate-500">ERP user ID</dt>
                <dd class="font-mono font-bold text-slate-800">#{{ me.erp.userId }}</dd>
              </div>
              <div class="flex items-center justify-between gap-3">
                <dt class="text-slate-500">Access token expires</dt>
                <dd class="font-bold text-slate-800">{{ tokenExpiry ?? '—' }}</dd>
              </div>
            </dl>
            <p v-else class="rounded-lg bg-amber-50 px-3 py-2.5 text-[11px] leading-relaxed text-amber-700 ring-1 ring-amber-100">
              This account has no ERP session yet (demo sign-in). Sign out and back in with your Rhyolite Prime credentials to link it — that's what enables password reset here.
            </p>
          </div>
        </div>
      </div>

      <!-- shortcuts -->
      <div class="grid gap-3 sm:grid-cols-2">
        <NuxtLink to="/developers" class="flex items-center gap-3 rounded-xl bg-white p-4 ring-1 ring-slate-200 transition hover:ring-brand-300">
          <BoltIcon class="h-5 w-5 text-brand-600" />
          <div>
            <div class="text-sm font-bold text-slate-800">API keys &amp; webhooks</div>
            <div class="text-xs text-slate-500">Your sk_live_ keys and webhook endpoints</div>
          </div>
        </NuxtLink>
        <NuxtLink to="/settings" class="flex items-center gap-3 rounded-xl bg-white p-4 ring-1 ring-slate-200 transition hover:ring-brand-300">
          <SignalIcon class="h-5 w-5 text-brand-600" />
          <div>
            <div class="text-sm font-bold text-slate-800">Workspace settings</div>
            <div class="text-xs text-slate-500">Defaults, use case and builder preferences</div>
          </div>
        </NuxtLink>
      </div>
    </div>
  </div>
</template>
