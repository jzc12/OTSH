<template>
  <div class="card">
    <div class="card-head">
      <h2>结构可视化（数组顺序视图）</h2>
      <div class="row">
        <label class="inline">
          <span>bin_start</span>
          <input v-model.number="snapForm.bin_start" type="number" min="0" step="10" />
        </label>
        <label class="inline">
          <span>bin_count</span>
          <input v-model.number="snapForm.bin_count" type="number" min="1" max="200" step="10" />
        </label>
        <button @click="$emit('refreshSnapshot')" :disabled="busy">刷新快照</button>
      </div>
    </div>

    <canvas ref="canvasEl" class="canvas" width="1000" height="340"></canvas>
    <div class="legend">
      <span class="chip empty">空</span>
      <span class="chip used">占用</span>
      <span class="chip fb">fallback</span>
      <span class="chip kick">kick path</span>
    </div>
    <div class="hint-block">坐标含义：横轴为 bin 内 slot 的顺序（mini-bin 在横向连续），纵轴为 bin index。</div>
  </div>
</template>

<script setup lang="ts">
import { onMounted, ref, watch } from 'vue'

const props = defineProps<{
  busy: boolean
  snapForm: { bin_start: number; bin_count: number }
  snapshot: any
  params: any
  lastTraceIdx: Set<number>
}>()

defineEmits<{
  (e: 'refreshSnapshot'): void
}>()

const canvasEl = ref<HTMLCanvasElement | null>(null)

function renderCanvas() {
  const c = canvasEl.value
  if (!c || !props.snapshot) return
  const ctx = c.getContext('2d')
  if (!ctx) return

  const binSize = Number(props.snapshot.bin_size ?? 0)
  const miniTotal = Number(props.snapshot.mini_total ?? 0)
  const numMiniBins = Number(props.params?.num_mini_bins ?? 0)
  const miniBinSize = Number(props.params?.mini_bin_size ?? 0)
  const binStart = Number(props.snapshot.bin_start ?? 0)
  const binCount = Number(props.snapshot.bin_count ?? 0)
  const slots: Array<{ idx: number; is_used: number; kick_depth: number }> = props.snapshot.slots ?? []

  ctx.clearRect(0, 0, c.width, c.height)
  if (!binSize || !binCount) return

  const topRulerH = 44
  const rightRulerW = 110
  const padL = 10
  const padT = 10

  const gridW = Math.max(10, c.width - padL - rightRulerW - 10)
  const gridH = Math.max(10, c.height - padT - topRulerH - 10)

  const cellW = Math.max(2, Math.floor(gridW / binSize))
  const cellH = Math.max(2, Math.floor(gridH / binCount))

  const usedColor = '#2563eb'
  const fbColor = '#f59e0b'
  const emptyColor = '#ffffff'
  const kickColor = '#ef4444'

  const gridLight = 'rgba(255,255,255,0.06)'
  const gridStrong = 'rgba(255,255,255,0.16)'
  const gridMini = 'rgba(34,211,238,0.18)'
  const gridFallback = 'rgba(245,158,11,0.30)'

  ctx.fillStyle = 'rgba(0,0,0,0.12)'
  ctx.fillRect(0, 0, c.width, c.height)

  const used = new Set<number>()
  for (const r of slots) {
    const idx = Number(r.idx)
    if (Number(r.is_used) === 1) used.add(idx)
  }

  const gridX0 = padL
  const gridY0 = padT + topRulerH

  for (let by = 0; by < binCount; by++) {
    const b = binStart + by
    const baseIdx = b * binSize
    for (let sx = 0; sx < binSize; sx++) {
      const idx = baseIdx + sx
      const x = gridX0 + sx * cellW
      const y = gridY0 + by * cellH

      let fill = emptyColor
      if (used.has(idx)) {
        fill = sx >= miniTotal ? fbColor : usedColor
      }
      if (props.lastTraceIdx.has(idx)) fill = kickColor

      ctx.fillStyle = fill
      ctx.fillRect(x, y, cellW, cellH)
    }
  }

  ctx.strokeStyle = gridLight
  ctx.lineWidth = 1

  for (let sx = 0; sx <= binSize; sx++) {
    const x = gridX0 + sx * cellW + 0.5
    ctx.beginPath()
    ctx.moveTo(x, gridY0)
    ctx.lineTo(x, gridY0 + binCount * cellH)
    ctx.stroke()
  }

  for (let by = 0; by <= binCount; by++) {
    const y = gridY0 + by * cellH + 0.5
    ctx.beginPath()
    ctx.moveTo(gridX0, y)
    ctx.lineTo(gridX0 + binSize * cellW, y)
    ctx.stroke()
  }

  if (miniBinSize > 0) {
    ctx.strokeStyle = gridMini
    ctx.lineWidth = 2
    for (let sx = 0; sx <= miniTotal; sx += miniBinSize) {
      const x = gridX0 + sx * cellW + 0.5
      ctx.beginPath()
      ctx.moveTo(x, gridY0)
      ctx.lineTo(x, gridY0 + binCount * cellH)
      ctx.stroke()
    }
  }

  ctx.strokeStyle = gridFallback
  ctx.lineWidth = 3
  {
    const x = gridX0 + miniTotal * cellW + 0.5
    ctx.beginPath()
    ctx.moveTo(x, gridY0)
    ctx.lineTo(x, gridY0 + binCount * cellH)
    ctx.stroke()
  }

  ctx.strokeStyle = gridStrong
  ctx.lineWidth = 2
  ctx.strokeRect(gridX0 + 0.5, gridY0 + 0.5, binSize * cellW - 1, binCount * cellH - 1)

  ctx.fillStyle = 'rgba(255,255,255,0.62)'
  ctx.font =
    '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace'
  ctx.fillText(`bins [${binStart}..${binStart + binCount - 1}]  bin_size=${binSize}`, gridX0, padT + 14)

  const rulerY = padT + 34
  ctx.fillStyle = 'rgba(255,255,255,0.75)'
  if (miniBinSize > 0 && numMiniBins > 0) {
    for (let i = 0; i < numMiniBins; i++) {
      const x0 = gridX0 + i * miniBinSize * cellW
      const x1 = gridX0 + (i + 1) * miniBinSize * cellW
      if (x0 >= gridX0 + miniTotal * cellW) break
      const xm = (x0 + x1) / 2
      ctx.fillText(`mini-bin #${i}`, xm - 36, rulerY)
    }
  }
  {
    const x0 = gridX0 + miniTotal * cellW
    const x1 = gridX0 + binSize * cellW
    const xm = (x0 + x1) / 2
    ctx.fillStyle = 'rgba(255,255,255,0.78)'
    ctx.fillText('fallback', xm - 24, rulerY)
  }

  const rulerX = gridX0 + binSize * cellW + 10
  ctx.strokeStyle = 'rgba(255,255,255,0.18)'
  ctx.lineWidth = 2
  ctx.beginPath()
  ctx.moveTo(rulerX + 0.5, gridY0)
  ctx.lineTo(rulerX + 0.5, gridY0 + binCount * cellH)
  ctx.stroke()

  ctx.fillStyle = 'rgba(255,255,255,0.72)'
  const step = binCount > 30 ? 5 : 1
  for (let by = 0; by < binCount; by += step) {
    const y = gridY0 + by * cellH + Math.min(14, cellH)
    ctx.fillText(`bin ${binStart + by}`, rulerX + 10, y)
  }
}

onMounted(() => renderCanvas())
watch(
  () => [props.snapshot, props.params, props.lastTraceIdx],
  () => renderCanvas(),
  { deep: true },
)
</script>

