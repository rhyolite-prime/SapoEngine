// ---------------------------------------------------------------------------
// ERP-backed sign-in (Rhyolite Prime ERP / ABP):
//   1. credentials (+ optional Google Authenticator code) go to
//      /api/tokenauth/authenticate on the ERP
//   2. if the account has two-factor enabled, the first attempt comes back
//      without a token — we surface { twoFactorRequired: true } and the UI
//      asks for the 6-digit code, then retries with twoFactorVerificationCode
//   3. on success we map the ERP identity to a local workspace user (so
//      flows, short codes and invoices keep working) and set the session
//      cookie.
// ---------------------------------------------------------------------------
import { useDb, saveDb, rid } from '../../../utils/db'
import { generateApiKey } from '../../../utils/auth'
import { erpRequest, ERP_PATHS, mentionsTwoFactor, syntheticEmail } from '../../../utils/erp'
import type { Member, User } from '../../../../shared/types'

export default defineEventHandler(async (event) => {
  const body = await readBody<{
    accountName?: string
    userNameOrEmailAddress?: string
    password?: string
    twoFactorVerificationCode?: string
    rememberClient?: boolean
  }>(event)

  const accountName = (body.accountName ?? '').trim()
  const userNameOrEmailAddress = (body.userNameOrEmailAddress ?? '').trim()
  const password = body.password ?? ''
  const code = (body.twoFactorVerificationCode ?? '').replace(/\D/g, '')

  if (!accountName) throw createError({ statusCode: 400, statusMessage: 'Enter your workspace name (e.g. CediSave)' })
  if (!userNameOrEmailAddress) throw createError({ statusCode: 400, statusMessage: 'Enter your username or email' })
  if (!password) throw createError({ statusCode: 400, statusMessage: 'Enter your password' })

  const res = await erpRequest(ERP_PATHS.login, {
    accountName,
    userNameOrEmailAddress,
    password,
    ...(code ? { twoFactorVerificationCode: code } : {}),
    rememberClient: !!body.rememberClient,
  })

  if (res.unreachable) {
    throw createError({ statusCode: 503, statusMessage: 'Cannot reach the Rhyolite Prime ERP right now — check your connection and try again shortly' })
  }

  // --- two-factor gate -----------------------------------------------------
  const result = res.result
  const twoFactorPending = !res.ok && (!!result?.requiresTwoFactorAuthenticaton || !!result?.requiresTwoFactorAuthentication || mentionsTwoFactor(res.error))
  if (twoFactorPending) {
    return {
      twoFactorRequired: true,
      // a code was sent and still gated → the code didn't match
      message: code
        ? 'That code didn\u2019t match — check Google Authenticator and try again'
        : 'This account is protected with Google Authenticator',
    }
  }

  if (!res.ok || !result?.accessToken || !result.userId) {
    throw createError({
      statusCode: 401,
      statusMessage: res.error || 'Invalid credentials — check your workspace, username and password',
    })
  }

  // --- map the ERP identity onto the local workspace ------------------------
  const db = useDb()
  const email = syntheticEmail(userNameOrEmailAddress, accountName)
  let user = db.users.find((u) => u.erp?.userId === result.userId)
    ?? db.users.find((u) => u.email.toLowerCase() === email.toLowerCase())

  const erp = {
    userId: result.userId,
    tenant: accountName,
    accessToken: result.accessToken,
    encryptedAccessToken: result.encryptedAccessToken ?? undefined,
    expiresAt: new Date(Date.now() + (result.expireInSeconds ?? 86400) * 1000).toISOString(),
  }

  if (user) {
    user.erp = erp
  } else {
    const name = userNameOrEmailAddress.includes('@')
      ? userNameOrEmailAddress.split('@')[0].replace(/[._-]+/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase())
      : userNameOrEmailAddress
    user = {
      id: rid('u'),
      name,
      email,
      role: 'owner',
      avatarHue: Math.floor(Math.random() * 360),
      title: `Workspace admin · ${accountName}`,
      company: accountName,
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
    twoFactorRequired: false,
    // ERP tokens for the client-side business identity (rhyolite-identity cookie)
    erp: {
      accessToken: result.accessToken,
      encryptedAccessToken: result.encryptedAccessToken ?? '',
      expireInSeconds: result.expireInSeconds ?? 86400,
      userId: result.userId,
      permissions: result.permissions ?? [],
    },
  }
})
