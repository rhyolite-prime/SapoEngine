import type { User } from '~/../shared/types'

export const useAuth = () => {
  const user = useState<User | null>('sce-user', () => null)
  const cookie = useCookie('sce_user')

  async function refresh() {
    if (!cookie.value) { user.value = null; return }
    user.value = await $fetch<User>('/api/auth/me')
  }

  async function login(email: string) {
    user.value = await $fetch<User>('/api/auth/login', { method: 'POST', body: { email } })
  }

  async function logout() {
    await $fetch('/api/auth/logout', { method: 'POST' })
    user.value = null
    cookie.value = null
    navigateTo('/login')
  }

  return { user, refresh, login, logout }
}
