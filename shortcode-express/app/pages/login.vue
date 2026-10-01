<script setup lang="ts">
import { ShieldCheckIcon, ArrowLeftIcon, BuildingOffice2Icon, UserIcon, LockClosedIcon } from '@heroicons/vue/24/outline'
import type { User } from '~/../shared/types'

definePageMeta({ layout: 'default' })
useHead({ title: 'Sign in · ShortCodeExpress' })

const route = useRoute()
const { login: demoLogin, saveBusinessIdentity } = useAuth()

// --- ERP credential sign-in ------------------------------------------------
const form = reactive({ accountName: '', userNameOrEmailAddress: '', password: '' })
const busy = ref(false)
const error = ref('')

// --- Google Authenticator (two-factor) step ----------------------------------
const twoFactor = ref(false)
const twoFactorMessage = ref('')
const code = ref('')
const rememberClient = ref(false)

async function submit() {
  error.value = ''
  busy.value = true
  try {
    const res = await $fetch<{
      user: User
      twoFactorRequired: boolean
      message?: string
      erp: { accessToken: string; encryptedAccessToken: string; expireInSeconds: number; userId: number; permissions: string[] }
    }>('/api/auth/erp/login', {
      method: 'POST',
      body: {
        accountName: form.accountName.trim(),
        userNameOrEmailAddress: form.userNameOrEmailAddress.trim(),
        password: form.password,
        ...(twoFactor.value ? { twoFactorVerificationCode: code.value.trim(), rememberClient: rememberClient.value } : {}),
      },
    })
    if (res.twoFactorRequired) {
      twoFactor.value = true
      twoFactorMessage.value = res.message ?? ''
      code.value = ''
      return
    }
    // persist the ERP identity for direct ERP service calls (httpClient)
    await saveBusinessIdentity({
      ...res.erp,
      expiresOn: String(Date.now() + res.erp.expireInSeconds * 1000),
    })
    navigateTo(typeof route.query.redirect === 'string' && route.query.redirect.startsWith('/') ? route.query.redirect : '/dashboard')
  } catch (e: unknown) {
    error.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Sign-in failed — try again'
  } finally {
    busy.value = false
  }
}

async function verifyCode() {
  await submit()
}

function backToCredentials() {
  twoFactor.value = false
  code.value = ''
  error.value = ''
}

// --- demo / offline member picker (kept for sandbox & offline demos) --------
const users = await useFetch<{ id: string; name: string; email: string; title: string; avatarHue: number }[]>('/api/auth/users')
const demoBusy = ref('')

async function pick(email: string) {
  demoBusy.value = email
  await demoLogin(email)
  navigateTo('/dashboard')
}
</script>

<template>
  <div class="flex min-h-screen items-center justify-center bg-gradient-to-br from-ink-950 via-ink-900 to-brand-900/60 px-4 py-10">
    <div class="w-full max-w-md">
      <div class="mb-8 text-center">
        <div class="mx-auto mb-4 flex h-14 w-14 items-center justify-center rounded-2xl bg-gradient-to-br from-brand-600 to-brand-400 shadow-xl shadow-brand-900/50">
          <svg viewBox="0 0 24 24" class="h-8 w-8 text-white" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
            <path d="M4 7h16M4 12h10M4 17h7" /><circle cx="18.5" cy="15.5" r="2.5" />
          </svg>
        </div>
        <h1 class="text-2xl font-bold text-white">Sign in to ShortCodeExpress</h1>
        <p class="mt-1 text-sm text-slate-400">Design, version and operate USSD services on the Sapo DSL Engine.</p>
      </div>

      <div class="rounded-2xl bg-white p-6 shadow-2xl">
        <!-- STEP 1 — ERP credentials -->
        <form v-if="!twoFactor" class="space-y-4" @submit.prevent="submit">
          <div class="mb-1 flex items-center gap-2 text-[11px] font-bold uppercase tracking-widest text-slate-400">
            <ShieldCheckIcon class="h-4 w-4 text-brand-600" /> Rhyolite Prime account
          </div>

          <label class="block">
            <span class="mb-1 flex items-center gap-1.5 text-xs font-semibold text-slate-600"><BuildingOffice2Icon class="h-3.5 w-3.5 text-slate-400" /> Workspace / tenant name</span>
            <input v-model="form.accountName" required placeholder="CediSave" autocomplete="organization"
              class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
          </label>

          <label class="block">
            <span class="mb-1 flex items-center gap-1.5 text-xs font-semibold text-slate-600"><UserIcon class="h-3.5 w-3.5 text-slate-400" /> Username or email</span>
            <input v-model="form.userNameOrEmailAddress" required placeholder="admin" autocomplete="username"
              class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
          </label>

          <label class="block">
            <span class="mb-1 flex items-center gap-1.5 text-xs font-semibold text-slate-600"><LockClosedIcon class="h-3.5 w-3.5 text-slate-400" /> Password</span>
            <input v-model="form.password" required type="password" placeholder="••••••••" autocomplete="current-password"
              class="w-full rounded-xl border border-slate-200 px-3.5 py-2.5 text-sm outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
          </label>

          <p v-if="error" class="rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700 ring-1 ring-rose-100">{{ error }}</p>

          <button type="submit" :disabled="busy"
            class="flex w-full items-center justify-center gap-2 rounded-xl bg-brand-600 px-4 py-3 text-sm font-semibold text-white shadow-lg shadow-brand-200 transition hover:bg-brand-500 disabled:opacity-50">
            <svg v-if="busy" class="h-4 w-4 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
            {{ busy ? 'Signing in…' : 'Sign in' }}
          </button>

          <p class="text-center text-[11px] text-slate-400">
            Accounts protected with Google Authenticator will be asked for their 6-digit code next.
          </p>
        </form>

        <!-- STEP 2 — Google Authenticator code -->
        <form v-else class="space-y-4" @submit.prevent="verifyCode">
          <div class="rounded-xl bg-brand-50 p-4 ring-1 ring-brand-100">
            <div class="flex items-center gap-2.5">
              <span class="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl bg-brand-600 text-white shadow-md"><ShieldCheckIcon class="h-5 w-5" /></span>
              <div>
                <div class="text-sm font-bold text-slate-900">Two-factor authentication</div>
                <div class="text-xs text-slate-500">{{ twoFactorMessage || 'Enter the 6-digit code from Google Authenticator' }}</div>
              </div>
            </div>
          </div>

          <label class="block text-center">
            <span class="mb-1.5 block text-xs font-semibold text-slate-600">Verification code</span>
            <input v-model="code" required inputmode="numeric" maxlength="6" placeholder="123456" autocomplete="one-time-code" autofocus
              class="mx-auto w-full max-w-[240px] rounded-xl border border-slate-200 px-4 py-3 text-center font-mono text-2xl font-extrabold tracking-[0.5em] outline-none focus:border-brand-500 focus:ring-2 focus:ring-brand-100" />
            <span class="mt-1.5 block text-[11px] text-slate-400">Open Google Authenticator on your phone</span>
          </label>

          <label class="flex items-center justify-center gap-2 text-xs text-slate-600">
            <input v-model="rememberClient" type="checkbox" class="h-4 w-4 rounded border-slate-300 text-brand-600 focus:ring-brand-300" />
            Trust this device for 30 days
          </label>

          <p v-if="error" class="rounded-lg bg-rose-50 px-3 py-2 text-xs font-medium text-rose-700 ring-1 ring-rose-100">{{ error }}</p>

          <button type="submit" :disabled="busy || code.replace(/\D/g, '').length !== 6"
            class="flex w-full items-center justify-center gap-2 rounded-xl bg-brand-600 px-4 py-3 text-sm font-semibold text-white shadow-lg shadow-brand-200 transition hover:bg-brand-500 disabled:opacity-50">
            <svg v-if="busy" class="h-4 w-4 animate-spin" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
            {{ busy ? 'Verifying…' : 'Verify &amp; sign in' }}
          </button>

          <button type="button" class="flex w-full items-center justify-center gap-1.5 text-xs font-semibold text-slate-500 hover:text-slate-700" @click="backToCredentials">
            <ArrowLeftIcon class="h-3.5 w-3.5" /> Use different credentials
          </button>
        </form>

        <div class="mt-5 border-t border-slate-100 pt-4">
          <p class="text-center text-xs text-slate-500">
            New here?
            <NuxtLink to="/signup" class="font-semibold text-brand-600 hover:underline">Create a workspace →</NuxtLink>
          </p>
        </div>
      </div>

      <!-- demo / offline fallback -->
      <details class="group mt-4 rounded-2xl bg-white/5 ring-1 ring-white/10 backdrop-blur">
        <summary class="cursor-pointer list-none px-5 py-3 text-center text-xs font-semibold text-slate-400 transition hover:text-slate-200">
          Demo mode — sign in as a seeded workspace member (offline)
        </summary>
        <div class="space-y-2 px-4 pb-4">
          <button
            v-for="u in users.data.value ?? []" :key="u.id"
            class="flex w-full items-center gap-3 rounded-xl bg-white p-3 text-left transition hover:bg-brand-50"
            :disabled="!!demoBusy" @click="pick(u.email)"
          >
            <div class="flex h-9 w-9 items-center justify-center rounded-full text-xs font-bold text-white" :style="{ background: `hsl(${u.avatarHue} 65% 45%)` }">
              {{ u.name.split(' ').map(n => n[0]).join('') }}
            </div>
            <div class="min-w-0 flex-1">
              <div class="text-sm font-semibold text-slate-800">{{ u.name }}</div>
              <div class="truncate text-xs text-slate-500">{{ u.title }} · {{ u.email }}</div>
            </div>
            <svg v-if="demoBusy === u.email" class="h-4 w-4 animate-spin text-brand-600" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
          </button>
        </div>
      </details>
    </div>
  </div>
</template>
