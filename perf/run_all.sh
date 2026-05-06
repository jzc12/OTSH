#!/usr/bin/env bash
# 构建并运行 OTSH 性能基准；JSON 写入 perf/results/
# 用法: ./perf/run_all.sh [n_insert] [n_rand_queries] [table_n_hint]
# 环境变量: OTSH_SEED, OTSH_BUILD_DIR (默认 ../build)

set -euo pipefail
PERF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$PERF_DIR/.." && pwd)"
BUILD_DIR="${OTSH_BUILD_DIR:-$REPO_ROOT/build}"
RESULTS_DIR="$PERF_DIR/results"
mkdir -p "$RESULTS_DIR"

cd "$REPO_ROOT"
if [[ ! -d "$BUILD_DIR" ]]; then
  cmake -B "$BUILD_DIR"
fi
cmake --build "$BUILD_DIR" --target otsh_perf
EXE="$BUILD_DIR/otsh_perf"
TS="$(date +%Y%m%d_%H%M%S)"
JSON="$RESULTS_DIR/bench_${TS}.json"
LOG="$RESULTS_DIR/bench_${TS}.log"
"$EXE" "$@" 2>"$LOG" | tee "$JSON"
echo "Wrote JSON: $JSON"
echo "Human log: $LOG"
