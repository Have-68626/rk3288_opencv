# =============================================================================
# run-android-face-regression.ps1
#
# 私有媒体人脸回归采集器（Android）。
#
# 行为要点（与任务 19 约束一致）：
#   1) 环境门禁：必须显式提供 $env:RK_PRIVATE_MEDIA_ROOT；缺失即退出,
#      **绝不回退扫描用户目录**（Desktop/Documents/Pictures/Downloads 等）。
#   2) 私有媒体不入库：媒体目录必须在仓库之外；脚本只统计数量/字节数,
#      不复制任何媒体进仓库, 报告目录位于 tests/reports/（已 gitignore）。
#   3) 报告匿名聚合：只输出数值指标的 count/min/p50/p95/max 与 reason 计数,
#      设备序列号以 SHA-256 截断作为去标识 device_id_hash；
#      不落盘原始 logcat、不写文件名/personId/模板/画面。
#   4) 不向设备推送媒体：设备侧采集只依赖 App 自身日志指标（fps/lat_ms/
#      effective_ms/p95 等）；App 侧无 regressionMediaDir 读取处，
#      推送媒体不会被消费，故不提供该路径。
#
# 用法：
#   $env:RK_PRIVATE_MEDIA_ROOT = "D:\private\face_media"
#   ./scripts/run-android-face-regression.ps1 -PackageName com.example.rk3288_opencv `
#       -ActivityName .MainActivity -DurationSec 60
# =============================================================================

param(
    [Parameter(Mandatory = $true)]
    [string]$PackageName,

    [Parameter(Mandatory = $true)]
    [string]$ActivityName,

    [string]$Serial = "",
    [int]$DurationSec = 60,
    [int]$SettleSec = 3,
    # 输出根目录：留空时基于 $PSScriptRoot 派生的仓库根解析到
    # tests/reports/private_face_regression；相对路径按仓库根解析，绝对值原样使用。
    # 这样不依赖调用方的当前工作目录（CWD）。
    [string]$OutputRoot = "",

    # 指标键允许清单（只聚合这些键的数值，避免把身份类字段带入报告）。
    [string]$MetricKeyPattern = "^(?i)(ui_fps|fps|lat_ms|effective_ms|base_ms|from_ms|to_ms|p50|p95|p50_ms|p95_ms|frame_ms|detect_ms|infer_ms|benchmarkp95ms|frames|faces|count)$",

    # 需要单独计数的 reason / 状态旗标。
    [string]$ReasonPattern = "reason=([A-Za-z_][A-Za-z0-9_]*)",
    [string]$EffectivePattern = "effective=(true|false)"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# 支持扩展名（大小写不敏感）。
$script:MediaExtensions = @(".jpg", ".jpeg", ".png", ".bmp", ".mp4", ".avi", ".mkv", ".mov")

# 终止但保留明确退出码：Write-Error 在 Stop 下会抛异常并覆盖 exit 码，故用 Continue。
function Fail([int]$Code, [string]$Message) {
    Write-Error -Message $Message -ErrorAction Continue
    exit $Code
}

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
    # adb 首次启动会往 stderr 写 "daemon not running"；在 Stop 模式下会被当成
    # 终止错误，故本函数内临时降级为 Continue 并只解析 stdout 的设备行。
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $lines = @(& $AdbPath devices 2>$null) }
    catch { $lines = @() }
    finally { $ErrorActionPreference = $prev }
    if (-not $lines) { return @() }
    $devices = @()
    foreach ($line in $lines) {
        if ($line -match "^\s*([^\s]+)\s+device\s*$") { $devices += $Matches[1] }
    }
    return @($devices)
}

# 运行 adb 子命令但不丢弃其 stdout（logcat -d 需要）；忽略 stderr 噪声。
function Invoke-AdbCapture([string]$AdbPath, [string[]]$AdbArgs, [string[]]$Command) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { return @(& $AdbPath @AdbArgs @Command 2>$null) }
    catch { return @() }
    finally { $ErrorActionPreference = $prev }
}

# 运行 adb 子命令且丢弃输出（logcat -c / am start / push / rm 等）。
function Invoke-AdbQuiet([string]$AdbPath, [string[]]$AdbArgs, [string[]]$Command) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $AdbPath @AdbArgs @Command 2>$null | Out-Null }
    catch { }
    finally { $ErrorActionPreference = $prev }
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
    } catch { return "" }
}

function Get-Percentile([double[]]$Values, [double]$P) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 1) { return [math]::Round($sorted[0], 3) }
    $rank = ($P / 100.0) * ($sorted.Count - 1)
    $lo = [math]::Floor($rank)
    $hi = [math]::Ceiling($rank)
    if ($lo -eq $hi) { return [math]::Round($sorted[[int]$lo], 3) }
    $frac = $rank - $lo
    $interp = $sorted[[int]$lo] + ($sorted[[int]$hi] - $sorted[[int]$lo]) * $frac
    return [math]::Round($interp, 3)
}

function Get-Sha256Short([string]$Text) {
    if ([string]::IsNullOrEmpty($Text)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $hash = $sha.ComputeHash($bytes)
        $hex = -join ($hash | ForEach-Object { $_.ToString("x2") })
        return $hex.Substring(0, 12)
    } finally { $sha.Dispose() }
}

# ══════════════════════════════════════════════════════════════════════════
# 环境门禁：缺 env 直接退出，绝不扫描用户目录
# ══════════════════════════════════════════════════════════════════════════
$mediaRootRaw = $env:RK_PRIVATE_MEDIA_ROOT
if ([string]::IsNullOrWhiteSpace($mediaRootRaw)) {
    Fail 2 ("缺少环境变量 RK_PRIVATE_MEDIA_ROOT：私有媒体回归必须显式指定媒体根目录。" +
        "本脚本不会（也不允许）回退扫描 Desktop/Documents/Pictures/Downloads 等用户目录。")
}

$repoRoot = (Split-Path -Parent $PSScriptRoot)

# 输出根解析：默认/相对路径基于 $PSScriptRoot 派生的仓库根，避免依赖调用方 CWD。
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot "tests/reports/private_face_regression"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot $OutputRoot
}
try { $mediaRoot = (Resolve-Path -LiteralPath $mediaRootRaw).Path }
catch {
    Fail 3 "RK_PRIVATE_MEDIA_ROOT 指向的路径不存在：$mediaRootRaw"
}
if (-not (Test-Path -LiteralPath $mediaRoot -PathType Container)) {
    Fail 3 "RK_PRIVATE_MEDIA_ROOT 不是目录：$mediaRoot"
}

# 私有媒体不入库：媒体根必须在仓库之外，避免误提交。
$repoFull = (Resolve-Path -LiteralPath $repoRoot).Path.TrimEnd('\', '/')
$mediaFull = $mediaRoot.TrimEnd('\', '/')
if ($mediaFull -eq $repoFull -or $mediaFull.StartsWith($repoFull + [System.IO.Path]::DirectorySeparatorChar)) {
    Fail 4 "媒体根目录位于仓库内（$mediaFull）：私有媒体不入库，请把 RK_PRIVATE_MEDIA_ROOT 指向仓库之外的目录。"
}

# 仅统计显式媒体目录下的媒体文件数量与字节数（不读取内容、不记录文件名）。
$mediaCount = 0
$mediaBytes = [long]0
$supported = @($script:MediaExtensions)
Get-ChildItem -LiteralPath $mediaRoot -File -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    if ($supported -contains $_.Extension.ToLowerInvariant()) {
        $mediaCount++
        $mediaBytes += $_.Length
    }
}
if ($mediaCount -eq 0) {
    Fail 5 "媒体根目录下未发现受支持的媒体文件（$($supported -join ', ')）：无可回归样本，拒绝生成空报告。"
}

# ══════════════════════════════════════════════════════════════════════════
# 设备与输出准备
# ══════════════════════════════════════════════════════════════════════════
$adb = Resolve-AdbPath
$connected = @(Get-ConnectedDevices -AdbPath $adb)
if (-not $connected -or $connected.Count -eq 0) {
    Fail 6 "未检测到已连接设备，请先通过 adb 连接目标设备（例如 adb connect 192.168.5.2:5555）。"
}
if ([string]::IsNullOrWhiteSpace($Serial)) {
    if ($connected.Count -eq 1) { $Serial = $connected[0] }
    else { Fail 6 "检测到多个设备，请通过 -Serial 指定目标设备：$($connected -join ', ')" }
}
$adbArgs = New-AdbArgs -TargetSerial $Serial

$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $OutputRoot $ts
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$jsonPath = Join-Path $outDir "summary.json"
$csvPath = Join-Path $outDir "metrics.csv"

# 去标识设备画像：只保留 SDK/ABI，序列号仅以哈希形式出现。
$sdkInt = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.build.version.sdk"
$abi = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.product.cpu.abi"
$deviceIdHash = Get-Sha256Short -Text $Serial

# 设备侧采集只依赖 App 自身输出的日志指标（fps/lat_ms/effective_ms/p95 等），
# 不消费推送到设备的私有媒体：App 侧没有任何 regressionMediaDir 读取处，
# 此前 -PushToDevice 推送的媒体不会被消费，已移除该半失效路径，
# 既避免浪费设备存储与推送耗时，也避免给人「推送即生效」的错觉。
Invoke-AdbQuiet -AdbPath $adb -AdbArgs $adbArgs -Command @("logcat", "-c")
Invoke-AdbQuiet -AdbPath $adb -AdbArgs $adbArgs -Command @(
    "shell", "am", "start", "-n", "$PackageName/$ActivityName")

Start-Sleep -Seconds $SettleSec
Start-Sleep -Seconds $DurationSec
$logLines = Invoke-AdbCapture -AdbPath $adb -AdbArgs $adbArgs -Command @("logcat", "-d", "-v", "brief")

# ══════════════════════════════════════════════════════════════════════════
# 匿名聚合：只保留允许清单内的数值指标 + reason/effective 计数
# ══════════════════════════════════════════════════════════════════════════
$metricRegex = [regex]"(?<key>[A-Za-z_][A-Za-z0-9_]*)[=:]\s*(?<val>-?\d+(?:\.\d+)?)"
$series = @{}
$reasonCounts = @{}
$effectiveCounts = @{}

foreach ($line in $logLines) {
    if ([string]::IsNullOrEmpty($line)) { continue }

    foreach ($m in $metricRegex.Matches($line)) {
        $key = $m.Groups["key"].Value
        if ($key -notmatch $MetricKeyPattern) { continue }
        $val = 0.0
        if (-not [double]::TryParse($m.Groups["val"].Value,
                [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture, [ref]$val)) { continue }
        if (-not $series.ContainsKey($key)) { $series[$key] = New-Object System.Collections.Generic.List[double] }
        $series[$key].Add($val) | Out-Null
    }

    foreach ($rm in [regex]::Matches($line, $ReasonPattern)) {
        $reason = $rm.Groups[1].Value
        if (-not $reasonCounts.ContainsKey($reason)) { $reasonCounts[$reason] = 0 }
        $reasonCounts[$reason]++
    }
    foreach ($em in [regex]::Matches($line, $EffectivePattern)) {
        $eff = $em.Groups[1].Value.ToLowerInvariant()
        if (-not $effectiveCounts.ContainsKey($eff)) { $effectiveCounts[$eff] = 0 }
        $effectiveCounts[$eff]++
    }
}

# 汇总每个指标的 count/min/p50/p95/max（不含任何原始行）。
$metricRows = New-Object System.Collections.Generic.List[object]
$metricObj = [ordered]@{}
foreach ($key in ($series.Keys | Sort-Object)) {
    $vals = $series[$key].ToArray()
    if ($vals.Count -eq 0) { continue }
    $row = [ordered]@{
        metric   = $key
        count    = $vals.Count
        min      = [math]::Round(($vals | Measure-Object -Minimum).Minimum, 3)
        p50      = Get-Percentile -Values $vals -P 50
        p95      = Get-Percentile -Values $vals -P 95
        max      = [math]::Round(($vals | Measure-Object -Maximum).Maximum, 3)
    }
    $metricRows.Add([pscustomobject]$row) | Out-Null
    $metricObj[$key] = [ordered]@{ count = $row.count; min = $row.min; p50 = $row.p50; p95 = $row.p95; max = $row.max }
}

$sampleCount = 0
foreach ($k in $series.Keys) { $sampleCount += $series[$k].Count }

# 零样本不得宣称完成（与 bench_camera_adb.ps1 同口径）。
if ($sampleCount -eq 0) {
    Fail 1 "回归失败：未从 logcat 采集到任何允许清单内的指标样本，未生成有效报告。"
}

$metricRows | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8

$summary = [ordered]@{
    schema                = "rk_private_face_regression_v1"
    timestamp             = $ts
    # 去标识：不写序列号/机型/指纹，仅哈希与 ABI/SDK。
    device_id_hash        = $deviceIdHash
    abi                   = $abi
    sdk_int               = $sdkInt
    package               = $PackageName
    duration_sec          = $DurationSec
    settle_sec            = $SettleSec
    media                 = [ordered]@{
        file_count = $mediaCount      # 仅数量/字节数，绝不含文件名或路径
        total_bytes = $mediaBytes
    }
    metrics               = $metricObj
    reason_counts         = $reasonCounts
    effective_counts      = $effectiveCounts
    notes                 = @(
        "报告为匿名聚合：不含文件名、personId、模板、画面或原始 logcat。",
        "设备序列号仅以 SHA-256 截断哈希出现。",
        "缺 RK_PRIVATE_MEDIA_ROOT 时脚本直接退出，不扫描用户目录。",
        "设备侧采集依赖 App 日志指标（fps/lat_ms/effective_ms/p95），脚本不向设备推送媒体。",
        "本报告只做采集与聚合；验收判定由人工依据 docs/guides/rk3288-device-acceptance.md 完成。"
    )
    artifacts             = [ordered]@{
        metrics_csv = $csvPath
    }
    # 仅表示采集到有效样本；不代表任何验收项通过。
    collection_ok         = $true
}

$summary | ConvertTo-Json -Depth 10 | Set-Content -Path $jsonPath -Encoding UTF8

Write-Output "regression_report_dir=$outDir"
Write-Output "summary_json=$jsonPath"
Write-Output "metrics_csv=$csvPath"
Write-Output "sample_count=$sampleCount"
Write-Output "missing_model_count=$([int]($reasonCounts['missing_model'] | ForEach-Object { $_ }))"
