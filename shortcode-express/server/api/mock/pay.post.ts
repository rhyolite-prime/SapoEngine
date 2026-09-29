// ---------------------------------------------------------------------------
// Sandbox payment rail for USSD flows: point an HTTP node at /api/mock/pay
// and the gateway will fire a `payment.succeeded` webhook when it returns.
// Always approves (it's a sandbox) after a short auth delay.
// ---------------------------------------------------------------------------
export default defineEventHandler(async (event) => {
  const body = await readBody<{ msisdn?: string; amount?: number | string; description?: string; shortcode?: string }>(event).catch(() => ({}))
  await new Promise((r) => setTimeout(r, 600))
  const amount = Number(body.amount ?? 0)
  return {
    status: 'success',
    transactionId: `MP-${Date.now().toString(36).toUpperCase()}-${Math.random().toString(36).slice(2, 6).toUpperCase()}`,
    amount: Number.isFinite(amount) ? amount : 0,
    currency: 'GHS',
    description: body.description ?? null,
    msisdn: body.msisdn ?? null,
    shortcode: body.shortcode ?? null,
    settledAt: new Date().toISOString(),
  }
})
