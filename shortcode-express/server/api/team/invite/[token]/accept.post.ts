import { useDb, saveDb, rid } from '../../../../utils/db'

// Demo endpoint: the invited engineer clicks the emailed link, which lands
// here. They become an active member of the workspace.
export default defineEventHandler((event) => {
  const db = useDb()
  const token = getRouterParam(event, 'token')
  const invite = db.invites.find((i) => i.token === token && i.status === 'pending')
  if (!invite) throw createError({ statusCode: 404, statusMessage: 'This invitation is no longer valid' })
  invite.status = 'accepted'
  const member = {
    id: rid('m'),
    name: invite.email.split('@')[0].replace(/[._]/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase()),
    email: invite.email,
    role: invite.role,
    joinedAt: new Date().toISOString(),
    status: 'active' as const,
  }
  db.members.push(member)
  saveDb(db)
  return { ok: true, member }
})
