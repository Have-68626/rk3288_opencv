param(
    [Parameter(Mandatory = $true)]
    [string]$PackageName,

    [Parameter(Mandatory = $true)]
    [string]$ActivityName,

    [ValidateRange(1, 2147483647)][int]$Iterations = 50,
    [ValidateRange(1, 2147483647)][int]$TimeoutSec = 45,
    [string]$Serial = "",
    [string]$LogTag = "BENCH_CAMERA",
    [string]$OutputDir = "tests\bench"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-AdbPath {
    $sdkRoot = $env:ANDROID_SDK_ROOT
    if ([string]::IsNullOrWhiteSpace($sdkRoot)) { $sdkRoot = $env:ANDROID_HOME }
    if (-not [string]::IsNullOrWhiteSpace($sdkRoot)) {
        $candidate = Join-Path $sdkRoot "platform-tools\adb.exe"
        if (Test-Path $candidate) { return $candidate }
    }
    return "adb"
}

function Get-ConnectedDevices([string]$AdbPath) {
    $lines = & $AdbPath devices 2>$null
    if (-not $lines) { return @() }
    $devices = @()
    foreach ($line in $lines) {
        if ($line -match "^\s*([^\s]+)\s+device\s*$") {
            $devices += $Matches[1]
        }
    }
    return $devices
}

function New-AdbArgs([string]$Serial) {
    if ([string]::IsNullOrWhiteSpace($Serial)) { return @() }
    return @("-s", $Serial)
}

function Ensure-OutputDir([string]$Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Parse-FirstMatchValue([string[]]$Lines, [string]$Key) {
    foreach ($line in $Lines) {
        if ($line -match ('(?:^|\s)' + [regex]::Escape($Key) + '=([+]?[0-9]+)(?:\s|$)')) {
            return [int]$Matches[1]
        }
    }
    return $null
}

function Wait-ForBenchLines([string]$AdbPath, [string[]]$AdbArgs, [string]$LogTag, [int]$TimeoutSec) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $dump = @(& $AdbPath @AdbArgs logcat -d -v brief -s "$LogTag:I" 2>$null)
        if ($LASTEXITCODE -ne 0) { throw 'BENCH_QUERY_FAILED' }
        # logcat分区标题不是样本；等待同一轮的完整指标后才结束采集。
        $complete = $dump.Count -gt 0
        foreach ($key in @('TTFF_MS', 'CAPTURE_OK', 'CAPTURE_FAIL', 'CAMERA_SERVICE_RESTART')) {
            if ($null -eq (Parse-FirstMatchValue -Lines $dump -Key $key)) { $complete = $false }
        }
        if ($complete) { return $dump }
        Start-Sleep -Seconds 1
    }
    return @()
}

$adb = Resolve-AdbPath
$connected = @(Get-ConnectedDevices -AdbPath $adb)
if (-not $connected -or $connected.Count -eq 0) {
    throw "未检测到已连接设备，请先通过 adb 连接目标设备。"
}

if ([string]::IsNullOrWhiteSpace($Serial)) {
    if ($connected.Count -eq 1) {
        $Serial = $connected[0]
    } else {
        throw "检测到多个设备，请通过 -Serial 指定目标设备：$($connected -join ', ')"
    }
}

$adbArgs = New-AdbArgs -Serial $Serial
Ensure-OutputDir -Path $OutputDir

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$outCsv = Join-Path $OutputDir "bench_camera_$timestamp.csv"

$rows = New-Object System.Collections.Generic.List[object]

# Q-CQ-B08-008：样本计数，零样本时不得宣称完成。
$capturedSamples = 0

for ($i = 1; $i -le $Iterations; $i++) {
    & $adb @adbArgs logcat -c | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'BENCH_CLEAR_FAILED' }
    & $adb @adbArgs shell am force-stop $PackageName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'BENCH_STOP_FAILED' }
    $started = @(& $adb @adbArgs shell am start -n "$PackageName/$ActivityName" --ez BENCH true --ei BENCH_ITERATION $i 2>$null)
    if ($LASTEXITCODE -ne 0 -or ($started -match '(?i)Error:|Exception')) { throw 'BENCH_START_FAILED' }

    $lines = Wait-ForBenchLines -AdbPath $adb -AdbArgs $adbArgs -LogTag $LogTag -TimeoutSec $TimeoutSec
    if (-not $lines) {
        Write-Warning "Iteration $i 超时未采集到样本，跳过"
        continue
    }
    # Wait-ForBenchLines已验证全部必需指标，缺测不会计入成功样本。
    $capturedSamples++

    $ttff = Parse-FirstMatchValue -Lines $lines -Key "TTFF_MS"
    $captureOk = Parse-FirstMatchValue -Lines $lines -Key "CAPTURE_OK"
    $captureFail = Parse-FirstMatchValue -Lines $lines -Key "CAPTURE_FAIL"
    $cameraServiceRestart = Parse-FirstMatchValue -Lines $lines -Key "CAMERA_SERVICE_RESTART"

    $rows.Add([pscustomobject]@{
        timestamp = (Get-Date).ToString("o")
        serial = $Serial
        iteration = $i
        package = $PackageName
        activity = $ActivityName
        ttff_ms = $ttff
        capture_ok = $captureOk
        capture_fail = $captureFail
        camera_service_restart = $cameraServiceRestart
        log_tag = $LogTag
    }) | Out-Null
}

# Q-CQ-B08-008：空样本（adb 启动失败 / logcat 全程超时）不得宣称完成 → 以非零码退出。
if ($capturedSamples -eq 0) {
    Write-Error "BENCH_NO_SAMPLES 基准失败：未采集到完整指标，未生成有效结果。"
    exit 1
}

$rows | Export-Csv -Path $outCsv -NoTypeInformation -Encoding UTF8
Write-Output "已生成基准结果：$outCsv"
