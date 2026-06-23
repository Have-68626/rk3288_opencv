param(
    [Parameter(Mandatory = $true)]
    [string]$PackageName,

    [Parameter(Mandatory = $true)]
    [string]$ActivityName,

    [int]$Iterations = 50,
    [int]$ForegroundSec = 2,
    [int]$BackgroundSec = 1,
    [int]$SettleSec = 2,
    [string]$Serial = "",
    [string]$OutputRoot = "tests/reports/stability"
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

function New-AdbArgs([string]$TargetSerial) {
    if ([string]::IsNullOrWhiteSpace($TargetSerial)) { return @() }
    return @("-s", $TargetSerial)
}

function Get-ShellValue([string]$AdbPath, [string[]]$AdbArgs, [string]$Cmd) {
    try {
        $v = & $AdbPath @AdbArgs shell $Cmd 2>$null
        if ($null -eq $v) { return "" }
        return ($v | Out-String).Trim()
    } catch {
        return ""
    }
}

function Get-FocusSummary([string]$AdbPath, [string[]]$AdbArgs) {
    # Try modern dumpsys activity first, fall back to deprecated window dumpsys
    $dump = Get-ShellValue -AdbPath $AdbPath -AdbArgs $AdbArgs -Cmd "dumpsys activity activities"
    if (-not [string]::IsNullOrWhiteSpace($dump)) {
        $lines = $dump -split "`r?`n"
        foreach ($line in $lines) {
            if ($line -match "mResumedActivity|mFocusedActivity|mCurrentFocus") {
                return $line.Trim()
            }
        }
    }
    # Fallback: deprecated but still works on older Android
    $dump = Get-ShellValue -AdbPath $AdbPath -AdbArgs $AdbArgs -Cmd "dumpsys window windows"
    if (-not [string]::IsNullOrWhiteSpace($dump)) {
        $lines = $dump -split "`r?`n"
        foreach ($line in $lines) {
            if ($line -match "mCurrentFocus|mFocusedApp") {
                return $line.Trim()
            }
        }
    }
    return ""
}

$adb = Resolve-AdbPath
$connected = Get-ConnectedDevices -AdbPath $adb
if (-not $connected -or $connected.Count -eq 0) {
    throw "No connected Android device detected."
}

if ([string]::IsNullOrWhiteSpace($Serial)) {
    if ($connected.Count -eq 1) {
        $Serial = $connected[0]
    } else {
        throw "Multiple devices detected. Please set -Serial. Devices: $($connected -join ', ')"
    }
}

$adbArgs = New-AdbArgs -TargetSerial $Serial
$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $OutputRoot $ts
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$deviceInfoPath = Join-Path $outDir "device_info.txt"
$cyclesPath = Join-Path $outDir "cycles.csv"
$logcatPath = Join-Path $outDir "logcat_full.txt"
$summaryPath = Join-Path $outDir "summary.json"

$brand = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.product.brand"
$model = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.product.model"
$fingerprint = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.build.fingerprint"
$androidRelease = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.build.version.release"
$sdkInt = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.build.version.sdk"

@(
    "serial=$Serial"
    "brand=$brand"
    "model=$model"
    "android_release=$androidRelease"
    "sdk_int=$sdkInt"
    "fingerprint=$fingerprint"
) | Set-Content -Path $deviceInfoPath -Encoding UTF8

& $adb @adbArgs logcat -c | Out-Null

$rows = New-Object System.Collections.Generic.List[object]

for ($i = 1; $i -le $Iterations; $i++) {
    $startAt = (Get-Date).ToString("o")
    & $adb @adbArgs shell am start -n "$PackageName/$ActivityName" | Out-Null
    Start-Sleep -Seconds $ForegroundSec
    $focusFront = Get-FocusSummary -AdbPath $adb -AdbArgs $adbArgs

    & $adb @adbArgs shell input keyevent 3 | Out-Null
    Start-Sleep -Seconds $BackgroundSec
    $focusBack = Get-FocusSummary -AdbPath $adb -AdbArgs $adbArgs

    $rows.Add([pscustomobject]@{
        iteration = $i
        started_at = $startAt
        front_focus = $focusFront
        back_focus = $focusBack
        package = $PackageName
        activity = $ActivityName
    }) | Out-Null
}

& $adb @adbArgs shell am start -n "$PackageName/$ActivityName" | Out-Null
Start-Sleep -Seconds $SettleSec
& $adb @adbArgs logcat -d -v threadtime | Set-Content -Path $logcatPath -Encoding UTF8

$rows | Export-Csv -Path $cyclesPath -NoTypeInformation -Encoding UTF8

$logLines = @(Get-Content -Path $logcatPath -ErrorAction SilentlyContinue)
$blackPattern = "(?i)(black\s*screen|blackscreen|黑屏|surface.*(invalid|lost)|preview.*(freeze|stuck)|ANR)"
$abnormalPattern = "handleAbnormalEvent"
$blackMatches = @($logLines | Select-String -Pattern $blackPattern)
$abnormalMatches = @($logLines | Select-String -Pattern $abnormalPattern)

$summary = [ordered]@{
    timestamp = $ts
    serial = $Serial
    package = $PackageName
    activity = $ActivityName
    iterations = $Iterations
    foreground_sec = $ForegroundSec
    background_sec = $BackgroundSec
    settle_sec = $SettleSec
    device = [ordered]@{
        brand = $brand
        model = $model
        android_release = $androidRelease
        sdk_int = $sdkInt
        fingerprint = $fingerprint
    }
    blackscreen_count = $blackMatches.Count
    abnormal_event_count = $abnormalMatches.Count
    pass = ($blackMatches.Count -eq 0)
    artifacts = [ordered]@{
        device_info = $deviceInfoPath
        cycles_csv = $cyclesPath
        logcat_full = $logcatPath
    }
}

$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8

Write-Output "stability_report_dir=$outDir"
Write-Output "summary_json=$summaryPath"
Write-Output "pass=$($summary.pass)"
