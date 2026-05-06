#include "metrics.h"

#include <cmath>

namespace otsh {

static Metrics g_metrics;

Metrics &global_metrics() { return g_metrics; }

void Metrics::atomic_max(std::atomic<uint64_t> &x, uint64_t v) {
  uint64_t cur = x.load(std::memory_order_relaxed);
  while (cur < v && !x.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    // cur updated by compare_exchange_weak
  }
}

size_t Metrics::bucket_idx(uint64_t ns) {
  // bucket 0: [0..1], bucket i: (2^(i-1), 2^i]
  if (ns <= 1)
    return 0;
  size_t i = 0;
  while (ns > 1 && i + 1 < 32) {
    ns >>= 1;
    i++;
  }
  return i;
}

uint64_t Metrics::estimate_quantile_from_buckets(const std::array<uint64_t, 32> &b,
                                                uint64_t count,
                                                double q) {
  if (count == 0)
    return 0;
  const uint64_t target =
      static_cast<uint64_t>(std::ceil(q * static_cast<double>(count)));
  uint64_t run = 0;
  for (size_t i = 0; i < b.size(); i++) {
    run += b[i];
    if (run >= target) {
      // return upper bound of bucket
      return (i == 0) ? 1 : (1ULL << i);
    }
  }
  return 1ULL << (b.size() - 1);
}

void Metrics::on_init() { ops_init_.fetch_add(1, std::memory_order_relaxed); }

void Metrics::on_insert(uint64_t moved, uint64_t router_steps,
                        uint64_t meta_bits) {
  ops_insert_.fetch_add(1, std::memory_order_relaxed);
  insert_moved_total_.fetch_add(moved, std::memory_order_relaxed);
  atomic_max(insert_moved_max_, moved);

  router_steps_total_.fetch_add(router_steps, std::memory_order_relaxed);
  atomic_max(router_steps_max_, router_steps);

  meta_bits_total_.fetch_add(meta_bits, std::memory_order_relaxed);
  atomic_max(meta_bits_max_, meta_bits);
}

void Metrics::on_query(uint64_t router_steps) {
  ops_query_.fetch_add(1, std::memory_order_relaxed);
  router_steps_total_.fetch_add(router_steps, std::memory_order_relaxed);
  atomic_max(router_steps_max_, router_steps);
}

void Metrics::on_delete(uint64_t moved, uint64_t router_steps,
                        uint64_t meta_bits) {
  ops_delete_.fetch_add(1, std::memory_order_relaxed);
  delete_moved_total_.fetch_add(moved, std::memory_order_relaxed);
  atomic_max(delete_moved_max_, moved);

  router_steps_total_.fetch_add(router_steps, std::memory_order_relaxed);
  atomic_max(router_steps_max_, router_steps);

  meta_bits_total_.fetch_add(meta_bits, std::memory_order_relaxed);
  atomic_max(meta_bits_max_, meta_bits);
}

static void record_latency(std::atomic<uint64_t> &count,
                           std::atomic<uint64_t> &total,
                           std::atomic<uint64_t> &maxv,
                           std::array<std::atomic<uint64_t>, 32> &buckets,
                           uint64_t ns) {
  count.fetch_add(1, std::memory_order_relaxed);
  total.fetch_add(ns, std::memory_order_relaxed);
  // local max
  uint64_t cur = maxv.load(std::memory_order_relaxed);
  while (cur < ns &&
         !maxv.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {
  }
  auto bucket_idx_local = [](uint64_t v) -> size_t {
    if (v <= 1)
      return 0;
    size_t i = 0;
    while (v > 1 && i + 1 < 32) {
      v >>= 1;
      i++;
    }
    return i;
  };
  const size_t bi = bucket_idx_local(ns);
  buckets[bi].fetch_add(1, std::memory_order_relaxed);
}

void Metrics::on_ht_init_ns(uint64_t ns) {
  record_latency(ht_init_count_, ht_init_total_, ht_init_max_, ht_init_b_, ns);
}
void Metrics::on_ht_insert_ns(uint64_t ns) {
  record_latency(ht_insert_count_, ht_insert_total_, ht_insert_max_,
                 ht_insert_b_, ns);
}
void Metrics::on_ht_query_ns(uint64_t ns) {
  record_latency(ht_query_count_, ht_query_total_, ht_query_max_, ht_query_b_,
                 ns);
}
void Metrics::on_ht_delete_ns(uint64_t ns) {
  record_latency(ht_delete_count_, ht_delete_total_, ht_delete_max_,
                 ht_delete_b_, ns);
}

void Metrics::on_rebuild_down() {
  ev_rebuild_down_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::on_rebuild_up() {
  ev_rebuild_up_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::on_resize_start() {
  ev_resize_start_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::on_resize_finish() {
  ev_resize_finish_.fetch_add(1, std::memory_order_relaxed);
}

Metrics::Snapshot Metrics::snapshot() const {
  Snapshot s;
  s.ops_init = ops_init_.load(std::memory_order_relaxed);
  s.ops_insert = ops_insert_.load(std::memory_order_relaxed);
  s.ops_query = ops_query_.load(std::memory_order_relaxed);
  s.ops_delete = ops_delete_.load(std::memory_order_relaxed);

  s.insert_moved_total = insert_moved_total_.load(std::memory_order_relaxed);
  s.insert_moved_max = insert_moved_max_.load(std::memory_order_relaxed);
  s.delete_moved_total = delete_moved_total_.load(std::memory_order_relaxed);
  s.delete_moved_max = delete_moved_max_.load(std::memory_order_relaxed);

  s.router_steps_total = router_steps_total_.load(std::memory_order_relaxed);
  s.router_steps_max = router_steps_max_.load(std::memory_order_relaxed);

  s.meta_bits_total = meta_bits_total_.load(std::memory_order_relaxed);
  s.meta_bits_max = meta_bits_max_.load(std::memory_order_relaxed);

  auto fill_lat = [&](LatencySummary &out,
                      const std::atomic<uint64_t> &cnt,
                      const std::atomic<uint64_t> &tot,
                      const std::atomic<uint64_t> &mx,
                      const std::array<std::atomic<uint64_t>, 32> &b) {
    out.count = cnt.load(std::memory_order_relaxed);
    out.total_ns = tot.load(std::memory_order_relaxed);
    out.max_ns = mx.load(std::memory_order_relaxed);
    for (size_t i = 0; i < out.buckets.size(); i++) {
      out.buckets[i] = b[i].load(std::memory_order_relaxed);
    }
    out.p50_ns = estimate_quantile_from_buckets(out.buckets, out.count, 0.50);
    out.p99_ns = estimate_quantile_from_buckets(out.buckets, out.count, 0.99);
  };

  fill_lat(s.ht_init, ht_init_count_, ht_init_total_, ht_init_max_, ht_init_b_);
  fill_lat(s.ht_insert, ht_insert_count_, ht_insert_total_, ht_insert_max_,
           ht_insert_b_);
  fill_lat(s.ht_query, ht_query_count_, ht_query_total_, ht_query_max_,
           ht_query_b_);
  fill_lat(s.ht_delete, ht_delete_count_, ht_delete_total_, ht_delete_max_,
           ht_delete_b_);

  s.events.rebuild_down = ev_rebuild_down_.load(std::memory_order_relaxed);
  s.events.rebuild_up = ev_rebuild_up_.load(std::memory_order_relaxed);
  s.events.resize_start = ev_resize_start_.load(std::memory_order_relaxed);
  s.events.resize_finish = ev_resize_finish_.load(std::memory_order_relaxed);
  return s;
}

} // namespace otsh

