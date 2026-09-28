import { useDb, saveDb, rid } from '../../utils/db'
import { randomUUID } from 'node:crypto'

// Invite a collaborator by email. In production this would dispatch a real
// email with a signed link; here the invite token is returned so the demo can
// show the accept flow end-to-end.
export default defineEventHandler(async (event) => {
  const db = useDb()
  const body = await readBody<{ email?: string; role?: 'owner' | 'editor' | 'viewer' }>(event)
  const email = (body.email ?? '').trim().toLowerCase()
  if (!/^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(email)) throw createError({ statusCode: 400, statusMessage: 'Enter a valid email address' })
  if (db.members.some((m) => m.email === email)) throw createError({ statusCode: 400, statusMessage: `${email} is already a team member` })
  if (db.invites.some((i) => i.email === email && i.status === 'pending')) throw createError({ statusCode: 400, statusMessage: `${email} already has a pending invite` })

  const invite = {
    id: rid('inv'),
    email,
    role: (body.role ?? 'editor') as 'owner' | 'editor' | 'viewer',
    invitedBy: 'ama@rhyoliteprime.com',
    invitedAt: new Date().toISOString(),
    token: 'inv-' + randomUUID().slice(0, 12),
    status: 'pending' as const,
  }
  db.invites.push(invite)
  saveDb(db)
  return invite
})
