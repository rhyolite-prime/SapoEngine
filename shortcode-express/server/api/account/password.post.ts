// ---------------------------------------------------------------------------
// Password reset for signed-in users — proxied to the Rhyolite Prime ERP
// (ABP Account/ChangePassword) and authenticated with the ERP access token
// issued at sign-in. The ERP owns credentials; we never store passwords.
// ---------------------------------------------------------------------------
import { useDb } from '../../utils/db'
import { requireUser } from '../../utils/auth'
import { erpRequest, ERP_PATHS } from '../../utils/erp'

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const body = await readBody<{ currentPassword?: string; newPassword?: string }>(event)
  const currentPassword = body.currentPassword ?? ''
  const newPassword = body.newPassword ?? ''

  if (!currentPassword) throw createError({ statusCode: 400, statusMessage: 'Enter your current password' })
  if (newPassword.length < 6) throw createError({ statusCode: 400, statusMessage: 'New password needs at least 6 characters' })

  const db = useDb()
  const fresh = db.users.find((u) => u.id === user.id)
  const token = fresh?.erp?.accessToken
  if (!token) {
    throw createError({ statusCode: 409, statusMessage: 'No ERP session on this account — sign in with your Rhyolite Prime credentials first' })
  }

  const res = await erpRequest(ERP_PATHS.changePassword, { currentPassword, newPassword }, token)
  if (res.unreachable) {
    throw createError({ statusCode: 503, statusMessage: 'Cannot reach the Rhyolite Prime ERP right now — check your connection and try again shortly' })
  }
  // ABP returns success:true with null result on a completed change
  if (!res.status || res.status >= 400) {
    throw createError({ statusCode: 409, statusMessage: res.error || 'The ERP rejected the password change' })
  }
  return { changed: true }
})
