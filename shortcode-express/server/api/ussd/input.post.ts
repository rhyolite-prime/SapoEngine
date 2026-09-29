import { requireUser } from '../../utils/auth'
import { sendInput } from '../../utils/ussd'

export default defineEventHandler(async (event) => {
  requireUser(event)
  const body = await readBody<{ sessionId?: string; text?: string }>(event)
  if (!body.sessionId) throw createError({ statusCode: 400, statusMessage: 'sessionId is required' })
  const text = (body.text ?? '').toString()
  if (!text) throw createError({ statusCode: 400, statusMessage: 'Nothing was sent' })
  return await sendInput(body.sessionId, text)
})
