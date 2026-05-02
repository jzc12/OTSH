<template>
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
          <input v-model.number="initForm.k" type="number" min="0" max="8" step="1" />
        </label>
        <label class="span-2">
          <span>snapshot_tag</span>
          <input v-model="initForm.snapshot_tag" type="text" maxlength="64" placeholder="实验标记" />
        </label>
      </div>

      <div class="row">
        <button class="primary" @click="$emit('initTable')" :disabled="busy">初始化</button>
        <button @click="$emit('refreshStats')" :disabled="busy">刷新统计</button>
        <button @click="$emit('loadSnapshots')" :disabled="busy">快照列表</button>
      </div>

      <div class="row mt-8">
        <label class="grow">
          <span class="hint">分析用 snapshot</span>
          <select
            class="mono"
            :value="selectedSnapshotId ?? ''"
            @change="onSnapChange(($event.target as HTMLSelectElement).value)"
          >
            <option value="" disabled>未选择</option>
            <option v-for="s in snapshots" :key="s.id" :value="String(s.id)">
              #{{ s.id }} {{ s.snapshot_tag ?? '' }} (n={{ s.n }})
            </option>
          </select>
        </label>
        <button @click="$emit('dumpStructure')" :disabled="busy || selectedSnapshotId == null">结构落库</button>
      </div>

      <div class="kv mono" v-if="params">
        <div class="kv-row"><span class="kv-k">n</span><span class="kv-v">{{ params.n }}</span></div>
        <div class="kv-row"><span class="kv-k">N</span><span class="kv-v">{{ params.N }}</span></div>
        <div class="kv-row"><span class="kv-k">K</span><span class="kv-v">{{ params.K }}</span></div>
      </div>
    </div>

    <div class="card">
        <div class="kv mono">
          <div class="kv-row"><span class="kv-k">init</span><span class="kv-v">{{ fmtMs(rt.init_ms) }}</span></div>
          <div class="kv-row"><span class="kv-k">insert</span><span class="kv-v">{{ fmtMs(rt.insert_ms) }}</span></div>
          <div class="kv-row"><span class="kv-k">find</span><span class="kv-v">{{ fmtMs(rt.find_ms) }}</span></div>
          <div class="kv-row"><span class="kv-k">erase</span><span class="kv-v">{{ fmtMs(rt.erase_ms) }}</span></div>
        </div>
    </div>

    <div class="card">
      <h2>单步操作</h2>
      <div class="row mt-8">
        <input v-model.number="keyProxy" type="number" placeholder="key (uint64)" />
      </div>
      <div class="row one-line mt-10">
        <button class="primary" @click="$emit('insertOne')" :disabled="busy">插入</button>
        <button @click="$emit('findOne')" :disabled="busy">查询</button>
        <button @click="$emit('eraseOne')" :disabled="busy">删除</button>
      </div>
      <div class="row mt-8">
        <span class="hint">结果：</span>
        <span class="mono">{{ opResult }}</span>
      </div>
    </div>

    <div class="card">
      <h2>验证实验</h2>
      <div class="row mt-10">
        <button class="warn" @click="$emit('runO1VsN')" :disabled="busy">实验：query O(1) vs n</button>
        <button class="warn" @click="$emit('runOkVsK')" :disabled="busy">实验：O(k) vs k</button>
      </div>
    </div>
  </aside>
</template>

<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{
  busy: boolean
  initForm: { n: number; k: number; snapshot_tag?: string }
  params: any
  snapshots: any[]
  selectedSnapshotId: number | null
  rt: {
    init_ms: number | null
    insert_ms: number | null
    find_ms: number | null
    erase_ms: number | null
    batch_insert_ms: number | null
    query_test_ms: number | null
    snapshot_ms: number | null
  }
  miniBinSizeDisplay: number
  fmtMs: (v: number | null) => string
  keyValue: number
  opResult: string
}>()

const emit = defineEmits<{
  (e: 'initTable'): void
  (e: 'refreshStats'): void
  (e: 'loadSnapshots'): void
  (e: 'dumpStructure'): void
  (e: 'insertOne'): void
  (e: 'findOne'): void
  (e: 'eraseOne'): void
  (e: 'runO1VsN'): void
  (e: 'runOkVsK'): void
  (e: 'update:keyValue', v: number): void
  (e: 'update:selectedSnapshotId', v: number | null): void
}>()

function onSnapChange(v: string) {
  if (!v) emit('update:selectedSnapshotId', null)
  else {
    const n = Number(v)
    emit('update:selectedSnapshotId', Number.isFinite(n) ? n : null)
  }
}

const keyProxy = computed<number>({
  get() {
    return props.keyValue
  },
  set(v) {
    if (Number.isFinite(v)) emit('update:keyValue', v)
  },
})

// resize info is provided by /api/stats and shown here
</script>
