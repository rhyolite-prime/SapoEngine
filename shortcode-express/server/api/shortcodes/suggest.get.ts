// Next available system-assigned code (the default "let the system choose" mode)
import { useDb } from '../../utils/db'
import { normalizeCode } from '../../utils/ussd'

export default defineEventHandler(() => {
  const db = useDb()
  const taken = new Set(db.shortcodes.map((s) => normalizeCode(s.code)))
  const pending = new Set(db.checkouts.filter((c) => c.status === 'pending' && c.kind === 'shortcode').map((c) => normalizeCode(String(c.meta.code ?? ''))))
  for (let n = 10; n <= 999; n++) {
    const code = `*714*${n}#`
    if (!taken.has(normalizeCode(code)) && !pending.has(normalizeCode(code))) return { code }
  }
  for (let n = 1000; n <= 9999; n++) {
    const code = `*71${n}#`
    if (!taken.has(normalizeCode(code)) && !pending.has(normalizeCode(code))) return { code }
  }
  throw createError({ statusCode: 503, statusMessage: 'No codes available' })
})
