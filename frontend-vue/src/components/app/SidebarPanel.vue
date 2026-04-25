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
        <button class="primary" @click="$emit('initTable')" :disabled="busy">初始化</button>
        <button @click="$emit('refreshStats')" :disabled="busy">刷新统计</button>
        <button @click="$emit('refreshKickHist')" :disabled="busy">kick 深度分布</button>
      </div>

      <div class="kv mono" v-if="params">
        <div class="kv-row"><span class="kv-k">n</span><span class="kv-v">{{ params.n }}</span></div>
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
              <div class="complex-v mono">O(log<sup>(k)</sup> n) ≈ O({{ miniBinSizeDisplay }})</div>
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
            <span class="tip" title="这里是前端测得的端到端耗时（含网络与后端 SQL）。实验接口会重建表，耗时会明显更大。">?</span>
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
      <h2>批量插入 / 查询测试</h2>
      <div class="grid grid-4 mt-8">
        <label>
          <span>batch count</span>
          <input v-model.number="batchForm.count" type="number" min="1" step="100" />
        </label>
        <label>
          <span>skew</span>
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
        <button class="primary" @click="$emit('batchInsert')" :disabled="busy">批量插入</button>
        <button @click="$emit('queryTest')" :disabled="busy">查询测试</button>
      </div>
      <div class="row mt-8">
        <button class="warn" @click="$emit('runFallbackVsLoad')" :disabled="busy">实验：fallback vs load</button>
        <button class="warn" @click="$emit('runProbeVsN')" :disabled="busy">实验：probe vs n</button>
      </div>
    </div>
  </aside>
</template>

<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{
  busy: boolean
  initForm: { n: number; k: number; load_factor: number }
  distForm: { distribution: 'uniform' | 'skewed' }
  batchForm: { count: number; skew: number }
  queryForm: { count: number; hit_rate: number }
  params: any
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
  (e: 'refreshKickHist'): void
  (e: 'insertOne'): void
  (e: 'findOne'): void
  (e: 'eraseOne'): void
  (e: 'batchInsert'): void
  (e: 'queryTest'): void
  (e: 'runFallbackVsLoad'): void
  (e: 'runProbeVsN'): void
  (e: 'update:keyValue', v: number): void
}>()

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
