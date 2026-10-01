// ---------------------------------------------------------------------------
// ERP-backed self-service signup (Rhyolite Prime ERP / ABP):
// POST /api/services/app/Tenant/PrivateCreateSignup with the business name,
// admin email, phone and password. On success the ERP issues the access
// token; we mirror the tenant admin into a local workspace user (API key,
// team member row) and start the session.
// ---------------------------------------------------------------------------
import { useDb, saveDb, rid } from '../../../utils/db'
import { generateApiKey } from '../../../utils/auth'
import { erpRequest, ERP_PATHS } from '../../../utils/erp'
import type { Member, User } from '../../../../shared/types'

export default defineEventHandler(async (event) => {
  const body = await readBody<{
    businessname?: string
    adminEmailAddress?: string
    phoneNo?: string
    Password?: string
  }>(event)

  const businessname = (body.businessname ?? '').trim()
  const adminEmailAddress = (body.adminEmailAddress ?? '').trim().toLowerCase()
  const phoneNo = (body.phoneNo ?? '').trim()
  const Password = body.Password ?? ''

  if (businessname.length < 2) throw createError({ statusCode: 400, statusMessage: 'Enter your business name' })
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(adminEmailAddress)) throw createError({ statusCode: 400, statusMessage: 'Enter a valid admin email address' })
  if (Password.length < 6) throw createError({ statusCode: 400, statusMessage: 'Choose a password with at least 6 characters' })

  const res = await erpRequest(ERP_PATHS.signup, { businessname, adminEmailAddress, phoneNo, Password })

  if (res.unreachable) {
    throw createError({ statusCode: 503, statusMessage: 'Cannot reach the Rhyolite Prime ERP right now — check your connection and try again shortly' })
  }
  if (!res.ok || !res.result?.accessToken || !res.result.userId) {
    throw createError({ statusCode: 409, statusMessage: res.error || 'The ERP could not create your tenant — try again' })
  }

  // --- mirror the tenant admin into the local workspace ---------------------
  const db = useDb()
  const existing = db.users.find((u) => u.email.toLowerCase() === adminEmailAddress)
  const erp = {
    userId: res.result.userId,
    tenant: businessname,
    accessToken: res.result.accessToken,
    encryptedAccessToken: res.result.encryptedAccessToken ?? undefined,
    expiresAt: new Date(Date.now() + (res.result.expireInSeconds ?? 86400) * 1000).toISOString(),
  }

  let user: User
  if (existing) {
    existing.erp = erp
    user = existing
  } else {
    user = {
      id: rid('u'),
      name: businessname,
      email: adminEmailAddress,
      role: 'owner',
      avatarHue: Math.floor(Math.random() * 360),
      title: `Tenant admin · ${businessname}`,
      company: businessname,
      erp,
      createdAt: new Date().toISOString(),
      apiKeys: [{ id: rid('key'), name: 'Default key', key: generateApiKey(), createdAt: new Date().toISOString() }],
    }
    db.users.push(user)
    const member: Member = { id: rid('m'), name: user.name, email: user.email, role: 'owner', joinedAt: user.createdAt!, status: 'active' }
    db.members.push(member)
  }
  saveDb(db)

  setCookie(event, 'sce_user', user.id, { path: '/', maxAge: 60 * 60 * 24 * 30, httpOnly: false, sameSite: 'lax' })
  return {
    user,
    // ERP tokens for the client-side business identity (rhyolite-identity cookie)
    erp: {
      accessToken: erp.accessToken,
      encryptedAccessToken: erp.encryptedAccessToken ?? '',
      expireInSeconds: res.result.expireInSeconds ?? 86400,
      userId: erp.userId,
      permissions: res.result.permissions ?? [],
    },
  }
})
