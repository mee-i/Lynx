<template>
  <div class="min-h-screen bg-default flex flex-col items-center justify-center px-6">
    <div class="text-center max-w-2xl">
      <div class="flex items-center justify-center gap-2 mb-8">
        <span class="text-2xl font-bold text-highlighted">Lynx</span>
      </div>

      <h1 class="text-4xl lg:text-5xl font-bold text-highlighted mb-4">
        Get Access.<br />
      </h1>

      <div class="flex justify-center gap-3">
        <UButton size="lg" variant="outline" to="/login">
          Sign In
        </UButton>
      </div>
    </div>
  </div>
</template>