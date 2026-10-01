<script setup lang="ts">
import { UserPlusIcon, EnvelopeIcon, TrashIcon, ArrowPathIcon } from '@heroicons/vue/24/outline'

useHead({ title: 'Team · ShortCodeExpress' })

interface Member { id: string; name: string; email: string; role: 'owner' | 'editor' | 'viewer'; joinedAt: string; status: string }
interface Invite { id: string; email: string; role: string; invitedBy: string; invitedAt: string; token: string; status: string }

const { data, refresh } = await useFetch<{ members: Member[]; invites: Invite[]; business: { name: string } }>('/api/team')
const inviteEmail = ref('')
const inviteRole = ref<'editor' | 'viewer' | 'owner'>('editor')
const inviting = ref(false)
const error = ref('')
const copied = ref('')

async function invite() {
  error.value = ''
  inviting.value = true
  try {
    await $fetch('/api/team/invite', { method: 'POST', body: { email: inviteEmail.value, role: inviteRole.value } })
    inviteEmail.value = ''
    await refresh()
  } catch (e: unknown) {
    error.value = (e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not send invite'
  } finally {
    inviting.value = false
  }
}

async function accept(inv: Invite) {
  await $fetch(`/api/team/invite/${inv.token}/accept`, { method: 'POST' })
  await refresh()
}

async function rescind(inv: Invite) {
  await $fetch(`/api/team/invites/${inv.id}`, { method: 'DELETE' })
  await refresh()
}

async function removeMember(m: Member) {
  if (!confirm(`Remove ${m.name} from the workspace?`)) return
  try {
    await $fetch(`/api/team/${m.id}`, { method: 'DELETE' })
    await refresh()
  } catch (e: unknown) {
    alert((e as { data?: { statusMessage?: string } }).data?.statusMessage ?? 'Could not remove member')
  }
}

async function setRole(m: Member, role: string) {
  await $fetch(`/api/team/${m.id}`, { method: 'PATCH', body: { role } })
  await refresh()
}

function copyLink(inv: Invite) {
  const link = `${window.location.origin}/api/team/invite/${inv.token}/accept`
  navigator.clipboard?.writeText(link)
  copied.value = inv.id
  setTimeout(() => { copied.value = '' }, 1600)
}

const roleStyle: Record<string, string> = {
  owner: 'bg-brand-100 text-brand-700',
  editor: 'bg-sky-100 text-sky-700',
  viewer: 'bg-slate-100 text-slate-600',
}
const initials = (name: string) => name.split(' ').map((n) => n[0]).join('').slice(0, 2).toUpperCase()
const hue = (s: string) => [...s].reduce((a, c) => a + c.charCodeAt(0), 0) % 360
</script>

<template>
  <div class="p-8">
    <div class="mb-6">
      <h1 class="text-2xl font-bold text-slate-900">Team</h1>
      <p class="mt-1 text-sm text-slate-500">Invite collaborators by email to co-design USSD flows in the {{ data?.business.name }} workspace.</p>
    </div>

    <div class="grid gap-6 lg:grid-cols-3">
      <!-- members -->
      <div class="rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200/70 lg:col-span-2">
        <h3 class="mb-4 text-sm font-bold text-slate-800">Members ({{ data?.members.length }})</h3>
        <div class="space-y-3">
          <div v-for="m in data?.members ?? []" :key="m.id" class="flex items-center gap-4 rounded-xl border border-slate-100 p-4">
            <div class="flex h-11 w-11 items-center justify-center rounded-full text-sm font-bold text-white" :style="{ background: `hsl(${hue(m.email)} 55% 45%)` }">{{ initials(m.name) }}</div>
            <div class="min-w-0 flex-1">
              <div class="text-sm font-bold text-slate-800">{{ m.name }}</div>
              <div class="truncate text-xs text-slate-500">{{ m.email }} · joined {{ new Date(m.joinedAt).toLocaleDateString() }}</div>
            </div>
            <select
              v-if="m.role !== 'owner'" :value="m.role"
              class="rounded-lg border border-slate-200 bg-white px-2 py-1.5 text-xs font-semibold text-slate-700"
              @change="setRole(m, ($event.target as HTMLSelectElement).value)"
            >
              <option value="editor">Editor</option>
              <option value="viewer">Viewer</option>
            </select>
            <span v-else class="rounded-full bg-brand-100 px-2.5 py-1 text-[11px] font-bold text-brand-700">Owner</span>
            <button v-if="m.role !== 'owner'" class="rounded-md p-1.5 text-slate-300 hover:bg-rose-50 hover:text-rose-600" title="Remove" @click="removeMember(m)">
              <TrashIcon class="h-4 w-4" />
            </button>
          </div>
        </div>

        <!-- pending invites -->
        <h3 class="mb-3 mt-8 text-sm font-bold text-slate-800">Pending invitations ({{ data?.invites.length }})</h3>
        <div v-if="!data?.invites.length" class="rounded-xl border border-dashed border-slate-200 p-4 text-xs text-slate-400">No pending invitations.</div>
        <div v-for="inv in data?.invites ?? []" :key="inv.id" class="mb-2 flex flex-wrap items-center gap-3 rounded-xl border border-amber-100 bg-amber-50/50 p-4">
          <div class="flex h-9 w-9 items-center justify-center rounded-full bg-amber-100 text-amber-600"><EnvelopeIcon class="h-4 w-4" /></div>
          <div class="min-w-0 flex-1">
            <div class="text-sm font-semibold text-slate-800">{{ inv.email }}</div>
            <div class="text-xs text-slate-500">Invited as {{ inv.role }} by {{ inv.invitedBy }} · {{ new Date(inv.invitedAt).toLocaleDateString() }}</div>
          </div>
          <span class="rounded-full px-2 py-0.5 text-[11px] font-bold" :class="roleStyle[inv.role] ?? roleStyle.viewer">{{ inv.role }}</span>
          <button class="rounded-lg border border-slate-200 bg-white px-3 py-1.5 text-xs font-semibold text-slate-600 hover:border-slate-300" @click="copyLink(inv)">
            {{ copied === inv.id ? 'Copied!' : 'Copy invite link' }}
          </button>
          <button class="inline-flex items-center gap-1 rounded-lg bg-success-600 px-3 py-1.5 text-xs font-semibold text-white hover:bg-success-500" @click="accept(inv)">
            <ArrowPathIcon class="h-3.5 w-3.5" /> Simulate accept
          </button>
          <button class="rounded-md p-1.5 text-slate-300 hover:bg-rose-50 hover:text-rose-600" title="Rescind" @click="rescind(inv)"><TrashIcon class="h-4 w-4" /></button>
        </div>
      </div>

      <!-- invite card -->
      <div class="h-fit rounded-2xl bg-white p-6 shadow-sm ring-1 ring-slate-200/70">
        <h3 class="flex items-center gap-2 text-sm font-bold text-slate-800"><UserPlusIcon class="h-4 w-4 text-brand-500" /> Invite a collaborator</h3>
        <p class="mt-1 text-xs text-slate-500">They'll receive an email with a join link for this workspace.</p>
        <div class="mt-4 space-y-3">
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Email address</label>
            <input
              v-model="inviteEmail" type="email" placeholder="engineer@company.com"
              class="w-full rounded-lg border border-slate-200 px-3 py-2 text-sm focus:border-brand-400 focus:outline-none focus:ring-2 focus:ring-brand-100"
              @keyup.enter="invite()"
            />
            <p v-if="error" class="mt-1 text-xs text-rose-600">{{ error }}</p>
          </div>
          <div>
            <label class="mb-1 block text-xs font-semibold text-slate-600">Role</label>
            <div class="grid gap-2">
              <label v-for="r in [
                { id: 'editor', name: 'Editor', desc: 'Design flows, build & release' },
                { id: 'viewer', name: 'Viewer', desc: 'Read-only access to flows and stats' },
              ]" :key="r.id" class="flex cursor-pointer items-start gap-2 rounded-lg border p-2.5" :class="inviteRole === r.id ? 'border-brand-400 bg-brand-50' : 'border-slate-200'">
                <input v-model="inviteRole" type="radio" :value="r.id" class="mt-0.5 accent-brand-600" />
                <div><div class="text-xs font-bold text-slate-800">{{ r.name }}</div><div class="text-[11px] text-slate-500">{{ r.desc }}</div></div>
              </label>
            </div>
          </div>
          <button :disabled="inviting || !inviteEmail.includes('@')" class="w-full rounded-lg bg-brand-600 px-4 py-2.5 text-sm font-semibold text-white hover:bg-brand-500 disabled:opacity-50" @click="invite()">
            {{ inviting ? 'Sending…' : 'Send invitation' }}
          </button>
        </div>
        <div class="mt-5 rounded-xl bg-slate-50 p-3 text-[11px] leading-relaxed text-slate-500">
          <b class="text-slate-600">How invites work:</b> each invitation is a signed token. In this demo you can copy the join link or simulate the recipient accepting it. Production hooks into your SMTP / SES account.
        </div>
      </div>
    </div>
  </div>
</template>
