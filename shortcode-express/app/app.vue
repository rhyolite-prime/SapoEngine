<script setup lang="ts">
const head = useHead
head({
  link: [
    { rel: 'preconnect', href: 'https://fonts.bunny.net' },
    { rel: 'stylesheet', href: 'https://fonts.bunny.net/css?family=inter:400,500,600,700,800|jetbrains-mono:400,500' },
  ],
})
</script>

<template>
  <NuxtLayout>
    <NuxtPage />
  </NuxtLayout>
</template>
