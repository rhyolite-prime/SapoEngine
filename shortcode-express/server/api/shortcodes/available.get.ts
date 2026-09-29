// Live availability check for developer-chosen codes
import { useDb } from '../../utils/db'
import { findShortcodeByCode } from '../../utils/ussd'

const CODE_RE = /^\*[0-9]{3,5}(\*[0-9]{1,3})?#$/

export default defineEventHandler((event) => {
  const q = getQuery(event)
  const code = String(q.code ?? '').replace(/\s+/g, '')
  if (!CODE_RE.test(code)) {
    return { code, available: false, reason: 'format', message: 'Format: *714*42# or *565# — 3-5 digits, optional *group, then #' }
  }
  if (findShortcodeByCode(code)) {
    return { code, available: false, reason: 'taken', message: `${code} is already taken` }
  }
  const db = useDb()
  const pending = db.checkouts.some((c) => c.status === 'pending' && c.kind === 'shortcode' && String(c.meta.code ?? '').toLowerCase() === code.toLowerCase())
  if (pending) return { code, available: false, reason: 'reserved', message: `${code} is in someone's checkout right now` }
  return { code, available: true, message: `${code} is yours 🎉` }
})
