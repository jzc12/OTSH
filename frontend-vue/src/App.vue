<template>
  <div class="app">
    <div v-if="busy" class="overlay">
      <div class="overlay-card">
        <div class="spinner"></div>
        <div class="overlay-text">
          <div class="overlay-title">{{ busyTitle }}</div>
          <div class="overlay-sub">{{ busyHint }}</div>
          <div class="overlay-sub mono" v-if="elapsedMs > 0">已运行：{{ Math.floor(elapsedMs / 1000) }}s</div>
        </div>
      </div>
    </div>

    <header class="header">
      <div class="title">
        <p class="subtitle">On the Optimal Time/Space Tradeoff for Hash Tables（可视化验证系统）</p>
      </div>
      <div class="pill">
        <div class="pill-k">Backend</div>
        <div class="pill-v mono">{{ backendBase }}</div>
      </div>
    </header>

    <section class="layout">
      <aside class="sidebar">
        <div class="card">
          <h2>参数控制</h2>
          <div class="grid grid-4">
          <label>
            <span>n</span>
            <input v-model.number="initForm.n" type="number" min="1000" step="1000" />
          </label>
          <label>
            <span>k</span>
            <input v-model.number="initForm.k" type="number" min="0" max="4" step="1" />
          </label>
          <label>
            <span>load factor</span>
            <input v-model.number="initForm.load_factor" type="number" min="0.7" max="0.99" step="0.01" />
          </label>
          <label>
            <span>distribution</span>
            <select v-model="distForm.distribution">
              <option value="uniform">uniform</option>
              <option value="skewed">skewed</option>
            </select>
          </label>
        </div>

          <div class="row">
            <button class="primary" @click="initTable" :disabled="busy">初始化</button>
            <button @click="refreshStats" :disabled="busy">刷新统计</button>
            <button @click="refreshKickHist" :disabled="busy">kick 深度分布</button>
          </div>

          <div class="kv mono" v-if="params">
            <div class="kv-row"><span class="kv-k">total_bins</span><span class="kv-v">{{ params.total_bins }}</span></div>
            <div class="kv-row"><span class="kv-k">bin_size</span><span class="kv-v">{{ params.bin_size }}</span></div>
            <div class="kv-row"><span class="kv-k">mini_bin_size</span><span class="kv-v">{{ params.mini_bin_size }}</span></div>
            <div class="kv-row"><span class="kv-k">num_mini_bins</span><span class="kv-v">{{ params.num_mini_bins }}</span></div>
            <div class="kv-row"><span class="kv-k">capacity_slots</span><span class="kv-v">{{ params.capacity_slots }}</span></div>
          </div>
        </div>

        <div class="card">
          <h2>理论复杂度</h2>
          <div class="complex2">
            <div class="complex-col">
              <div class="complex-title">理论</div>
              <div class="complex">
                <div class="complex-row">
                  <div class="complex-k">查询（probe 次数）</div>
                  <div class="complex-v mono">O(1)</div>
                </div>
                <div class="complex-row">
                  <div class="complex-k">查询（扫描成本）</div>
                  <div class="complex-v mono">
                    O(log<sup>(k)</sup> n) ≈ O({{ miniBinSizeDisplay }})
                  </div>
                </div>
                <div class="complex-row">
                  <div class="complex-k">插入 / 删除</div>
                  <div class="complex-v mono">期望 O(k)</div>
                </div>
                <div class="complex-row">
                  <div class="complex-k">空间浪费</div>
                  <div class="complex-v mono">O(log<sup>(k)</sup> n) bits / key</div>
                </div>
              </div>
            </div>

            <div class="complex-col">
              <div class="complex-title">
                真实运行时间（本机）
                <span
                  class="tip"
                  title="这里是前端测得的端到端耗时（含网络与后端 SQL）。实验接口会重建表，耗时会明显更大。"
                >?</span>
              </div>
              <div class="kv mono">
                <div class="kv-row"><span class="kv-k">init</span><span class="kv-v">{{ fmtMs(rt.init_ms) }}</span></div>
                <div class="kv-row"><span class="kv-k">insert(单步)</span><span class="kv-v">{{ fmtMs(rt.insert_ms) }}</span></div>
                <div class="kv-row"><span class="kv-k">find</span><span class="kv-v">{{ fmtMs(rt.find_ms) }}</span></div>
                <div class="kv-row"><span class="kv-k">erase</span><span class="kv-v">{{ fmtMs(rt.erase_ms) }}</span></div>
                <div class="kv-row"><span class="kv-k">batch_insert</span><span class="kv-v">{{ fmtMs(rt.batch_insert_ms) }}</span></div>
                <div class="kv-row"><span class="kv-k">query_test</span><span class="kv-v">{{ fmtMs(rt.query_test_ms) }}</span></div>
                <div class="kv-row"><span class="kv-k">snapshot</span><span class="kv-v">{{ fmtMs(rt.snapshot_ms) }}</span></div>
              </div>
            </div>
          </div>
        </div>

        <div class="card">
          <h2>单步操作</h2>
          <div class="row mt-8">
            <input v-model.number="key" type="number" placeholder="key (uint64)" />
          </div>
          <div class="row one-line mt-10">
            <button class="primary" @click="insertOne" :disabled="busy">插入</button>
            <button @click="findOne" :disabled="busy">查询</button>
            <button @click="eraseOne" :disabled="busy">删除</button>
          </div>
          <div class="row mt-8">
            <span class="hint">结果：</span>
            <span class="mono">{{ opResult }}</span>
          </div>
        </div>

        <div class="card">
          <h2>批量插入 / 查询测试</h2>
          <div class="grid grid-4 mt-8">
          <label>
            <span>batch count</span>
            <input v-model.number="batchForm.count" type="number" min="1" step="100" />
          </label>
          <label>
            <span>
              skew
            </span>
            <input v-model.number="batchForm.skew" type="number" min="1.01" step="0.05" />
          </label>
          <label>
            <span>query count</span>
            <input v-model.number="queryForm.count" type="number" min="100" step="100" />
          </label>
          <label>
            <span>hit_rate</span>
            <input v-model.number="queryForm.hit_rate" type="number" min="0" max="1" step="0.1" />
          </label>
        </div>
        <div class="row mt-10">
          <button class="primary" @click="batchInsert" :disabled="busy">批量插入</button>
          <button @click="queryTest" :disabled="busy">查询测试</button>
        </div>
        <div class="row mt-8">
          <button class="warn" @click="runFallbackVsLoad" :disabled="busy">实验：fallback vs load</button>
          <button class="warn" @click="runProbeVsN" :disabled="busy">实验：probe vs n</button>
        </div>

      </div>
      </aside>

      <main class="content">
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
              <button @click="refreshSnapshot" :disabled="busy">刷新快照</button>
            </div>
          </div>
          <canvas ref="canvasEl" class="canvas" width="1000" height="340"></canvas>
          <div class="legend">
            <span class="chip empty">空</span>
            <span class="chip used">占用</span>
            <span class="chip fb">fallback</span>
            <span class="chip kick">kick path</span>
          </div>
          <div class="hint-block">
            坐标含义：横轴为 bin 内 slot 的顺序（mini-bin 在横向连续），纵轴为 bin index。
          </div>
        </div>

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
      </main>
    </section>
  </div>
</template>

<script setup lang="ts">
import axios from 'axios'
import * as echarts from 'echarts'
import { computed, onMounted, onBeforeUnmount, ref, watch } from 'vue'

const backendBase = 'http://127.0.0.1:8080'

const busy = ref(false)
const busyTitle = ref('处理中...')
const busyHint = ref('这是一个可能较慢的数据库操作，请稍等。')
const startedAt = ref<number>(0)
const elapsedMs = ref<number>(0)
let tickTimer: number | null = null
const key = ref<number>(1)
const opResult = ref<string>('-')

const initForm = ref({
  n: 100000,
  k: 2,
  load_factor: 0.98,
})

const distForm = ref({
  distribution: 'uniform' as 'uniform' | 'skewed',
})

const batchForm = ref({
  count: 5000,
  skew: 1.2,
})

const queryForm = ref({
  count: 5000,
  hit_rate: 0.5,
})

const snapForm = ref({
  bin_start: 0,
  bin_count: 60,
})

const params = ref<any>(null)
const stats = ref<any>(null)
const lastTraceIdx = ref<Set<number>>(new Set())
const snapshot = ref<any>(null)

const canvasEl = ref<HTMLCanvasElement | null>(null)

const chartEl = ref<HTMLDivElement | null>(null)
let chart: echarts.ECharts | null = null
const history = ref<Array<{ used: number; fb: number }>>([])

const kickHistEl = ref<HTMLDivElement | null>(null)
let kickChart: echarts.ECharts | null = null

const expEl = ref<HTMLDivElement | null>(null)
let expChart: echarts.ECharts | null = null

const miniBinSizeDisplay = computed(() => {
  const v = params.value?.mini_bin_size
  if (typeof v === 'number') return v
  if (typeof v === 'string') return Number(v) || 0
  return 0
})

const rt = ref({
  init_ms: null as number | null,
  insert_ms: null as number | null,
  find_ms: null as number | null,
  erase_ms: null as number | null,
  batch_insert_ms: null as number | null,
  query_test_ms: null as number | null,
  snapshot_ms: null as number | null,
})

function fmtMs(v: number | null) {
  if (v == null) return '-'
  if (v < 1000) return `${v.toFixed(1)} ms`
  return `${(v / 1000).toFixed(2)} s`
}

async function initTable() {
  busyTitle.value = '初始化中...'
  busyHint.value = '会预创建所有槽位（数据量大时可能需要几十秒到几分钟）。'
  beginBusy()
  opResult.value = 'initializing...'
  try {
    const t0 = performance.now()
    const { data } = await axios.post('/api/init', initForm.value)
    rt.value.init_ms = performance.now() - t0
    params.value = data.params
    opResult.value = data.ok ? 'init ok' : `init failed: ${data.error ?? ''}`
    history.value = []
    lastTraceIdx.value = new Set()
    await refreshStats()
    await refreshSnapshot()
    await refreshKickHist()
  } catch (e: any) {
    opResult.value = `init error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

async function refreshStats() {
  busyTitle.value = '刷新统计...'
  busyHint.value = '正在读取全表统计。'
  beginBusy()
  try {
    const { data } = await axios.get('/api/stats')
    stats.value = data
    history.value.push({ used: data.used_slots ?? 0, fb: data.fallback_used ?? 0 })
    renderChart()
  } catch (e: any) {
    opResult.value = `stats error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

async function insertOne() {
  busyTitle.value = '单步插入中...'
  busyHint.value = '将返回 kick chain，用于 Canvas 高亮路径。'
  beginBusy()
  try {
    const t0 = performance.now()
    const { data } = await axios.post('/api/insert', { key: key.value, trace: true })
    rt.value.insert_ms = performance.now() - t0
    opResult.value = data.ok ? `insert ok (probes=${data.probes})` : `insert fail (probes=${data.probes})`
    const s = new Set<number>()
    if (Array.isArray(data.trace)) {
      for (const t of data.trace) s.add(Number(t.idx))
    }
    lastTraceIdx.value = s
    await refreshStats()
    await refreshSnapshot()
    await refreshKickHist()
  } catch (e: any) {
    opResult.value = `insert error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

async function findOne() {
  busyTitle.value = '查询中...'
  busyHint.value = '正在扫描偏好 mini-bin + fallback。'
  beginBusy()
  try {
    const t0 = performance.now()
    const { data } = await axios.post('/api/find', { key: key.value })
    rt.value.find_ms = performance.now() - t0
    opResult.value = data.found ? `found (probes=${data.probes})` : `not found (probes=${data.probes})`
  } catch (e: any) {
    opResult.value = `find error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

async function eraseOne() {
  busyTitle.value = '删除中...'
  busyHint.value = '正在从 mini-bin / fallback 删除。'
  beginBusy()
  try {
    const t0 = performance.now()
    const { data } = await axios.post('/api/erase', { key: key.value })
    rt.value.erase_ms = performance.now() - t0
    opResult.value = data.ok ? `erase ok (probes=${data.probes})` : `erase miss (probes=${data.probes})`
    await refreshStats()
    await refreshSnapshot()
    await refreshKickHist()
  } catch (e: any) {
    opResult.value = `erase error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

async function batchInsert() {
  busyTitle.value = '批量插入中...'
  busyHint.value = '数据量大时会比较久；可看后端终端日志的 progress 输出。'
  beginBusy()
  try {
    const t0 = performance.now()
    const { data } = await axios.post('/api/batch_insert', {
      count: batchForm.value.count,
      distribution: distForm.value.distribution,
      skew: batchForm.value.skew,
    })
    rt.value.batch_insert_ms = performance.now() - t0
    opResult.value = `batch ok_count=${data.ok_count}/${data.total} avg_probes=${Number(data.avg_probes).toFixed(3)}`
    await refreshStats()
    await refreshSnapshot()
    await refreshKickHist()
  } catch (e: any) {
    opResult.value = `batch error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

async function queryTest() {
  busyTitle.value = '查询测试中...'
  busyHint.value = '正在执行多次查询并统计 avg/p99/max probe。'
  beginBusy()
  try {
    const t0 = performance.now()
    const { data } = await axios.post('/api/query_test', {
      count: queryForm.value.count,
      hit_rate: queryForm.value.hit_rate,
    })
    rt.value.query_test_ms = performance.now() - t0
    opResult.value = `query avg=${Number(data.avg_probes).toFixed(3)} p99=${data.p99_probes} max=${data.max_probes}`
  } catch (e: any) {
    opResult.value = `query_test error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

async function refreshSnapshot() {
  try {
    const t0 = performance.now()
    const { data } = await axios.get('/api/snapshot', { params: snapForm.value })
    rt.value.snapshot_ms = performance.now() - t0
    snapshot.value = data
    renderCanvas()
  } catch (e: any) {
    opResult.value = `snapshot error: ${e?.message ?? e}`
  }
}

function renderCanvas() {
  const c = canvasEl.value
  if (!c || !snapshot.value) return
  const ctx = c.getContext('2d')
  if (!ctx) return

  const binSize = Number(snapshot.value.bin_size ?? 0)
  const miniTotal = Number(snapshot.value.mini_total ?? 0)
  const numMiniBins = Number(params.value?.num_mini_bins ?? 0)
  const miniBinSize = Number(params.value?.mini_bin_size ?? 0)
  const binStart = Number(snapshot.value.bin_start ?? 0)
  const binCount = Number(snapshot.value.bin_count ?? 0)
  const slots: Array<{ idx: number; is_used: number; kick_depth: number }> = snapshot.value.slots ?? []

  ctx.clearRect(0, 0, c.width, c.height)

  if (!binSize || !binCount) return

  // Match array layout more directly:
  // x-axis = slot index within a bin (0..bin_size-1), y-axis = bin index.
  // mini-bins are contiguous segments along the x-axis.
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

  // draw grid background
  ctx.fillStyle = 'rgba(0,0,0,0.12)'
  ctx.fillRect(0, 0, c.width, c.height)

  // map idx -> state
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
      if (lastTraceIdx.value.has(idx)) fill = kickColor

      ctx.fillStyle = fill
      ctx.fillRect(x, y, cellW, cellH)
    }
  }

  // grid lines: cell grid (light)
  ctx.strokeStyle = gridLight
  ctx.lineWidth = 1

  // vertical grid (slots within bin)
  for (let sx = 0; sx <= binSize; sx++) {
    const x = gridX0 + sx * cellW + 0.5
    ctx.beginPath()
    ctx.moveTo(x, gridY0)
    ctx.lineTo(x, gridY0 + binCount * cellH)
    ctx.stroke()
  }

  // horizontal grid (bins)
  for (let by = 0; by <= binCount; by++) {
    const y = gridY0 + by * cellH + 0.5
    ctx.beginPath()
    ctx.moveTo(gridX0, y)
    ctx.lineTo(gridX0 + binSize * cellW, y)
    ctx.stroke()
  }

  // mini-bin boundaries (vertical, stronger every miniBinSize)
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

  // fallback boundary line (vertical)
  ctx.strokeStyle = gridFallback
  ctx.lineWidth = 3
  {
    const x = gridX0 + miniTotal * cellW + 0.5
    ctx.beginPath()
    ctx.moveTo(x, gridY0)
    ctx.lineTo(x, gridY0 + binCount * cellH)
    ctx.stroke()
  }

  // outline
  ctx.strokeStyle = gridStrong
  ctx.lineWidth = 2
  ctx.strokeRect(gridX0 + 0.5, gridY0 + 0.5, binSize * cellW - 1, binCount * cellH - 1)

  // small corner label
  ctx.fillStyle = 'rgba(255,255,255,0.62)'
  ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace'
  ctx.fillText(`bins [${binStart}..${binStart + binCount - 1}]  bin_size=${binSize}`, gridX0, padT + 14)

  // top ruler: mini-bin #i / fallback (so you never count cells)
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

  // right ruler: bin labels
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

async function refreshKickHist() {
  try {
    const { data } = await axios.get('/api/experiment/kick_depth_hist')
    renderKickHist(data.hist ?? [])
  } catch (e: any) {
    opResult.value = `kick_hist error: ${e?.message ?? e}`
  }
}

function renderKickHist(hist: Array<{ depth: number; count: number }>) {
  if (!kickHistEl.value) return
  if (!kickChart) kickChart = echarts.init(kickHistEl.value)
  kickChart.setOption({
    title: { text: 'Kick 深度分布', left: 'left' },
    tooltip: { trigger: 'axis' },
    xAxis: { type: 'category', data: hist.map((h) => String(h.depth)) },
    yAxis: { type: 'value' },
    series: [{ type: 'bar', data: hist.map((h) => h.count), itemStyle: { color: '#111827' } }],
  })
}

async function runFallbackVsLoad() {
  busyTitle.value = '实验运行中：fallback vs load'
  busyHint.value = '该实验会重建表并分阶段插入，可能需要几分钟。'
  beginBusy()
  try {
    const { data } = await axios.post('/api/experiment/fallback_vs_load', {
      n: initForm.value.n,
      k: initForm.value.k,
      distribution: distForm.value.distribution,
      skew: batchForm.value.skew,
      load_targets: [0.7, 0.8, 0.9, 0.95, 0.98],
      step: 2000,
    })
    renderExperiment('fallback_vs_load', data.points ?? [])
    opResult.value = 'fallback_vs_load done (table re-initialized)'
    await refreshStats()
    await refreshSnapshot()
    await refreshKickHist()
  } catch (e: any) {
    opResult.value = `fallback_vs_load error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

async function runProbeVsN() {
  busyTitle.value = '实验运行中：probe vs n'
  busyHint.value = '该实验会对多个 n/k 反复 init+insert+query，可能需要几分钟。'
  beginBusy()
  try {
    const { data } = await axios.post('/api/experiment/probe_vs_n', {
      ns: [10000, 30000, 100000],
      ks: [1, 2, 3],
      inserts: 20000,
      queries: 5000,
    })
    renderExperiment('probe_vs_n', data.series ?? [])
    opResult.value = 'probe_vs_n done (table re-initialized)'
    await refreshStats()
    await refreshSnapshot()
    await refreshKickHist()
  } catch (e: any) {
    opResult.value = `probe_vs_n error: ${e?.message ?? e}`
  } finally {
    endBusy()
  }
}

function beginBusy() {
  busy.value = true
  startedAt.value = Date.now()
  elapsedMs.value = 0
  if (tickTimer) window.clearInterval(tickTimer)
  tickTimer = window.setInterval(() => {
    elapsedMs.value = Date.now() - startedAt.value
  }, 250)
}

function endBusy() {
  busy.value = false
  if (tickTimer) window.clearInterval(tickTimer)
  tickTimer = null
}

function renderExperiment(kind: 'fallback_vs_load' | 'probe_vs_n', payload: any) {
  if (!expEl.value) return
  if (!expChart) expChart = echarts.init(expEl.value)

  if (kind === 'fallback_vs_load') {
    const pts: Array<any> = payload
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

  const series: Array<any> = payload
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

function renderChart() {
  if (!chartEl.value) return
  if (!chart) chart = echarts.init(chartEl.value)
  const xs = history.value.map((_, i) => i + 1)
  chart.setOption({
    tooltip: { trigger: 'axis' },
    legend: { data: ['used_slots', 'fallback_used'] },
    xAxis: { type: 'category', data: xs },
    yAxis: { type: 'value' },
    series: [
      { name: 'used_slots', type: 'line', data: history.value.map((p) => p.used), smooth: true },
      { name: 'fallback_used', type: 'line', data: history.value.map((p) => p.fb), smooth: true },
    ],
  })
}

onMounted(() => {
  if (chartEl.value) {
    chart = echarts.init(chartEl.value)
    renderChart()
    window.addEventListener('resize', onResize)
  }
  if (kickHistEl.value) kickChart = echarts.init(kickHistEl.value)
  if (expEl.value) expChart = echarts.init(expEl.value)
  if (canvasEl.value) renderCanvas()
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

function onResize() {
  chart?.resize()
  kickChart?.resize()
  expChart?.resize()
}

watch(chartEl, () => renderChart())
watch(canvasEl, () => renderCanvas())
</script>

<style scoped>
.overlay {
  position: fixed;
  inset: 0;
  background: rgba(17, 24, 39, 0.35);
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 16px;
  z-index: 9999;
}
.overlay-card {
  width: min(560px, 100%);
  background: rgba(255, 255, 255, 0.08);
  border-radius: 14px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  box-shadow: 0 18px 70px rgba(0, 0, 0, 0.45);
  padding: 16px;
  display: flex;
  gap: 14px;
  align-items: center;
  backdrop-filter: blur(10px);
}
.overlay-title {
  font-weight: 700;
  font-size: 14px;
}
.overlay-sub {
  margin-top: 6px;
  color: rgba(255,255,255,0.68);
  font-size: 12px;
  line-height: 1.4;
}
.spinner {
  width: 28px;
  height: 28px;
  border-radius: 999px;
  border: 3px solid rgba(255,255,255,0.22);
  border-top-color: rgba(34, 211, 238, 0.9);
  animation: spin 0.9s linear infinite;
  flex: 0 0 auto;
}
@keyframes spin {
  to { transform: rotate(360deg); }
}
.app {
  max-width: 1320px;
  margin: 0 auto;
  padding: 22px 18px 40px;
}
.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  margin-bottom: 18px;
}
.title h1 {
  margin: 0;
  font-size: 22px;
}
.subtitle {
  margin: 6px 0 0;
  color: rgba(255,255,255,0.62);
  font-size: 13px;
}
.pill {
  padding: 10px 12px;
  border: 1px solid rgba(255,255,255,0.12);
  border-radius: 12px;
  background: rgba(255,255,255,0.06);
  box-shadow: 0 10px 40px rgba(0,0,0,0.25);
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.pill-k {
  font-size: 11px;
  color: rgba(255,255,255,0.55);
}
.pill-v {
  font-size: 12px;
  color: rgba(255,255,255,0.88);
}
.layout {
  display: grid;
  grid-template-columns: 420px 1fr;
  gap: 14px;
  align-items: start;
}
@media (max-width: 1180px) {
  .layout { grid-template-columns: 1fr; }
}
.sidebar {
  display: grid;
  gap: 14px;
  position: sticky;
  top: 14px;
}
@media (max-width: 1180px) {
  .sidebar { position: static; }
}
.content {
  display: grid;
  gap: 14px;
}
.grid2 {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 14px;
}
@media (max-width: 980px) {
  .grid2 { grid-template-columns: 1fr; }
}
.card {
  border: 1px solid rgba(255,255,255,0.12);
  border-radius: 14px;
  padding: 14px;
  background: rgba(255,255,255,0.06);
  box-shadow: 0 18px 60px rgba(0,0,0,0.18);
  backdrop-filter: blur(10px);
}
.card h2 {
  margin: 0 0 10px;
  font-size: 15px;
  color: rgba(255,255,255,0.90);
}
.card-head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 10px;
  flex-wrap: wrap;
}
.grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
  margin-bottom: 10px;
}
.grid-4 {
  grid-template-columns: repeat(4, minmax(0, 1fr));
}
.grid-3 {
  grid-template-columns: repeat(3, minmax(0, 1fr));
}
label span {
  display: block;
  font-size: 12px;
  color: rgba(255,255,255,0.62);
  margin-bottom: 6px;
}
select {
  width: 100%;
  padding: 8px 10px;
  border: 1px solid rgba(255,255,255,0.14);
  border-radius: 12px;
  outline: none;
  background: rgba(0,0,0,0.22);
  color: rgba(255,255,255,0.86);
}
select:focus {
  border-color: rgba(34, 211, 238, 0.55);
  box-shadow: 0 0 0 3px rgba(34, 211, 238, 0.14);
}
input {
  width: 100%;
  padding: 8px 10px;
  border: 1px solid rgba(255,255,255,0.14);
  border-radius: 12px;
  outline: none;
  background: rgba(0,0,0,0.22);
  color: rgba(255,255,255,0.86);
}
input:focus {
  border-color: rgba(34, 211, 238, 0.55);
  box-shadow: 0 0 0 3px rgba(34, 211, 238, 0.14);
}
.row {
  display: flex;
  gap: 10px;
  align-items: center;
  flex-wrap: wrap;
}
button {
  border: 1px solid rgba(255,255,255,0.16);
  border-radius: 12px;
  padding: 9px 12px;
  background: rgba(255,255,255,0.06);
  color: rgba(255,255,255,0.86);
  cursor: pointer;
  transition: transform 0.05s ease, background 0.15s ease, border-color 0.15s ease;
}
button.primary {
  background: linear-gradient(135deg, rgba(79,70,229,0.95), rgba(34,211,238,0.80));
  border-color: rgba(255,255,255,0.16);
  color: #fff;
}
button.warn {
  background: rgba(245, 158, 11, 0.16);
  border-color: rgba(245, 158, 11, 0.35);
  color: rgba(255,255,255,0.92);
}
button:active {
  transform: translateY(1px);
}
button:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}
.mono {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, 'Liberation Mono', 'Courier New', monospace;
  font-size: 12px;
}
.pre {
  margin: 10px 0 0;
  padding: 10px;
  background: rgba(0,0,0,0.30);
  color: rgba(255,255,255,0.82);
  border-radius: 12px;
  overflow: auto;
  border: 1px solid rgba(255,255,255,0.10);
}
.hint {
  color: rgba(255,255,255,0.62);
  font-size: 12px;
}
.stats {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
  gap: 10px;
  margin-bottom: 12px;
}
.stat {
  border: 1px solid rgba(255,255,255,0.10);
  border-radius: 14px;
  padding: 10px;
  background: rgba(255,255,255,0.05);
}
.stat .k {
  font-size: 12px;
  color: rgba(255,255,255,0.60);
}
.stat .v {
  font-size: 16px;
  font-weight: 600;
  margin-top: 4px;
  color: rgba(255,255,255,0.90);
}
.chart {
  height: 260px;
  width: 100%;
}
.canvas {
  width: 100%;
  height: 340px;
  border: 1px solid rgba(255,255,255,0.12);
  border-radius: 14px;
  background: rgba(0,0,0,0.25);
}
.legend {
  display: flex;
  gap: 10px;
  flex-wrap: wrap;
  margin-top: 10px;
}
.chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: rgba(255,255,255,0.72);
}
.chip::before {
  content: '';
  width: 12px;
  height: 12px;
  border-radius: 3px;
  display: inline-block;
  border: 1px solid rgba(255,255,255,0.18);
}
.chip.empty::before { background: #ffffff; }
.chip.used::before { background: #2563eb; border-color: #2563eb; }
.chip.fb::before { background: #f59e0b; border-color: #f59e0b; }
.chip.kick::before { background: #ef4444; border-color: #ef4444; }
.inline {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.one-line {
  align-items: center;
}
.one-line input {
  flex: 1 1 140px;
  min-width: 140px;
}

.tip {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 16px;
  height: 16px;
  margin-left: 6px;
  border-radius: 999px;
  border: 1px solid rgba(255,255,255,0.20);
  color: rgba(255,255,255,0.75);
  background: rgba(0,0,0,0.25);
  font-size: 11px;
  cursor: help;
  user-select: none;
}

.hint-block {
  margin-top: 8px;
  padding: 10px;
  border: 1px solid rgba(255,255,255,0.10);
  border-radius: 12px;
  background: rgba(0,0,0,0.20);
  color: rgba(255,255,255,0.62);
  font-size: 12px;
  line-height: 1.5;
}

.mt-8 { margin-top: 8px; }
.mt-10 { margin-top: 10px; }

.kv {
  margin-top: 10px;
  display: grid;
  gap: 6px;
  padding: 10px;
  border: 1px solid rgba(255,255,255,0.10);
  border-radius: 12px;
  background: rgba(0,0,0,0.22);
}
.kv-row {
  display: flex;
  justify-content: space-between;
  gap: 8px;
}
.kv-k { color: rgba(255,255,255,0.55); }
.kv-v { color: rgba(255,255,255,0.86); }

.complex {
  display: grid;
  gap: 10px;
}
.complex2 {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}
@media (max-width: 1180px) {
  .complex2 { grid-template-columns: 1fr; }
}
.complex-title {
  font-size: 12px;
  color: rgba(255,255,255,0.72);
  margin-bottom: 8px;
}
.complex-col {
  border: 1px solid rgba(255,255,255,0.10);
  border-radius: 12px;
  background: rgba(0,0,0,0.16);
  padding: 10px;
}
.complex-row {
  display: grid;
  gap: 6px;
}
.complex-k {
  color: rgba(255,255,255,0.72);
  font-size: 12px;
}
.complex-v {
  color: rgba(255,255,255,0.88);
  font-size: 12px;
}
.complex-note {
  color: rgba(255,255,255,0.55);
  font-size: 12px;
  line-height: 1.5;
}
</style>

