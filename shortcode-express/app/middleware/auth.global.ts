// ---------------------------------------------------------------------------
// Global route guard. Everything requires a session except the landing page,
// the auth pages and the public provider porting links (/port/:token — the
// token itself is the authorization). Signed-in users are bounced away from
// the login/signup pages so they land on the dashboard instead.
// ---------------------------------------------------------------------------
export default defineNuxtRouteMiddleware((to) => {
  const PUBLIC_PAGES = ['/', '/login', '/signup']
  const isPublic = PUBLIC_PAGES.includes(to.path) || to.path.startsWith('/port/')

  const cookie = useCookie('sce_user')

  if (isPublic) {
    if (cookie.value && (to.path === '/login' || to.path === '/signup')) {
      return navigateTo('/dashboard')
    }
    return
  }

  if (!cookie.value) {
    return navigateTo(`/login?redirect=${encodeURIComponent(to.fullPath)}`)
  }
})
