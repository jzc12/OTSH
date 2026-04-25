<template>
    <div class="app">
        <BusyOverlay :busy="blocked" :busy-title="busyTitle" :busy-hint="busyHint" :elapsed-ms="elapsedMs"
        />
        <AppHeader :backend-base="backendBase" />

        <section class="layout">
            <SidebarPanel :busy="blocked" :init-form="initForm" :dist-form="distForm" :batch-form="batchForm"
                :query-form="queryForm" :params="params" :rt="rt"
                :mini-bin-size-display="miniBinSizeDisplay" :fmt-ms="fmtMs" :key-value="key" :op-result="opResult"
                @update:keyValue="setKey" @initTable="initTable" @refreshStats="refreshStats"
                @refreshKickHist="refreshKickHist" @insertOne="insertOne" @findOne="findOne" @eraseOne="eraseOne"
                @batchInsert="batchInsert" @queryTest="queryTest" @runFallbackVsLoad="runFallbackVsLoad"
                @runProbeVsN="runProbeVsN" />

            <main class="content">
                <StructureCard :busy="blocked" :snap-form="snapForm" :snapshot="snapshot" :params="params"
                    :last-trace-idx="lastTraceIdx" @refreshSnapshot="refreshSnapshot" />

                <ChartsPanel :stats="stats" :history="history" :kick-hist="kickHist" :experiment="experiment" />
            </main>
        </section>
    </div>
</template>

<script setup lang="ts">
import axios from 'axios'
import { computed, ref } from 'vue'
import AppHeader from './components/app/AppHeader.vue'
import BusyOverlay from './components/app/BusyOverlay.vue'
import ChartsPanel from './components/app/ChartsPanel.vue'
import SidebarPanel from './components/app/SidebarPanel.vue'
import StructureCard from './components/app/StructureCard.vue'

const backendBase = 'http://127.0.0.1:8080'

const busy = ref(false)
const busyTitle = ref('处理中...')
const busyHint = ref('这是一个可能较慢的数据库操作，请稍等。')
const startedAt = ref<number>(0)
const elapsedMs = ref<number>(0)
let tickTimer: number | null = null
const key = ref<number>(1)
const opResult = ref<string>('-')

function setKey(v: number) {
    key.value = v
}

const initForm = ref({
    n: 10000,
    k: 2,
    load_factor: 0.9,
})

const distForm = ref({
    distribution: 'uniform' as 'uniform' | 'skewed',
})

const batchForm = ref({
    count: 1500,
    skew: 1,
})

const queryForm = ref({
    count: 1500,
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
const history = ref<Array<{ used: number; fb: number }>>([])
const kickHist = ref<Array<{ depth: number; count: number }>>([])
const experiment = ref<{ kind: 'fallback_vs_load' | 'probe_vs_n'; payload: any } | null>(null)

const miniBinSizeDisplay = computed(() => {
    const v = params.value?.mini_bin_size
    if (typeof v === 'number') return v
    if (typeof v === 'string') return Number(v) || 0
    return 0
})

const blocked = computed(() => {
    return busy.value
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

        // sync params/meta back to UI (resize may update n/total_bins)
        if (data?.params) {
            params.value = params.value ? { ...params.value, ...data.params } : data.params
            if (typeof data.params.n === 'number') initForm.value.n = data.params.n
        }       
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
        if (data?.params) {
            params.value = params.value ? { ...params.value, ...data.params } : data.params
            if (typeof data.params.n === 'number') initForm.value.n = data.params.n
        }
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
    } catch (e: any) {
        opResult.value = `snapshot error: ${e?.message ?? e}`
    }
}

async function refreshKickHist() {
    try {
        const { data } = await axios.get('/api/experiment/kick_depth_hist')
        kickHist.value = data.hist ?? []
    } catch (e: any) {
        opResult.value = `kick_hist error: ${e?.message ?? e}`
    }
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
        experiment.value = { kind: 'fallback_vs_load', payload: data.points ?? [] }
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
        experiment.value = { kind: 'probe_vs_n', payload: data.series ?? [] }
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

</script>

<style src="./styles/app.css"></style>
