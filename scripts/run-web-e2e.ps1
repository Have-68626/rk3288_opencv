param(
  [int]$BackendPort = 8080,
  [string]$BackendExe = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$webDir = Join-Path $repoRoot "web"

$backendProc = $null
if ($BackendExe -ne "") {
  $backendProc = Start-Process -FilePath $BackendExe -PassThru
}

# Q-CQ-B08-006：devJob 先初始化为 $null，finally 兜底清理时按 null 跳过
$devJob = $null

Push-Location $webDir
try {
  $env:VITE_DEV_PROXY_TARGET = "http://127.0.0.1:$BackendPort"
  $env:CYPRESS_COVERAGE = "1"

  corepack pnpm install | Out-Host
  # Q-CQ-B08-006：$ErrorActionPreference="Stop" 不影响原生命令退出码，必须显式检查 $LASTEXITCODE
  if ($LASTEXITCODE -ne 0) { throw "pnpm install failed (exit=$LASTEXITCODE)" }

  $devJob = Start-Job -ScriptBlock {
    param($dir)
    Set-Location $dir
    $env:VITE_DEV_PROXY_TARGET = $env:VITE_DEV_PROXY_TARGET
    $env:CYPRESS_COVERAGE = $env:CYPRESS_COVERAGE
    corepack pnpm run dev -- --host 127.0.0.1 --port 5173 | Out-Host
  } -ArgumentList $webDir

  # Wait for Vite dev server to be ready (up to 30s)
  $viteReady = $false
  for ($i = 0; $i -lt 30; $i++) {
    try {
      $req = [System.Net.WebRequest]::Create("http://127.0.0.1:5173")
      $req.Timeout = 1000
      $resp = $req.GetResponse()
      if ($resp.StatusCode -eq 200) { $viteReady = $true; $resp.Close(); break }
      $resp.Close()
    } catch { }
    Start-Sleep -Seconds 1
  }
  if (-not $viteReady) { throw "Vite dev server did not become ready within 30s" }

  corepack pnpm run e2e:run:coverage | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "e2e:run:coverage failed (exit=$LASTEXITCODE)" }

  corepack pnpm run coverage:report | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "coverage:report failed (exit=$LASTEXITCODE)" }
} finally {
  Pop-Location
  # Q-CQ-B08-006：任何退出路径（含 throw 提前退出）都兜底清理后台 job，避免残留
  if ($devJob -ne $null) {
    Stop-Job $devJob -Force -ErrorAction SilentlyContinue | Out-Null
    Remove-Job $devJob -Force -ErrorAction SilentlyContinue | Out-Null
  }
  if ($backendProc -ne $null -and !$backendProc.HasExited) {
    Stop-Process -Id $backendProc.Id -Force
  }
}

