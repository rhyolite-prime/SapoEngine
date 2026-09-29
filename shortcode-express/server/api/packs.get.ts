import { SESSION_PACKS, SHORTCODE_SETUP_FEES, PORT_FLAT_MONTHLY } from '../utils/db'

export default defineEventHandler(() => ({ packs: SESSION_PACKS, setupFees: SHORTCODE_SETUP_FEES, portFlatMonthly: PORT_FLAT_MONTHLY }))
