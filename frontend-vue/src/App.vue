<template>
    <div class="app">
        <BusyOverlay :busy="blocked" :busy-title="busyTitle" :busy-hint="busyHint" :elapsed-ms="elapsedMs"
        />
        <AppHeader :backend-base="backendBase" />

        <section class="layout">
            <SidebarPanel :busy="blocked" :init-form="initForm" :params="params" :rt="rt"
                :mini-bin-size-display="miniBinSizeDisplay" :fmt-ms="fmtMs" :key-value="key" :op-result="opResult"
                :snapshots="snapshots" :selected-snapshot-id="selectedSnapshotId"
                @update:keyValue="setKey" @initTable="initTable" @refreshStats="refreshStats"
                @insertOne="insertOne" @findOne="findOne" @eraseOne="eraseOne"
                @runO1VsN="runO1VsN" @runOkVsK="runOkVsK"
                @loadSnapshots="loadSnapshots" @dumpStructure="dumpStructure"
                @update:selectedSnapshotId="setSelectedSnapshot" />

            <main class="content">
                <ChartsPanel :stats="stats" :history="history" :analytics-db="stats?.analytics_db ?? null"
                    :experiment="experiment" />
            </main>
        </section>
    </div>
</template>

<script setup lang="ts">
import axios from 'axios'
import { computed, ref, watch } from 'vue'
import AppHeader from './components/app/AppHeader.vue'
import BusyOverlay from './components/app/BusyOverlay.vue'
import ChartsPanel from './components/app/ChartsPanel.vue'
import SidebarPanel from './components/app/SidebarPanel.vue'

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

function setSelectedSnapshot(id: number | null) {
    selectedSnapshotId.value = id
}

const initForm = ref({
    n: 10000,
    k: 2,
    snapshot_tag: 'init',
})

const snapshots = ref<any[]>([])
const selectedSnapshotId = ref<number | null>(null)

const params = ref<any>(null)
const stats = ref<any>(null)
const lastTraceIdx = ref<Set<number>>(new Set())
const history = ref<Array<{ used: number; fb: number }>>([])
const experiment = ref<any>(null)

const miniBinSizeDisplay = computed(() => {
    const v = params.value?.mini_bin_size
    if (typeof v === 'number') return v
    if (typeof v === 'string') return Number(v) || 0
    return 0
})

const blocked = computed(() => {
    return busy.value
})

watch(selectedSnapshotId, async (id) => {
    if (id == null || id <= 0) return
    try {
        const { data: a } = await axios.post('/api/analytics/summary', { snapshot_id: id })
        if (a?.ok && a.summary && stats.value)
            stats.value = { ...stats.value, analytics_db: a.summary }
    } catch {
        /* ignore */
    }
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
        const { data } = await axios.post('/api/init', {
            n: initForm.value.n,
            k: initForm.value.k,
            snapshot_tag: initForm.value.snapshot_tag || 'init',
        })
        rt.value.init_ms = performance.now() - t0
        params.value = data.params
        if (typeof data.snapshot_id === 'number' && data.snapshot_id > 0) {
            selectedSnapshotId.value = data.snapshot_id
        }
        opResult.value = data.ok ? 'init ok' : `init failed: ${data.error ?? ''}`
        history.value = []
        lastTraceIdx.value = new Set()
        await refreshStats()
        await loadSnapshots()
    } catch (e: any) {
        opResult.value = `init error: ${e?.message ?? e}`
    } finally {
        endBusy()
    }
}

async function loadSnapshots() {
    try {
        const { data } = await axios.post('/api/analytics/snapshots', {})
        if (data?.ok && Array.isArray(data.items)) {
            snapshots.value = data.items
            if (selectedSnapshotId.value == null && data.items.length) {
                selectedSnapshotId.value = Number(data.items[0].id) || null
            }
        }
    } catch {
        /* ignore */
    }
}

async function dumpStructure() {
    const sid = selectedSnapshotId.value
    if (sid == null) {
        opResult.value = 'structure_dump: no snapshot selected'
        return
    }
    busyTitle.value = '结构落库…'
    busyHint.value = '写入 facility / cubby / slot_snapshot / tier_stat'
    beginBusy()
    try {
        const { data } = await axios.post('/api/analytics/structure_dump', { snapshot_id: sid })
        opResult.value = data.ok ? `structure_dump ok (snapshot_id=${data.snapshot_id})` : `structure_dump: ${data.error ?? ''}`
    } catch (e: any) {
        opResult.value = `structure_dump error: ${e?.message ?? e}`
    } finally {
        endBusy()
    }
}

async function refreshStats() {
    busyTitle.value = '刷新统计...'
    busyHint.value = '正在读取全表统计。'
    beginBusy()
    try {
        const { data } = await axios.post('/api/stats', {})
        let merged = { ...data }
        if (selectedSnapshotId.value != null && selectedSnapshotId.value > 0) {
            try {
                const { data: a } = await axios.post('/api/analytics/summary', {
                    snapshot_id: selectedSnapshotId.value,
                })
                if (a?.ok && a.summary) merged = { ...merged, analytics_db: a.summary }
            } catch {
                /* keep server analytics_db */
            }
        }
        stats.value = merged
        history.value.push({ used: merged?.state?.n ?? 0, fb: merged?.metrics?.router_steps?.max ?? 0 })

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

async function runO1VsN() {
    busyTitle.value = '实验：query O(1) vs n'
    busyHint.value = '后端一次性跑完并返回曲线数据。'
    beginBusy()
    try {
        const kFixed = initForm.value.k
        const { data } = await axios.post('/api/experiment/o1_vs_n', { k: kFixed })
        const jobId = data?.job_id
        if (!data?.ok || !jobId) {
            opResult.value = `experiment start failed: ${data?.error ?? ''}`
            return
        }
        while (true) {
            const { data: j } = await axios.post('/api/jobs/get', { id: jobId })
            if (j?.ok) {
                busyHint.value = `进度 ${j.progress ?? 0}%：${j.message ?? ''}`
                if (j.status === 'done') {
                    const r = j.result
                    experiment.value = { kind: 'o1_vs_n', payload: r?.points ?? [], k: kFixed }
                    break
                }
                if (j.status === 'error') {
                    opResult.value = `experiment failed: ${j.job_error ?? ''}`
                    break
                }
            }
            await new Promise((r) => setTimeout(r, 500))
        }
    } catch (e: any) {
        opResult.value = `experiment error: ${e?.message ?? e}`
    } finally {
        endBusy()
    }
}

async function runOkVsK() {
    busyTitle.value = '实验：insert/delete O(k) vs k'
    busyHint.value = '后端一次性跑完并返回曲线数据。'
    beginBusy()
    try {
        const n = initForm.value.n
        const { data } = await axios.post('/api/experiment/ok_vs_k', { n })
        const jobId = data?.job_id
        if (!data?.ok || !jobId) {
            opResult.value = `experiment start failed: ${data?.error ?? ''}`
            return
        }
        while (true) {
            const { data: j } = await axios.post('/api/jobs/get', { id: jobId })
            if (j?.ok) {
                busyHint.value = `进度 ${j.progress ?? 0}%：${j.message ?? ''}`
                if (j.status === 'done') {
                    const r = j.result
                    experiment.value = { kind: 'ok_vs_k', payload: r?.series ?? [], n }
                    break
                }
                if (j.status === 'error') {
                    opResult.value = `experiment failed: ${j.job_error ?? ''}`
                    break
                }
            }
            await new Promise((r) => setTimeout(r, 500))
        }
    } catch (e: any) {
        opResult.value = `experiment error: ${e?.message ?? e}`
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
        const { data } = await axios.post('/api/insert', { key: key.value })
        rt.value.insert_ms = performance.now() - t0
        opResult.value = data.ok ? `insert ok (inserted=${data.inserted})` : `insert fail: ${data.error ?? ''}`
        await refreshStats()
    } catch (e: any) {
        opResult.value = `insert error: ${e?.message ?? e}`
    } finally {
        endBusy()
    }
}

async function findOne() {
    busyTitle.value = '查询中...'
    busyHint.value = '查询 key 是否存在。'
    beginBusy()
    try {
        const t0 = performance.now()
        const { data } = await axios.post('/api/query', { key: key.value })
        rt.value.find_ms = performance.now() - t0
        opResult.value = data.found ? `found` : `not found`
    } catch (e: any) {
        opResult.value = `find error: ${e?.message ?? e}`
    } finally {
        endBusy()
    }
}

async function eraseOne() {
    busyTitle.value = '删除中...'
    busyHint.value = '删除 key（幂等）。'
    beginBusy()
    try {
        const t0 = performance.now()
        const { data } = await axios.post('/api/delete', { key: key.value })
        rt.value.erase_ms = performance.now() - t0
        opResult.value = data.ok ? `delete ok (deleted=${data.deleted})` : `delete failed: ${data.error ?? ''}`
        await refreshStats()
    } catch (e: any) {
        opResult.value = `erase error: ${e?.message ?? e}`
    } finally {
        endBusy()
    }
}

// 其余实验/快照接口在该版本前端不再使用（前端仅展示后端效果，不改变样式框架）。

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
