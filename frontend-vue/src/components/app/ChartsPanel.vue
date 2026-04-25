<template>
  <div class="grid2">
    <div class="card">
      <h2>状态统计</h2>
      <div class="stats">
        <div class="stat">
          <div class="k">used_slots</div>
          <div class="v">{{ stats?.used_slots ?? '-' }}</div>
        </div>
        <div class="stat">
          <div class="k">fallback_used</div>
          <div class="v">{{ stats?.fallback_used ?? '-' }}</div>
        </div>
        <div class="stat">
          <div class="k">capacity</div>
          <div class="v">{{ stats?.capacity_slots ?? '-' }}</div>
        </div>
        <div class="stat">
          <div class="k">load</div>
          <div class="v">{{ (stats?.load_factor ?? 0).toFixed?.(4) ?? '-' }}</div>
        </div>
      </div>
      <div ref="chartEl" class="chart"></div>
    </div>

    <div class="card">
      <h2>
        Kick 深度与实验曲线
        <span
          class="tip"
          title="上图：当前表中已放置元素的 kick_depth 直方图（反映插入时的重排深度分布）。下图：你点击“实验”按钮得到的对比曲线（会重建表来跑不同参数）。"
        >?</span>
      </h2>
      <div ref="kickHistEl" class="chart"></div>
      <div ref="expEl" class="chart"></div>
    </div>
  </div>
</template>

<script setup lang="ts">
import * as echarts from 'echarts'
import { onBeforeUnmount, onMounted, ref, watch } from 'vue'

const props = defineProps<{
  stats: any
  history: Array<{ used: number; fb: number }>
  kickHist: Array<{ depth: number; count: number }>
  experiment: { kind: 'fallback_vs_load' | 'probe_vs_n'; payload: any } | null
}>()

const chartEl = ref<HTMLDivElement | null>(null)
let chart: echarts.ECharts | null = null

const kickHistEl = ref<HTMLDivElement | null>(null)
let kickChart: echarts.ECharts | null = null

const expEl = ref<HTMLDivElement | null>(null)
let expChart: echarts.ECharts | null = null

function renderStatsChart() {
  if (!chartEl.value) return
  if (!chart) chart = echarts.init(chartEl.value)
  const xs = props.history.map((_, i) => i + 1)
  chart.setOption({
    tooltip: { trigger: 'axis' },
    legend: { data: ['used_slots', 'fallback_used'] },
    xAxis: { type: 'category', data: xs },
    yAxis: { type: 'value' },
    series: [
      { name: 'used_slots', type: 'line', data: props.history.map((p) => p.used), smooth: true },
      { name: 'fallback_used', type: 'line', data: props.history.map((p) => p.fb), smooth: true },
    ],
  })
}

function renderKickHist() {
  if (!kickHistEl.value) return
  if (!kickChart) kickChart = echarts.init(kickHistEl.value)
  const hist = props.kickHist ?? []
  kickChart.setOption({
    title: { text: 'Kick 深度分布', left: 'left' },
    tooltip: { trigger: 'axis' },
    xAxis: { type: 'category', data: hist.map((h) => String(h.depth)) },
    yAxis: { type: 'value' },
    series: [
      {
        type: 'bar',
        data: hist.map((h) => h.count),
        barMaxWidth: 20,
        itemStyle: { color: '#2563eb', borderRadius: [3, 3, 0, 0] },
      },
    ],
  })
}

function renderExperiment() {
  if (!expEl.value) return
  if (!expChart) expChart = echarts.init(expEl.value)
  const exp = props.experiment
  if (!exp) {
    expChart.clear()
    return
  }

  if (exp.kind === 'fallback_vs_load') {
    const pts: Array<any> = exp.payload
    expChart.setOption({
      title: { text: 'Fallback 使用率 vs Load factor', left: 'left' },
      tooltip: { trigger: 'axis' },
      legend: { data: ['fallback_ratio', 'fallback_used'] },
      xAxis: { type: 'category', data: pts.map((p) => Number(p.load).toFixed(3)) },
      yAxis: [{ type: 'value' }, { type: 'value' }],
      series: [
        { name: 'fallback_ratio', type: 'line', data: pts.map((p) => p.fallback_ratio), smooth: true, yAxisIndex: 0 },
        { name: 'fallback_used', type: 'line', data: pts.map((p) => p.fallback_used), smooth: true, yAxisIndex: 1 },
      ],
    })
    return
  }

  const series: Array<any> = exp.payload
  expChart.setOption({
    title: { text: 'Probe vs n（avg / p99）', left: 'left' },
    tooltip: { trigger: 'axis' },
    legend: { data: series.flatMap((s) => [`k=${s.k} avg`, `k=${s.k} p99`]) },
    xAxis: { type: 'category', data: (series[0]?.points ?? []).map((p: any) => String(p.n)) },
    yAxis: { type: 'value' },
    series: series.flatMap((s) => [
      { name: `k=${s.k} avg`, type: 'line', data: s.points.map((p: any) => p.avg_probes ?? null), smooth: true },
      { name: `k=${s.k} p99`, type: 'line', data: s.points.map((p: any) => p.p99_probes ?? null), smooth: true },
    ]),
  })
}

function onResize() {
  chart?.resize()
  kickChart?.resize()
  expChart?.resize()
}

onMounted(() => {
  renderStatsChart()
  renderKickHist()
  renderExperiment()
  window.addEventListener('resize', onResize)
})

onBeforeUnmount(() => {
  window.removeEventListener('resize', onResize)
  chart?.dispose()
  chart = null
  kickChart?.dispose()
  kickChart = null
  expChart?.dispose()
  expChart = null
})

watch(
  () => props.history,
  () => renderStatsChart(),
  { deep: true },
)
watch(
  () => props.kickHist,
  () => renderKickHist(),
  { deep: true },
)
watch(
  () => props.experiment,
  () => renderExperiment(),
  { deep: true },
)
</script>

