# CH5 三方案对比实验入口（V1 / V2 / V3）。
#
# Usage:
#   .\experiments\run_ch5.ps1                       # all variants, scale=1e4, core workload
#   .\experiments\run_ch5.ps1 -Quick                # 小规模冒烟
#   .\experiments\run_ch5.ps1 -Scale all            # 5e3~5e5 共 5 档
#   .\experiments\run_ch5.ps1 -Variant V3 -Workload core
#   .\experiments\run_ch5.ps1 -Runs 3 -Seed 42      # 多 seed 中位数

param(
    [ValidateSet("V1", "V2", "V3", "all")] [string]$Variant = "all",
    [ValidateSet("5e3", "1e4", "5e4", "1e5", "5e5", "all")] [string]$Scale = "1e4",
    [ValidateSet("core", "all")] [string]$Workload = "all",
    [switch]$Quick,
    [int]$Runs = 1,
    [string]$Seed = "1",
    [switch]$KeepDb,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$ExpDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ExpDir
$BuildDir = if ($env:OTSH_BUILD_DIR) { $env:OTSH_BUILD_DIR } else { Join-Path $RepoRoot "build" }
$ExpBin = Join-Path $BuildDir "otsh_ch5_variants.exe"

$Utf8NoBom = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $Utf8NoBom
[Console]::InputEncoding = $Utf8NoBom
$OutputEncoding = $Utf8NoBom
try { chcp 65001 | Out-Null } catch {}

Push-Location $RepoRoot
if (-not $SkipBuild) {
    cmake --build $BuildDir --target otsh_ch5_variants | Out-Null
}
if (-not (Test-Path $ExpBin)) { Write-Error "Missing $ExpBin"; Pop-Location; return }

$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$logPath = Join-Path $ExpDir "ch5_$ts.log"
[System.IO.File]::WriteAllText($logPath, "", $Utf8NoBom)

function Write-LogLine([string]$Line) {
  Write-Host $Line
  [System.IO.File]::AppendAllText($logPath, $Line + [Environment]::NewLine, $Utf8NoBom)
}

Write-LogLine "=== otsh_ch5_variants ==="

$variantList = if ($Variant -eq "all") { @("V1", "V2", "V3") } else { @($Variant) }
$scaleList = if ($Scale -eq "all") { @("5e3", "1e4", "5e4", "1e5", "5e5") } else { @($Scale) }
$workloadList = if ($Workload -eq "all") { @("core") } else { @($Workload) }

foreach ($sc in $scaleList) {
  foreach ($var in $variantList) {
    foreach ($wl in $workloadList) {
      $expArgs = @("--variant=$var", "--scale=$sc", "--workload=$wl",
                   "--seed=$Seed", "--runs=$Runs")
      if ($Quick)   { $expArgs += "--quick" }
      if ($KeepDb)  { $expArgs += "--keep-db" }
      if ($var -eq "V1") { $expArgs += "--fast-process-exit" }

      $banner = "=== run variant=$var scale=$sc workload=$wl ==="
      Write-LogLine $banner
      & $ExpBin @expArgs 2>&1 | ForEach-Object {
        Write-LogLine $_.ToString()
      }
      if ($LASTEXITCODE -ne 0) {
        $warn = "WARN: variant=$var scale=$sc workload=$wl exit=$LASTEXITCODE"
        Write-Warning $warn
        [System.IO.File]::AppendAllText($logPath, $warn + [Environment]::NewLine, $Utf8NoBom)
      }
    }
  }
}
Write-Host ""
Write-Host "Log -> $logPath"
Pop-Location
