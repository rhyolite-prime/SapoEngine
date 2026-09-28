export default defineNuxtRouteMiddleware((to) => {
  // Public pages
  if (['/', '/login'].includes(to.path)) return
  const cookie = useCookie('sce_user')
  if (!cookie.value) {
    return navigateTo('/login')
  }
})
