# 构建并运行 OTSH 性能基准；JSON 写入 perf/results/
# 用法: .\perf\run_all.ps1 [n_insert] [n_rand_queries] [table_n_hint]
# 环境变量: OTSH_SEED, OTSH_BUILD_DIR (默认 ..\build)

$ErrorActionPreference = "Stop"
$PerfDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $PerfDir
$BuildDir = if ($env:OTSH_BUILD_DIR) { $env:OTSH_BUILD_DIR } else { Join-Path $RepoRoot "build" }
$ResultsDir = Join-Path $PerfDir "results"
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

Push-Location $RepoRoot
if (-not (Test-Path $BuildDir)) {
  cmake -B $BuildDir -G "MinGW Makefiles"
}
cmake --build $BuildDir --target otsh_perf
$Exe = Join-Path $BuildDir "otsh_perf.exe"
if (-not (Test-Path $Exe)) {
  $Exe = Join-Path $BuildDir "otsh_perf"
}
$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$JsonPath = Join-Path $ResultsDir "bench_$ts.json"
$LogPath = Join-Path $ResultsDir "bench_$ts.log"
# Native EXE stderr is mapped to PowerShell's error stream; with
# $ErrorActionPreference = Stop that aborts the script. Redirect via
# Start-Process so human-readable bench output stays a plain file.
$ArgList = [string[]]@($args | ForEach-Object { "$_" })
$sp = @{
  FilePath               = $Exe
  RedirectStandardOutput = $JsonPath
  RedirectStandardError  = $LogPath
  NoNewWindow            = $true
  Wait                   = $true
  PassThru               = $true
}
if ($ArgList.Count -gt 0) {
  $sp.ArgumentList = $ArgList
}
$p = Start-Process @sp
if ($null -ne $p.ExitCode -and $p.ExitCode -ne 0) {
  throw "otsh_perf exited with code $($p.ExitCode)"
}
Write-Host "Wrote JSON: $JsonPath"
Write-Host "Human log: $LogPath"
Pop-Location
