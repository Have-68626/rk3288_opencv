param([string]$Driver, [string]$Case, [string]$Report)
$ErrorActionPreference = 'Stop'
$global:fixtureCalls = [System.Collections.Generic.List[string]]::new()
function global:fixture-pnpm {
    $global:fixtureCalls.Add(($args -join '|')); $global:LASTEXITCODE = 0
}
function global:node {
    $global:fixtureCalls.Add(($args -join '|')); $global:LASTEXITCODE = 0
}
$global:fixtureJobIds = [System.Collections.Generic.List[int]]::new()
function global:corepack {
    $global:LASTEXITCODE = 0
    if (($Case -eq 'install-failed' -and $args -contains 'install') -or
        ($Case -eq 'e2e-failed' -and $args -contains 'e2e:run:coverage') -or
        ($Case -eq 'coverage-failed' -and $args -contains 'coverage:report')) { $global:LASTEXITCODE = 7 }
}
function global:Start-Job {
    param([scriptblock]$ScriptBlock, $ArgumentList)
    # 使用真实PowerShell作业提供本机空HTTP响应；不启动Vite或访问外网。
    $job = Microsoft.PowerShell.Core\Start-Job -ArgumentList $Case -ScriptBlock {
        param($Case)
        if ($Case -eq 'no-ready') { Start-Sleep -Seconds 300; return }
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 5173)
        $listener.Start()
        try {
            while ($true) {
                if (-not $listener.Pending()) { Start-Sleep -Milliseconds 50; continue }
                $client = $listener.AcceptTcpClient()
                try {
                    $stream = $client.GetStream()
                    $stream.ReadTimeout = 1000
                    $request = [byte[]]::new(4096)
                    $null = $stream.Read($request, 0, $request.Length)
                    $bytes = [System.Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
                    $stream.Write($bytes, 0, $bytes.Length)
                } finally { $client.Dispose() }
            }
        } finally { $listener.Stop() }
    }
    $global:fixtureJobIds.Add($job.Id)
    return $job
}
function global:Start-Sleep {
    param([int]$Seconds)
    Microsoft.PowerShell.Utility\Start-Sleep -Milliseconds 200
}
$failed = $false
$reason = ''
try {
    if ($Case -eq 'coordinated') { & $Driver -WebDirectory (Join-Path (Split-Path (Split-Path $Driver)) 'web') -PnpmPath 'fixture-pnpm' -SkipInstall -ReportDirectory (Join-Path (Split-Path $Report) 'junit') }
    else { & $Driver }
} catch { $failed = $true; $reason = $_.FullyQualifiedErrorId }
$remaining = @($global:fixtureJobIds | ForEach-Object { Get-Job -Id $_ -ErrorAction SilentlyContinue })
try {
    @{ failed = $failed; created = $global:fixtureJobIds.Count; remaining = $remaining.Count; reason = $reason; calls = @($global:fixtureCalls) } |
        ConvertTo-Json | Set-Content -LiteralPath $Report -Encoding utf8
} finally {
    # 检测结果先落盘；即使变异破坏正式清理，夹具也只收回自己的作业。
    foreach ($job in $remaining) {
        Microsoft.PowerShell.Core\Stop-Job $job -ErrorAction SilentlyContinue
        Microsoft.PowerShell.Core\Remove-Job $job -Force -ErrorAction SilentlyContinue
    }
}
