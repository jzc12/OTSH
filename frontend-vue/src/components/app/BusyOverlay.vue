<template>
  <div v-if="busy || (resize?.is_resizing ?? false)" class="overlay">
    <div class="overlay-card">
      <div class="spinner"></div>
      <div class="overlay-text">
        <div class="overlay-title">{{ titleText }}</div>
        <div class="overlay-sub">{{ hintText }}</div>
        <div class="overlay-sub mono" v-if="resize?.is_resizing">
          扩容：{{ resize.resize_progress }}/{{ resize.total_bins }} bins
          ({{ resizePercent }}%)，已迁移 {{ resize.migrated_keys }} keys，
          已运行 {{ Math.floor((resize.elapsed_ms ?? 0) / 1000) }}s
        </div>
        <div class="overlay-sub mono" v-if="!resize?.is_resizing && elapsedMs > 0">
          已运行：{{ Math.floor(elapsedMs / 1000) }}s
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{
  busy: boolean
  busyTitle: string
  busyHint: string
  elapsedMs: number
  resize?: {
    is_resizing: boolean
    resize_progress: number
    total_bins: number
    new_total_bins: number
    migrated_keys: number
    started_at_ms: number
    elapsed_ms: number
    last_step_ms: number
    finished_total_ms: number
  } | null
}>()

const titleText = computed(() => {
  if (props.resize?.is_resizing) return '扩容中...'
  return props.busyTitle
})

const hintText = computed(() => {
  if (props.resize?.is_resizing) return '正在后台迁移数据；你可以观察迁移数量与耗时。'
  return props.busyHint
})

const resizePercent = computed(() => {
  const r = props.resize
  if (!r || !r.total_bins) return 0
  const p = Math.floor((r.resize_progress / r.total_bins) * 100)
  return Math.max(0, Math.min(100, p))
})
</script>
