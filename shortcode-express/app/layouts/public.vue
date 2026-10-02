<!-- Public shell: zero app chrome. Landing, login, signup, checkout and the
     provider porting pages render their own full-screen designs — the menus
     only appear once you're signed in (layouts/default.vue). -->
<template>
  <div class="min-h-screen">
    <slot />
  </div>
</template>
