// Built-in webhook receiver: point your endpoint here to see deliveries in
// the dashboard without running your own server. Responds 200 instantly.
export default defineEventHandler(async (event) => {
  const body = await readBody(event).catch(() => null)
  return { received: true, event: (body as { event?: string } | null)?.event ?? null, at: new Date().toISOString() }
})
