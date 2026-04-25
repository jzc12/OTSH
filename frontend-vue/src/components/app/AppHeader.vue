<template>
  <header class="header">
    <div class="title">
      <p class="subtitle">On the Optimal Time/Space Tradeoff for Hash Tables（可视化验证系统）</p>
    </div>
    <div class="pill">
      <div class="pill-k">Backend</div>
      <div class="pill-v mono">{{ backendBase }}</div>
    </div>
  </header>
</template>

<script setup lang="ts">
defineProps<{
  backendBase: string
}>()
</script>
