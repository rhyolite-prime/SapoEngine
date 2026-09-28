<script setup lang="ts">
definePageMeta({ layout: 'default' })
const { login } = useAuth()
const users = await useFetch<{ id: string; name: string; email: string; title: string; avatarHue: number }[]>('/api/auth/users')
const busy = ref('')

async function pick(email: string) {
  busy.value = email
  await login(email)
  navigateTo('/dashboard')
}
</script>

<template>
  <div class="flex min-h-screen items-center justify-center bg-gradient-to-br from-ink-950 via-ink-900 to-brand-900/60 px-4">
    <div class="w-full max-w-md">
      <div class="mb-8 text-center">
        <div class="mx-auto mb-4 flex h-14 w-14 items-center justify-center rounded-2xl bg-gradient-to-br from-brand-500 to-fuchsia-600 shadow-xl shadow-brand-900/50">
          <svg viewBox="0 0 24 24" class="h-8 w-8 text-white" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
            <path d="M4 7h16M4 12h10M4 17h7" /><circle cx="18.5" cy="15.5" r="2.5" />
          </svg>
        </div>
        <h1 class="text-2xl font-bold text-white">Sign in to ShortCodeExpress</h1>
        <p class="mt-1 text-sm text-slate-400">Design, version and operate USSD services on the Sapo DSL Engine.</p>
      </div>

      <div class="rounded-2xl bg-white p-6 shadow-2xl">
        <p class="mb-4 text-xs font-semibold uppercase tracking-wide text-slate-400">Choose a workspace member (demo)</p>
        <div class="space-y-2">
          <button
            v-for="u in users.data.value ?? []" :key="u.id"
            class="flex w-full items-center gap-3 rounded-xl border border-slate-200 p-3 text-left transition hover:border-brand-400 hover:bg-brand-50"
            :disabled="!!busy" @click="pick(u.email)"
          >
            <div class="flex h-10 w-10 items-center justify-center rounded-full text-sm font-bold text-white" :style="{ background: `hsl(${u.avatarHue} 65% 45%)` }">
              {{ u.name.split(' ').map(n => n[0]).join('') }}
            </div>
            <div class="min-w-0 flex-1">
              <div class="text-sm font-semibold text-slate-800">{{ u.name }}</div>
              <div class="truncate text-xs text-slate-500">{{ u.title }} · {{ u.email }}</div>
            </div>
            <svg v-if="busy === u.email" class="h-4 w-4 animate-spin text-brand-600" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="3" class="opacity-25" /><path d="M22 12a10 10 0 0 1-10 10" stroke="currentColor" stroke-width="3" /></svg>
          </button>
        </div>
        <p class="mt-4 text-center text-[11px] text-slate-400">
          Demo authentication — production deployments wire SSO (Google Workspace / Entra ID).
        </p>
      </div>
    </div>
  </div>
</template>
