// Onboarding step 1: declare how the workspace will be used — a business
// merchant running its own USSD services, or an aggregator building and
// managing services for client businesses. Shapes the onboarding copy and
// how codes are framed (yours vs. your clients').
import { useDb, saveDb } from '../../utils/db'
import { requireUser } from '../../utils/auth'

export default defineEventHandler(async (event) => {
  const user = requireUser(event)
  const body = await readBody<{ useCase?: 'merchant' | 'aggregator' }>(event)
  if (body.useCase !== 'merchant' && body.useCase !== 'aggregator') {
    throw createError({ statusCode: 400, statusMessage: 'Pick how you will use ShortCodeExpress — business merchant or aggregator' })
  }
  const db = useDb()
  const fresh = db.users.find((u) => u.id === user.id)
  if (!fresh) throw createError({ statusCode: 404, statusMessage: 'Account not found' })
  fresh.useCase = body.useCase
  saveDb(db)
  return { useCase: fresh.useCase }
})
