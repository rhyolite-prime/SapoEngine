// Server-side proxy so the in-browser simulator can exercise *real* API
// integrations without CORS problems — the same posture the C++ engine has
// with its outbound HTTP transport.
export default defineEventHandler(async (event) => {
  const body = await readBody<{ method?: string; url?: string; headers?: Record<string, string>; query?: Record<string, string>; body?: unknown; timeoutMs?: number }>(event)
  if (!body.url) throw createError({ statusCode: 400, statusMessage: 'url is required' })
  let parsed: URL
  try {
    parsed = new URL(body.url)
  } catch {
    throw createError({ statusCode: 400, statusMessage: 'invalid url' })
  }
  for (const [k, v] of Object.entries(body.query ?? {})) parsed.searchParams.set(k, v)

  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), Math.min(body.timeoutMs ?? 8000, 15000))
  try {
    const res = await fetch(parsed.toString(), {
      method: (body.method ?? 'GET').toUpperCase(),
      headers: { ...(body.headers ?? {}) },
      body: ['GET', 'HEAD'].includes((body.method ?? 'GET').toUpperCase()) ? undefined : JSON.stringify(body.body ?? {}),
      signal: controller.signal,
    })
    const text = await res.text()
    let json: unknown = null
    try { json = JSON.parse(text) } catch { json = { raw: text.slice(0, 2000) } }
    return { status: res.status, body: json }
  } catch (e) {
    throw createError({ statusCode: 502, statusMessage: `request failed: ${(e as Error).message}` })
  } finally {
    clearTimeout(timer)
  }
})
