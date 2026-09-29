import { useDb } from '../../utils/db'
import { requireUser } from '../../utils/auth'
import { emitWebhook } from '../../utils/webhooks'

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const db = useDb()
  const delivery = await emitWebhook(db, user, 'test.ping', {
    message: 'Hello from ShortCodeExpress — your webhook endpoint works!',
    code: '*714*42#',
    msisdn: '0244000000',
    sent_by: user.email,
  }, { force: true })
  if (!delivery) throw createError({ statusCode: 400, statusMessage: 'Configure an endpoint URL first' })
  return delivery
})
