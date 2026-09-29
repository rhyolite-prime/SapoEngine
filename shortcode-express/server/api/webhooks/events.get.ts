import { WEBHOOK_EVENTS } from '../../utils/db'

export default defineEventHandler(() => ({ events: WEBHOOK_EVENTS }))
