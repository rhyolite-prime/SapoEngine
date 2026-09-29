import {
  ListBulletIcon, PencilIcon, LockClosedIcon, ChatBubbleLeftIcon, GlobeAltIcon, CubeIcon,
  BoltIcon, ArrowsRightLeftIcon, ArrowsPointingOutIcon, ShieldCheckIcon, CodeBracketIcon, CircleStackIcon,
  FunnelIcon, ClockIcon, ArrowPathIcon, ArrowRightCircleIcon, ViewColumnsIcon, CalendarDaysIcon,
  SignalIcon, CheckCircleIcon, XCircleIcon, VariableIcon,
} from '@heroicons/vue/24/outline'
import type { FunctionalComponent } from 'vue'

export const KIND_ICONS: Record<string, FunctionalComponent> = {
  list: ListBulletIcon, keyboard: PencilIcon, lock: LockClosedIcon, chat: ChatBubbleLeftIcon,
  globe: GlobeAltIcon, cube: CubeIcon, bolt: BoltIcon, branch: ArrowsRightLeftIcon,
  shuffle: ArrowsPointingOutIcon, shield: ShieldCheckIcon, code: CodeBracketIcon, variable: VariableIcon,
  filter: FunnelIcon, database: CircleStackIcon, clock: ClockIcon, repeat: ArrowPathIcon,
  exit: ArrowRightCircleIcon, columns: ViewColumnsIcon, calendar: CalendarDaysIcon,
  radio: SignalIcon, check: CheckCircleIcon, x: XCircleIcon,
}

// Full literal class strings so Tailwind's scanner keeps them
export interface KindVisual { ring: string; head: string; icon: string; dot: string; text: string }

export const KIND_VISUALS: Record<string, KindVisual> = {
  brand: { ring: 'ring-brand-200', head: 'bg-brand-50', icon: 'text-brand-600', dot: 'bg-brand-500', text: 'text-brand-700' },
  sky: { ring: 'ring-sky-200', head: 'bg-sky-50', icon: 'text-sky-600', dot: 'bg-sky-500', text: 'text-sky-700' },
  amber: { ring: 'ring-amber-200', head: 'bg-amber-50', icon: 'text-amber-600', dot: 'bg-amber-500', text: 'text-amber-700' },
  success: { ring: 'ring-success-200', head: 'bg-success-50', icon: 'text-success-600', dot: 'bg-success-500', text: 'text-success-700' },
  slate: { ring: 'ring-slate-200', head: 'bg-slate-50', icon: 'text-slate-600', dot: 'bg-slate-500', text: 'text-slate-700' },
  rose: { ring: 'ring-rose-200', head: 'bg-rose-50', icon: 'text-rose-600', dot: 'bg-rose-500', text: 'text-rose-700' },
}

export function kindVisual(kind: string): KindVisual {
  const groups: Record<string, string> = {
    menu: 'brand', input: 'brand', pin: 'brand', display: 'brand', await_event: 'brand',
    http: 'sky', subflow: 'sky', event: 'sky',
    if: 'amber', choice: 'amber', try: 'amber', script: 'amber',
    assign: 'success', transform: 'success', query: 'success',
    wait: 'slate', loop: 'slate', break: 'slate', parallel: 'slate', schedule: 'slate',
    end_success: 'rose', end_failure: 'rose',
  }
  return KIND_VISUALS[groups[kind] ?? 'slate']
}
