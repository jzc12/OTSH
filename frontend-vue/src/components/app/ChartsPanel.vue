<template>
  <div class="charts-root">
    <div class="grid2">
      <div class="card">
        <h2>耗时</h2>
        <div class="stats">
          <div class="stat">
            <div class="k">query p50</div>
            <div class="v">{{ fmtNs(stats?.metrics?.ht_ns?.query?.p50_ns) }}</div>
          </div>
          <div class="stat">
            <div class="k">query p99</div>
            <div class="v">{{ fmtNs(stats?.metrics?.ht_ns?.query?.p99_ns) }}</div>
          </div>
          <div class="stat">
            <div class="k">insert p99</div>
            <div class="v">{{ fmtNs(stats?.metrics?.ht_ns?.insert?.p99_ns) }}</div>
          </div>
          <div class="stat">
            <div class="k">delete p99</div>
            <div class="v">{{ fmtNs(stats?.metrics?.ht_ns?.delete?.p99_ns) }}</div>
          </div>
        </div>
        <div ref="chartEl" class="chart"></div>
      </div>

      <div class="card">
        <h2>空间与时空折中</h2>
        <div ref="expEl" class="chart"></div>
      </div>
    </div>

    <div v-if="hasAnalytics" class="grid3 mt-12">
      <div class="card">
        <h2>DB：probe 分布</h2>
        <p class="hint sm">metrics 表按 probe_count 聚合（当前所选 snapshot）</p>
        <div ref="probeHistEl" class="chart chart-sm"></div>
      </div>
      <div class="card">
        <h2>DB：kick 分布</h2>
        <p class="hint sm">metrics 表按 kick_count 聚合</p>
        <div ref="kickHistEl" class="chart chart-sm"></div>
      </div>
      <div class="card">
        <h2>DB：tier 与负载</h2>
        <p class="hint sm">左：按 cubby_tier 平均 probe；右：cubby 表按 tier 平均利用率</p>
        <div ref="tierEl" class="chart chart-sm"></div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import * as echarts from 'echarts'
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'

const props = defineProps<{
  stats: any
  history: Array<{ used: number; fb: number }>
  analyticsDb: Record<string, any> | null
  experiment:
    | { kind: 'fallback_vs_load' | 'probe_vs_n'; payload: any }
    | { kind: 'o1_vs_n'; payload: any; k: number }
    | { kind: 'ok_vs_k'; payload: any; n: number }
    | null
}>()

const hasAnalytics = computed(() => {
  const a = props.analyticsDb
  return !!(a && (a.probe_hist?.length || a.kick_hist?.length || a.probe_by_tier?.length || a.cubby_load_by_tier?.length))
})

const chartEl = ref<HTMLDivElement | null>(null)
let chart: echarts.ECharts | null = null

const expEl = ref<HTMLDivElement | null>(null)
let expChart: echarts.ECharts | null = null

const probeHistEl = ref<HTMLDivElement | null>(null)
let probeHistChart: echarts.ECharts | null = null

const kickHistEl = ref<HTMLDivElement | null>(null)
let kickHistChart: echarts.ECharts | null = null

const tierEl = ref<HTMLDivElement | null>(null)
let tierChart: echarts.ECharts | null = null

function fmtNs(v: any) {
  const x = Number(v)
  if (!Number.isFinite(x)) return '-'
  if (x < 1000) return `${x.toFixed(0)} ns`
  if (x < 1e6) return `${(x / 1000).toFixed(2)} µs`
  return `${(x / 1e6).toFixed(2)} ms`
}

function renderStatsChart() {
  if (!chartEl.value) return
  if (!chart) chart = echarts.init(chartEl.value)
  const xs = props.history.map((_, i) => i + 1)
  const q = props.history.map((p) => p.used)
  const ins = props.history.map((p) => p.fb)
  chart.setOption({
    tooltip: { trigger: 'axis' },
    legend: { data: ['n', 'router_steps_max'] },
    xAxis: { type: 'category', data: xs },
    yAxis: { type: 'value' },
    series: [
      { name: 'n', type: 'line', data: q, smooth: true },
      { name: 'router_steps_max', type: 'line', data: ins, smooth: true },
    ],
  })
}

function renderExperiment() {
  if (!expEl.value) return
  if (!expChart) expChart = echarts.init(expEl.value)
  const exp = props.experiment
  if (!exp) {
    const s = props.stats
    const wasted = Number(s?.space_bits?.wasted_bits_per_key_est ?? 0)
    const n = Number(s?.state?.n ?? 0)
    const ins_p99 = Number(s?.metrics?.ht_ns?.insert?.p99_ns ?? 0)
    const del_p99 = Number(s?.metrics?.ht_ns?.delete?.p99_ns ?? 0)
    expChart.setOption({
      title: { text: 'wasted bits/key', left: 'left' },
      tooltip: { trigger: 'axis' },
      legend: { data: ['wasted_bits_per_key', 'insert_p99_ns', 'delete_p99_ns'] },
      xAxis: { type: 'category', data: [String(n)] },
      yAxis: [{ type: 'value' }, { type: 'value' }],
      series: [
        { name: 'wasted_bits_per_key', type: 'bar', data: [wasted], yAxisIndex: 0 },
        { name: 'insert_p99_ns', type: 'line', data: [ins_p99], yAxisIndex: 1 },
        { name: 'delete_p99_ns', type: 'line', data: [del_p99], yAxisIndex: 1 },
      ],
    })
    return
  }

  if (exp.kind === 'o1_vs_n') {
    const pts: any[] = exp.payload ?? []
    expChart.setOption({
      title: { text: `query O(1) vs n (k=${exp.k})`, left: 'left' },
      tooltip: { trigger: 'axis' },
      legend: { data: ['p50_ns', 'p99_ns'] },
      xAxis: { type: 'category', data: pts.map((p) => String(p.n)) },
      yAxis: { type: 'value' },
      series: [
        { name: 'p50_ns', type: 'line', data: pts.map((p) => p.query_p50_ns), smooth: true },
        { name: 'p99_ns', type: 'line', data: pts.map((p) => p.query_p99_ns), smooth: true },
      ],
    })
    return
  }

  if (exp.kind === 'ok_vs_k') {
    const pts: any[] = exp.payload ?? []
    expChart.setOption({
      title: { text: `time-space tradeoff (n=${exp.n})`, left: 'left' },
      tooltip: { trigger: 'axis' },
      legend: { data: ['insert_p99_ns', 'delete_p99_ns', 'wasted_bits/key'] },
      xAxis: { type: 'category', data: pts.map((p) => String(p.k)) },
      yAxis: [{ type: 'value' }, { type: 'value' }],
      series: [
        { name: 'insert_p99_ns', type: 'line', data: pts.map((p) => p.insert_p99_ns), yAxisIndex: 0, smooth: true },
        { name: 'delete_p99_ns', type: 'line', data: pts.map((p) => p.delete_p99_ns), yAxisIndex: 0, smooth: true },
        {
          name: 'wasted_bits/key',
          type: 'bar',
          data: pts.map((p) => p.wasted_bpk ?? p.wasted_bits_per_key_est ?? 0),
          yAxisIndex: 1,
        },
      ],
    })
    return
  }
}

function renderAnalyticsCharts() {
  const a = props.analyticsDb
  if (!a) {
    probeHistChart?.clear()
    kickHistChart?.clear()
    tierChart?.clear()
    return
  }

  if (probeHistEl.value) {
    if (!probeHistChart) probeHistChart = echarts.init(probeHistEl.value)
    const rows: any[] = a.probe_hist ?? []
    probeHistChart.setOption({
      tooltip: { trigger: 'axis' },
      xAxis: { type: 'category', data: rows.map((r) => String(r.probe_count)) },
      yAxis: { type: 'value' },
      series: [{ name: 'count', type: 'bar', data: rows.map((r) => r.count) }],
    })
  }
  if (kickHistEl.value) {
    if (!kickHistChart) kickHistChart = echarts.init(kickHistEl.value)
    const rows: any[] = a.kick_hist ?? []
    kickHistChart.setOption({
      tooltip: { trigger: 'axis' },
      xAxis: { type: 'category', data: rows.map((r) => String(r.kick_count)) },
      yAxis: { type: 'value' },
      series: [{ name: 'count', type: 'bar', data: rows.map((r) => r.count) }],
    })
  }
  if (tierEl.value) {
    if (!tierChart) tierChart = echarts.init(tierEl.value)
    const pt: any[] = a.probe_by_tier ?? []
    const lf: any[] = a.cubby_load_by_tier ?? []
    const tierSet = new Set<number>()
    for (const r of pt) tierSet.add(Number(r.cubby_tier))
    for (const r of lf) tierSet.add(Number(r.tier))
    const tiers = Array.from(tierSet).sort((x, y) => x - y)
    const probeMap = new Map<number, number>()
    for (const r of pt) probeMap.set(Number(r.cubby_tier), Number(r.avg_probe))
    const lfMap = new Map<number, number>()
    for (const r of lf) lfMap.set(Number(r.tier), Number(r.avg_load_factor))
    const labels = tiers.map((t) => `tier ${t}`)
    tierChart.setOption({
      tooltip: { trigger: 'axis' },
      legend: { data: ['avg_probe', 'avg_load_factor'] },
      xAxis: { type: 'category', data: labels },
      yAxis: [{ type: 'value', name: 'probe' }, { type: 'value', name: 'LF' }],
      series: [
        {
          name: 'avg_probe',
          type: 'line',
          data: tiers.map((t) => probeMap.get(t) ?? null),
          yAxisIndex: 0,
          smooth: true,
        },
        {
          name: 'avg_load_factor',
          type: 'bar',
          data: tiers.map((t) => lfMap.get(t) ?? null),
          yAxisIndex: 1,
        },
      ],
    })
  }
}

function onResize() {
  chart?.resize()
  expChart?.resize()
  probeHistChart?.resize()
  kickHistChart?.resize()
  tierChart?.resize()
}

onMounted(() => {
  renderStatsChart()
  renderExperiment()
  renderAnalyticsCharts()
  window.addEventListener('resize', onResize)
})

onBeforeUnmount(() => {
  window.removeEventListener('resize', onResize)
  chart?.dispose()
  chart = null
  expChart?.dispose()
  expChart = null
  probeHistChart?.dispose()
  probeHistChart = null
  kickHistChart?.dispose()
  kickHistChart = null
  tierChart?.dispose()
  tierChart = null
})

watch(
  () => props.history,
  () => renderStatsChart(),
  { deep: true },
)
watch(
  () => props.stats,
  () => renderExperiment(),
  { deep: true },
)
watch(
  () => props.analyticsDb,
  async () => {
    await nextTick()
    renderAnalyticsCharts()
  },
  { deep: true },
)
watch(hasAnalytics, async (v) => {
  if (!v) return
  await nextTick()
  renderAnalyticsCharts()
})
watch(
  () => props.experiment,
  () => renderExperiment(),
  { deep: true },
)
</script>

<style scoped>
.charts-root {
  display: flex;
  flex-direction: column;
  gap: 0;
}
.mt-12 {
  margin-top: 14px;
}
.hint.sm {
  font-size: 11px;
  margin: 0 0 8px;
  color: rgba(255, 255, 255, 0.55);
}
.chart {
  height: 260px;
  margin-top: 10px;
}
.chart-sm {
  height: 220px;
}
</style>
