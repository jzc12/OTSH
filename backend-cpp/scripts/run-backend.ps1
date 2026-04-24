$ErrorActionPreference = "Stop"

if (-not $env:DB_PASSWORD -or $env:DB_PASSWORD.Trim().Length -eq 0) {
  throw "Please set DB_PASSWORD (root password) before running."
}

if (-not $env:DB_HOST -or $env:DB_HOST.Trim().Length -eq 0) { $env:DB_HOST = "127.0.0.1" }
if (-not $env:DB_PORT -or $env:DB_PORT.Trim().Length -eq 0) { $env:DB_PORT = "3306" }
if (-not $env:DB_USER -or $env:DB_USER.Trim().Length -eq 0) { $env:DB_USER = "root" }
if (-not $env:DB_NAME -or $env:DB_NAME.Trim().Length -eq 0) { $env:DB_NAME = "otsh" }
if (-not $env:PORT -or $env:PORT.Trim().Length -eq 0) { $env:PORT = "8080" }
if (-not $env:LOG_FILE -or $env:LOG_FILE.Trim().Length -eq 0) { $env:LOG_FILE = "$PSScriptRoot\..\backend.log" }

Write-Host "Starting backend on :$env:PORT (DB=$env:DB_USER@$env:DB_HOST:$env:DB_PORT/$env:DB_NAME)"
Write-Host "Log file: $env:LOG_FILE"
& "$PSScriptRoot\..\build\otsh_backend.exe"

