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

Push-Location $webDir
try {
  $env:VITE_DEV_PROXY_TARGET = "http://127.0.0.1:$BackendPort"
  $env:CYPRESS_COVERAGE = "1"

  corepack pnpm install | Out-Host

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
  corepack pnpm run coverage:report | Out-Host

  Stop-Job $devJob -Force | Out-Null
  Remove-Job $devJob -Force | Out-Null
} finally {
  Pop-Location
  if ($backendProc -ne $null -and !$backendProc.HasExited) {
    Stop-Process -Id $backendProc.Id -Force
  }
}

