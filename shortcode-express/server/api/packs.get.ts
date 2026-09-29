import { SESSION_PACKS, SHORTCODE_SETUP_FEES } from '../utils/db'

export default defineEventHandler(() => ({ packs: SESSION_PACKS, setupFees: SHORTCODE_SETUP_FEES }))
