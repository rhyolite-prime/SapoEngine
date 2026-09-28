export default defineEventHandler((event) => {
  deleteCookie(event, 'sce_user', { path: '/' })
  return { ok: true }
})
