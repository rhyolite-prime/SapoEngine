// Self-service developer signup: creates the account, seeds an API key and
// signs the developer in — they land on the 5-minute onboarding.
import { useDb, saveDb, rid } from '../../utils/db'
import { generateApiKey } from '../../utils/auth'
import type { Member, User } from '../../../shared/types'

export default defineEventHandler(async (event) => {
  const body = await readBody<{ name?: string; email?: string; company?: string }>(event)
  const name = (body.name ?? '').trim()
  const email = (body.email ?? '').trim().toLowerCase()
  const company = (body.company ?? '').trim()

  if (name.length < 2) throw createError({ statusCode: 400, statusMessage: 'Tell us your name' })
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) throw createError({ statusCode: 400, statusMessage: 'Enter a valid email address' })

  const db = useDb()
  if (db.users.some((u) => u.email.toLowerCase() === email)) {
    throw createError({ statusCode: 409, statusMessage: 'That email already has an account — sign in instead' })
  }

  const user: User = {
    id: rid('u'),
    name,
    email,
    role: 'owner',
    avatarHue: Math.floor(Math.random() * 360),
    title: company ? `Developer · ${company}` : 'Developer',
    company: company || undefined,
    createdAt: new Date().toISOString(),
    apiKeys: [{ id: rid('key'), name: 'Default key', key: generateApiKey(), createdAt: new Date().toISOString() }],
    webhook: undefined,
  }
  db.users.push(user)

  const member: Member = { id: rid('m'), name, email, role: 'owner', joinedAt: user.createdAt!, status: 'active' }
  db.members.push(member)
  saveDb(db)

  setCookie(event, 'sce_user', user.id, { path: '/', maxAge: 60 * 60 * 24 * 30, httpOnly: false, sameSite: 'lax' })
  return user
})
