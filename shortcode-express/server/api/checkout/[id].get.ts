import { useDb } from '../../utils/db'

// checkout pages are loadable by id (the payment itself requires sign-in)
export default defineEventHandler((event) => {
  const id = getRouterParam(event, 'id')
  const db = useDb()
  const c = db.checkouts.find((x) => x.id === id)
  if (!c) throw createError({ statusCode: 404, statusMessage: 'Checkout not found' })
  return c
})
