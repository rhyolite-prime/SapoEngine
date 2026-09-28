import { useDb } from '../../utils/db'

export default defineEventHandler((event) => {
  const db = useDb()
  const id = getRouterParam(event, 'id')
  const flow = db.flows.find((f) => f.id === id)
  if (!flow) throw createError({ statusCode: 404, statusMessage: 'Flow not found' })
  return flow
})
