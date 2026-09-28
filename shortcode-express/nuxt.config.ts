import tailwindcss from '@tailwindcss/vite'

export default defineNuxtConfig({
  compatibilityDate: '2026-07-01',
  devtools: { enabled: false },
  ssr: true,

  css: ['~/assets/css/main.css'],

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
    },
  },
})
