// Generic client for Rhyolite Prime ERP (ABP) service calls from the browser.
// Calls go through the /erp Nitro proxy (no CORS) and are authenticated with
// the business identity's access token when one exists.

/** Strip null/undefined/empty params so ABP doesn't choke on them */
const filterQueryParams = (query: Record<string, unknown> = {}): Record<string, unknown> =>
  Object.fromEntries(Object.entries(query).filter(([, v]) => v !== undefined && v !== null && v !== ''))

export const httpClient = async <T>(urlPath: string, options?: any) => {
  const config = useRuntimeConfig().public

  const businessIdentity = useBusinessAuthIdentity()
  const authToken = businessIdentity.value?.accessToken

  const defaultOptions = {
    lazy: false,
    immediate: true,
    server: false,
    baseURL: config.proxyApiBaseURL,
    headers: {
      ...(authToken ? { Authorization: `Bearer ${authToken}` } : {}),
    },
    mode: 'cors',
  }

  const opts = { ...defaultOptions, ...(options ?? {}) }
  if (opts.query) {
    opts.query = filterQueryParams(opts.query)
  }

  return await $fetch<T>(urlPath, {
    ...opts,
    onRequestError({ error }) {
      console.log(error)
    },
    onResponseError({ response }) {
      console.log(response)
    },
  })
}
