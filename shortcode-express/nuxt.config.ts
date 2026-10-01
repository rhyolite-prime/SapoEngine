import tailwindcss from '@tailwindcss/vite'

// Rhyolite Prime ERP (ABP). Browser-side ERP service calls (httpClient) go
// through the /erp proxy below so the ERP never has to allow CORS; the
// server-side auth proxy uses the same base via ERP_API_BASE.
const ERP_API_BASE = process.env.ERP_API_BASE ?? 'https://erp-api.rhyoliteprime.com'

export default defineNuxtConfig({
  compatibilityDate: '2026-07-01',
  devtools: { enabled: false },
  ssr: true,

  css: ['~/assets/css/main.css'],

  runtimeConfig: {
    public: {
      proxyApiBaseURL: '/erp/api',
      proxyApiAuthBaseURL: '/erp/api',
    },
  },

  routeRules: {
    '/erp/**': { proxy: ERP_API_BASE + '/**' },
  },

  vite: {
    plugins: [tailwindcss()],
  },

  devServer: {
    host: '0.0.0.0',
    port: 3000,
  },

  nitro: {
    experimental: { tasks: false },
  },

  app: {
    head: {
      title: 'ShortCodeExpress — Build USSD services visually',
      htmlAttrs: { lang: 'en' },
      meta: [
        { charset: 'utf-8' },
        { name: 'viewport', content: 'width=device-width, initial-scale=1' },
        {
          name: 'description',
          content:
            'ShortCodeExpress: drag-and-drop builder for USSD services powered by the Sapo DSL Engine. Design menus, plug in APIs, version, release and roll back — all visually.',
        },
      ],
      link: [
        { rel: 'preconnect', href: 'https://fonts.googleapis.com' },
        { rel: 'preconnect', href: 'https://fonts.gstatic.com', crossorigin: '' },
        {
          rel: 'stylesheet',
          href: 'https://fonts.googleapis.com/css2?family=Sora:wght@300;400;500;600;700;800&family=JetBrains+Mono:wght@400;500;600&display=swap',
        },
      ],
    },
  },
})
