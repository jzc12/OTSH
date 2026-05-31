# 第四章实验（口径对齐 experiments/out.log）

# Usage:

#   .\experiments\run.ps1              # 全量，runs=3

#   .\experiments\run.ps1 -Micro       # 渐进验证 n=200→5000（秒级）

#   .\experiments\run.ps1 -Quick       # 快速冒烟

#   .\experiments\run.ps1              # 全量：表1 n=10^3…10^5；表2–4 三档各跑一遍

#   .\experiments\run.ps1 -Scale all   # 表2–4 跑 1e3/1e4/1e5（默认）

#   .\experiments\run.ps1 -Scale 1e4     # 表2–4 仅 10^4

#   .\experiments\run.ps1 -Group n

#   .\experiments\run.ps1 -SkipBuild



param(

  [switch]$Micro,

  [switch]$Quick,

  [switch]$SkipBuild,

  [ValidateSet("1e3", "1e4", "1e5", "all", "both", "")]

  [string]$Scale = "all",

  [ValidateSet("micro", "n", "Kfixed", "k", "tier", "all", "")]

  [string]$Group = "all",

  [int]$Runs = 0,

  [string]$Seed = "1"

)



$ErrorActionPreference = "Stop"

$ExpDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$RepoRoot = Split-Path -Parent $ExpDir

$BuildDir = if ($env:OTSH_BUILD_DIR) { $env:OTSH_BUILD_DIR } else { Join-Path $RepoRoot "build" }

$ExpBin = Join-Path $BuildDir "otsh_experiment.exe"

$TestBin = Join-Path $BuildDir "otsh_tests.exe"



# 统一 UTF-8（无 BOM），避免 Tee-Object/Out-File 混用 UTF-16 导致 “P A S S” 乱码

$Utf8NoBom = New-Object System.Text.UTF8Encoding $false

[Console]::OutputEncoding = $Utf8NoBom

[Console]::InputEncoding = $Utf8NoBom

$OutputEncoding = $Utf8NoBom

try { chcp 65001 | Out-Null } catch {}



function Write-Utf8Log([string]$Path, [string[]]$Lines, [switch]$Append) {

  if (-not $Append) {

    [System.IO.File]::WriteAllText($Path, ($Lines -join "`n") + "`n", $Utf8NoBom)

    return

  }

  foreach ($line in $Lines) {

    [System.IO.File]::AppendAllText($Path, $line + "`n", $Utf8NoBom)

  }

}



Push-Location $RepoRoot

if (-not $SkipBuild) {

  cmake --build $BuildDir --target otsh_experiment otsh_tests

}

if (-not (Test-Path $ExpBin)) { Write-Error "Missing $ExpBin" }



if ($Runs -le 0) { $Runs = 1 }



$ts = Get-Date -Format "yyyyMMdd_HHmmss"

$logPath = Join-Path $ExpDir "ch4_$ts.log"

if ($Micro) {

  $groups = @("micro")

} elseif ($Group -and $Group -ne "all") {

  $groups = @($Group)

} else {

  $groups = @("n", "Kfixed", "k", "tier")

}



Write-Utf8Log $logPath @(

  "# OTSH Ch4 experiments $ts"

  "micro=$($Micro.IsPresent) quick=$($Quick.IsPresent) scale=$Scale runs=$Runs seed=$Seed"

  ""

)



Write-Host "=== otsh_tests ==="

$testLines = & $TestBin 2>&1 | ForEach-Object { "$_" }

foreach ($line in $testLines) { Write-Host $line }

Write-Utf8Log $logPath $testLines -Append

if ($LASTEXITCODE -ne 0) { Write-Warning "otsh_tests exit=$LASTEXITCODE" }



$ch4Rows = New-Object System.Collections.Generic.List[string]



foreach ($g in $groups) {

  Write-Host ""

  Write-Host "=== group=$g ==="

  $expArgs = @("--group=$g", "--seed=$Seed", "--runs=$Runs")

  if ($g -eq "n" -or $g -eq "micro") {
    # 表1 / micro 不含 scale 维度
  } elseif ($Scale -eq "all" -or $Scale -eq "both") {
    $expArgs += "--scale=all"
  } elseif ($Scale -ne "") {
    $expArgs += "--scale=$Scale"
  }

  if ($Micro) { $expArgs += "--micro" }

  if ($Quick) { $expArgs += "--quick" }

  $lines = & $ExpBin @expArgs 2>&1 | ForEach-Object { "$_" }

  foreach ($line in $lines) { Write-Host $line }

  Write-Utf8Log $logPath @("", "======== group=$g scale=$Scale ========") -Append

  Write-Utf8Log $logPath $lines -Append

  foreach ($line in $lines) {

    if ($line -match "^CH4_TABLE,") { $ch4Rows.Add($line) | Out-Null }

  }

  if ($LASTEXITCODE -ne 0) { Write-Warning "group=$g exit=$LASTEXITCODE" }

}



Write-Utf8Log $logPath @("", "## CH4_TABLE") -Append

Write-Utf8Log $logPath $ch4Rows -Append



Write-Host ""

Write-Host "Done -> $logPath"

Pop-Location

