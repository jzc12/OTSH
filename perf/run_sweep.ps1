# Multi-config OTSH perf sweep -> perf/results/bench_<ts>.{log,json}
# Usage: .\perf\run_sweep.ps1

$ErrorActionPreference = "Stop"
$PerfDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RunAll = Join-Path $PerfDir "run_all.ps1"

# Each row: n_insert, n_rand_queries, table_n_hint
$sweeps = @(
  @(2000, 20000, 4096),
  @(10000, 80000, 32768),
  @(30000, 240000, 65536),
  @(50000, 200000, 200000),
  @(50000, 200000, 1048576),
  @(100000, 400000, 262144),
  @(200000, 800000, 1048576)
)

$i = 0
foreach ($cfg in $sweeps) {
  $i++
  $nIns, $nRand, $hint = $cfg
  Write-Host ""
  Write-Host "=== sweep $i/$($sweeps.Count): n_insert=$nIns table_n_hint=$hint ==="
  & $RunAll $nIns $nRand $hint
}

Write-Host ""
Write-Host "Done. See perf/results/bench_*.log"
