param(
  [int]$BackendPort = 8080,
  [string]$BackendExe = "",
  [string]$WebDirectory = "",
  [string]$PnpmPath = "",
  [switch]$SkipInstall,
  [string]$ReportDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$webDir = if ($WebDirectory) { (Resolve-Path -LiteralPath $WebDirectory).Path } else { Join-Path $repoRoot "web" }
if ($ReportDirectory) {
  $ReportDirectory = [System.IO.Path]::GetFullPath($ReportDirectory)
  if (Get-ChildItem -LiteralPath $ReportDirectory -Filter 'cypress-*.xml' -ErrorAction SilentlyContinue) {
    throw '本次JUnit输出已有报告，拒绝复用旧证据'
  }
  New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
}
$priorEnvironment = @{}
foreach ($name in @('VITE_DEV_PROXY_TARGET', 'CYPRESS_COVERAGE')) {
  $priorEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$backendProc = $null
$devJob = $null
Push-Location -LiteralPath $webDir
try {
  # 本脚本只清理自己启动的服务，端口被占用时不能误测其他Vite实例。
  $portProbe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 5173)
  try { $portProbe.Start() }
  catch { throw 'E2E端口5173无法绑定，请先确认占用情况' }
  finally { $portProbe.Stop() }
  if ($BackendExe -ne '') {
    $backendProc = Start-Process -FilePath $BackendExe -WindowStyle Hidden -PassThru
  }
  $env:VITE_DEV_PROXY_TARGET = "http://127.0.0.1:$BackendPort"
  $env:CYPRESS_COVERAGE = '1'
  if (-not $SkipInstall) {
    if ($PnpmPath) { & $PnpmPath install --frozen-lockfile | Out-Host }
    else { corepack pnpm install --frozen-lockfile | Out-Host }
    if ($LASTEXITCODE -ne 0) { throw "pnpm install failed (exit=$LASTEXITCODE)" }
  }
  $devJob = Start-Job -ScriptBlock {
    param($dir)
    Set-Location -LiteralPath $dir
    $env:CYPRESS_COVERAGE = '1'
    node node_modules/vite/bin/vite.js --host 127.0.0.1 --port 5173 --strictPort | Out-Host
  } -ArgumentList $webDir
  $viteReady = $false
  for ($i = 0; $i -lt 30; $i++) {
    try {
      $req = [System.Net.WebRequest]::Create('http://127.0.0.1:5173')
      $req.Timeout = 1000
      $resp = $req.GetResponse()
      try { if ($resp.StatusCode -eq 200) { $viteReady = $true; break } }
      finally { $resp.Close() }
    } catch { }
    Start-Sleep -Seconds 1
  }
  if (-not $viteReady) { throw 'Vite dev server did not become ready within 30s' }
  if ($ReportDirectory) {
    # 直接传给Node启动器，避免pnpm版本对双横线转发语义不同。
    node scripts/e2e-run.mjs --reporter junit --reporter-options "mochaFile=$ReportDirectory/cypress-[hash].xml" | Out-Host
  } elseif ($PnpmPath) { & $PnpmPath run e2e:run:coverage | Out-Host }
  else { corepack pnpm run e2e:run:coverage | Out-Host }
  if ($LASTEXITCODE -ne 0) { throw "e2e:run:coverage failed (exit=$LASTEXITCODE)" }
  if ($PnpmPath) { & $PnpmPath run coverage:report | Out-Host }
  else { corepack pnpm run coverage:report | Out-Host }
  if ($LASTEXITCODE -ne 0) { throw "coverage:report failed (exit=$LASTEXITCODE)" }
} finally {
  Pop-Location
  try {
    if ($devJob -ne $null) {
      Stop-Job $devJob -ErrorAction Stop | Out-Null
      Remove-Job $devJob -Force -ErrorAction Stop | Out-Null
    }
  } finally {
    try {
      if ($backendProc -ne $null -and !$backendProc.HasExited) { Stop-Process -Id $backendProc.Id -ErrorAction Stop }
    } finally {
      foreach ($name in $priorEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $priorEnvironment[$name], 'Process')
      }
    }
  }
}
