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
#   5) 稳态口径（新增）：验收判据要求「剔除启动前 5s + 取稳态中位数」。
#      RK3288 设备 wall-clock 停在 2017 年、`logcat -v brief` 不输出时间戳、
#      App 日志也没有帧计数等单调量，**逐行时间映射不可得**，故采用「采样计数
#      近似」：以主机时钟测得采集窗口 W（SettleSec+DurationSec），暖机占比 =
#      WarmupSec / W，对每个指标按采集顺序丢弃前 floor(count × 暖机占比) 个样本。
#      该口径可复现但为近似（采样密度不均匀时会有偏差），已在报告与验收文档中
#      写明。既有 metrics 块仍为「全量样本」口径，稳态结果另存 steady_state，
#      故默认不改变既有回归报告口径；`-NoSteadyState` 可完全关闭稳态块。
#   6) 测试接缝：`-LogcatFile <路径>` 为**离线测试专用接缝**（默认空 = 照旧走 adb
#      真机采集）。提供该参数时脚本不接触设备/媒体门禁，只解析给定文本，便于在
#      无设备条件下验证「暖机剔除 + 稳态中位数」路径；生产路径行为不变。
#
# 用法：
#   $env:RK_PRIVATE_MEDIA_ROOT = "D:\private\face_media"
#   ./scripts/run-android-face-regression.ps1 -PackageName com.example.rk3288_opencv `
#       -ActivityName .MainActivity -DurationSec 60
#
# 离线自检（无设备）：
#   ./scripts/run-android-face-regression.ps1 -PackageName com.x -ActivityName .M `
#       -LogcatFile .\fake_logcat.txt -WarmupSec 5 -SettleSec 3 -DurationSec 60
# =============================================================================

param(
    [Parameter(Mandatory = $true)]
    [string]$PackageName,

    [Parameter(Mandatory = $true)]
    [string]$ActivityName,

    [string]$Serial = "",
    [int]$DurationSec = 60,
    [int]$SettleSec = 3,

    # 稳态口径参数（见头部行为要点 5）。WarmupSec 为命名暖机时长（秒，默认 5，对齐
    # 判据）；FpsThreshold 为结论行使用的 FPS 阈值（默认 15，对齐 A1 轻量档）；
    # MinSteadySamples 为「稳态样本过少」的最小样本数阈值；NoSteadyState 显式关闭
    # 稳态块（回到与既有报告完全一致的输出）。
    [double]$WarmupSec = 5,
    [double]$FpsThreshold = 15,
    [int]$MinSteadySamples = 5,
    [switch]$NoSteadyState,

    # 离线测试接缝（见头部行为要点 6）：非空时从该文件读取 logcat 文本，不接触设备。
    [string]$LogcatFile = "",

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

# 离线测试接缝开关：提供 -LogcatFile 时跳过设备/媒体门禁，只解析给定文本。
$fileMode = -not [string]::IsNullOrWhiteSpace($LogcatFile)

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

# 稳态窗口剔除（采样计数近似，见头部行为要点 5）：
# 按采集顺序丢弃前 Fraction 比例的样本；Fraction<=0 原样返回，>=1 返回空数组
# （由调用方判为「暖机窗口吃光全部样本」并显式失败/标注）。
function Get-SteadySlice([double[]]$Values, [double]$Fraction) {
    if (-not $Values -or $Values.Count -eq 0) { return @() }
    if ($Fraction -le 0.0) { return $Values }
    if ($Fraction -ge 1.0) { return @() }
    $drop = [int][math]::Floor($Values.Count * $Fraction)
    if ($drop -ge $Values.Count) { return @() }
    return $Values[$drop..($Values.Count - 1)]
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
# 环境门禁：缺 env 直接退出，绝不扫描用户目录（-LogcatFile 接缝模式下跳过）
# ══════════════════════════════════════════════════════════════════════════
$repoRoot = (Split-Path -Parent $PSScriptRoot)

# 输出根解析：默认/相对路径基于 $PSScriptRoot 派生的仓库根，避免依赖调用方 CWD。
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot "tests/reports/private_face_regression"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot $OutputRoot
}

# 媒体计数在两种模式下都要有值（接缝模式恒为 0，并在报告里标注）。
$mediaCount = 0
$mediaBytes = [long]0

if (-not $fileMode) {
    $mediaRootRaw = $env:RK_PRIVATE_MEDIA_ROOT
    if ([string]::IsNullOrWhiteSpace($mediaRootRaw)) {
        Fail 2 ("缺少环境变量 RK_PRIVATE_MEDIA_ROOT：私有媒体回归必须显式指定媒体根目录。" +
            "本脚本不会（也不允许）回退扫描 Desktop/Documents/Pictures/Downloads 等用户目录。")
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
} else {
    # 测试接缝：不接触媒体/设备，仅校验离线输入文件存在。
    if (-not (Test-Path -LiteralPath $LogcatFile -PathType Leaf)) {
        Fail 2 "LogcatFile 不存在（-LogcatFile 为离线测试接缝，需指向一个现有文本文件）：$LogcatFile"
    }
}

# ══════════════════════════════════════════════════════════════════════════
# 设备与输出准备
# ══════════════════════════════════════════════════════════════════════════
if (-not $fileMode) {
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

    # 去标识设备画像：只保留 SDK/ABI，序列号仅以哈希形式出现。
    $sdkInt = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.build.version.sdk"
    $abi = Get-ShellValue -AdbPath $adb -AdbArgs $adbArgs -Cmd "getprop ro.product.cpu.abi"
    $deviceIdHash = Get-Sha256Short -Text $Serial

    # 设备侧采集只依赖 App 自身输出的日志指标（fps/lat_ms/effective_ms/p95 等），
    # 不消费推送到设备的私有媒体：App 侧没有任何 regressionMediaDir 读取处，
    # 此前 -PushToDevice 推送的媒体不会被消费，已移除该半失效路径，
    # 既避免浪费设备存储与推送耗时，也避免给人「推送即生效」的错觉。
    # 采集窗口 W 用**主机时钟**（Stopwatch）测量：设备 wall-clock 不可信，
    # 暖机占比 WarmupSec / W 必须建立在可信时基上。
    $captureWatch = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-AdbQuiet -AdbPath $adb -AdbArgs $adbArgs -Command @("logcat", "-c")
    Invoke-AdbQuiet -AdbPath $adb -AdbArgs $adbArgs -Command @(
        "shell", "am", "start", "-n", "$PackageName/$ActivityName")

    Start-Sleep -Seconds $SettleSec
    Start-Sleep -Seconds $DurationSec
    $logLines = Invoke-AdbCapture -AdbPath $adb -AdbArgs $adbArgs -Command @("logcat", "-d", "-v", "brief")
    $captureWatch.Stop()
    $windowSec = $captureWatch.Elapsed.TotalSeconds
} else {
    # 测试接缝：设备画像留空；无真机墙钟可测，窗口取名义值 SettleSec+DurationSec。
    $sdkInt = ""
    $abi = ""
    $deviceIdHash = ""
    $adbArgs = @()
    $logLines = @(Get-Content -LiteralPath $LogcatFile -Encoding UTF8)
    $windowSec = [double]($SettleSec + $DurationSec)
}

$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $OutputRoot $ts
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$jsonPath = Join-Path $outDir "summary.json"
$csvPath = Join-Path $outDir "metrics.csv"

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

# ══════════════════════════════════════════════════════════════════════════
# 稳态窗口剔除 + 稳态中位数 + FPS 判据结论（口径见头部行为要点 5）
# ══════════════════════════════════════════════════════════════════════════
$steadyEnabled = -not $NoSteadyState
$warmupFraction = 0.0
if ($windowSec -gt 0) { $warmupFraction = $WarmupSec / $windowSec }
if ($warmupFraction -gt 1.0) { $warmupFraction = 1.0 }

$steadyMetricsObj = [ordered]@{}
$steadyCount = @{}
$steadyP50 = @{}
$steadyTotal = 0
if ($steadyEnabled) {
    if ($windowSec -le 0) {
        Fail 7 "采集窗口为 0s：无法计算暖机占比，拒绝生成含空稳态指标的报告。"
    }
    if ($WarmupSec -ge $windowSec) {
        # 「暖机窗口吃掉全部样本」：显式失败，绝不安静地输出 0 或空指标。
        Fail 7 ("暖机窗口（WarmupSec=$WarmupSec）不小于采集窗口（$([math]::Round($windowSec, 3))s）：" +
            "稳态样本必为空，拒绝生成空报告。")
    }
    foreach ($key in ($series.Keys | Sort-Object)) {
        $vals = $series[$key].ToArray()
        if ($vals.Count -eq 0) { continue }
        $steady = @(Get-SteadySlice -Values $vals -Fraction $warmupFraction)
        # 该指标被暖机窗口吃光：不写入空指标（避免安静地输出 0）。
        if ($steady.Count -eq 0) { continue }
        $steadyTotal += $steady.Count
        $steadyCount[$key] = $steady.Count
        $sP50 = Get-Percentile -Values $steady -P 50
        $steadyP50[$key] = $sP50
        $steadyMetricsObj[$key] = [ordered]@{
            count = $steady.Count
            min   = [math]::Round(($steady | Measure-Object -Minimum).Minimum, 3)
            p50   = $sP50
            p95   = Get-Percentile -Values $steady -P 95
            max   = [math]::Round(($steady | Measure-Object -Maximum).Maximum, 3)
        }
    }
    if ($steadyTotal -eq 0) {
        # 全库稳态样本为空：显式失败，绝不宣称采集成功却给出空稳态。
        Fail 7 ("稳态样本为空：暖机占比 $([math]::Round($warmupFraction, 4)) 吃光了全部指标样本，" +
            "拒绝生成空报告。")
    }
}

# FPS 判据结论：优先 ui_fps，其次 fps（大小写不敏感；App 侧日志键以 ui_fps 为主）。
$fpsKey = $null
foreach ($k in $steadyMetricsObj.Keys) { if ($k -ieq "ui_fps") { $fpsKey = $k; break } }
if ($null -eq $fpsKey) {
    foreach ($k in $steadyMetricsObj.Keys) { if ($k -ieq "fps") { $fpsKey = $k; break } }
}
$fpsSteadyCount = 0
$fpsSteadyP50 = $null
$fpsSatisfied = $null
if (-not $steadyEnabled) {
    $fpsConclusion = "steady_state_disabled"
} elseif ($null -eq $fpsKey) {
    # 未采集到 FPS 样本：显式标注「无法判定」，不得默认判为通过。
    $fpsConclusion = "missing_fps_samples"
} else {
    $fpsSteadyCount = [int]$steadyCount[$fpsKey]
    $fpsSteadyP50 = $steadyP50[$fpsKey]
    if ($fpsSteadyCount -lt $MinSteadySamples) {
        # 稳态样本过少：显式标注，不给出通过结论。
        $fpsConclusion = "insufficient_steady_samples"
    } else {
        $fpsSatisfied = ($null -ne $fpsSteadyP50 -and [double]$fpsSteadyP50 -ge $FpsThreshold)
        if ($fpsSatisfied) { $fpsConclusion = "satisfied" } else { $fpsConclusion = "not_satisfied" }
    }
}

# 采集来源：adb=真机采集；logcat_file=离线测试接缝（不接触设备、不读媒体）。
# 提到哈希表外定义是刻意的：JSON（下方 summary）与 stdout 两处都要用。下游常只读 stdout，
# 若 stdout 不带模式标记，误用 -LogcatFile 跑出的报告会被当成真机采集结果——
# 而该模式下 device_id_hash/abi 为空、media.file_count 恒为 0，单看指标是看不出来的。
$inputMode = if ($fileMode) { "logcat_file" } else { "adb" }

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
    input_mode            = $inputMode
    # 全量样本口径（与既有报告完全一致）。
    metrics               = $metricObj
    # 稳态口径（新增，可 -NoSteadyState 关闭）：剔除暖机窗口后的样本统计。
    steady_state          = [ordered]@{
        enabled             = $steadyEnabled
        warmup_sec          = $WarmupSec
        window_sec          = [math]::Round($windowSec, 3)
        warmup_fraction     = [math]::Round($warmupFraction, 4)
        excluded_note       = "采样计数近似：按采集顺序丢弃前 floor(count × warmup_fraction) 个样本；设备时钟不可信、logcat -v brief 无时间戳、App 日志无单调计数，故无法逐行映射时间。"
        steady_sample_count = $steadyTotal
        metrics             = $steadyMetricsObj
    }
    # FPS 判据结论行（仅 A1 的 FPS 部分自动化；其余仍人工）。
    fps_criterion         = [ordered]@{
        metric              = $fpsKey
        threshold_fps       = $FpsThreshold
        steady_sample_count = $fpsSteadyCount
        steady_p50          = $fpsSteadyP50
        satisfied           = $fpsSatisfied
        conclusion          = $fpsConclusion  # satisfied|not_satisfied|insufficient_steady_samples|missing_fps_samples|steady_state_disabled
    }
    reason_counts         = $reasonCounts
    effective_counts      = $effectiveCounts
    notes                 = @(
        "报告为匿名聚合：不含文件名、personId、模板、画面或原始 logcat。",
        "设备序列号仅以 SHA-256 截断哈希出现。",
        "缺 RK_PRIVATE_MEDIA_ROOT 时脚本直接退出，不扫描用户目录。",
        "设备侧采集依赖 App 日志指标（fps/lat_ms/effective_ms/p95），脚本不向设备推送媒体。",
        "稳态口径为采样计数近似（设备时钟不可信）：暖机占比 = WarmupSec / 采集窗口；fps_criterion 仅覆盖 A1 的 FPS 判据，其余验收项仍由人工判定。",
        "fps_criterion 的 ui_fps 样本只来自 logcat 的 MainActivity inferenceAutoTune 行（StatusService 的 FPS 文本只写悬浮窗、PerfReporter 无 fps 字段，均不在 logcat）；该日志仅在识别节流 auto 模式下产生，且约每 30s 一个样本。",
        "因此：窗口过短（默认 60s 仅约 2 个样本）会报 insufficient_steady_samples；未开启 auto 模式则报 missing_fps_samples。二者均为显式标注，不是脚本故障。",
        "本报告只做采集与聚合；最终验收判定由人工依据 docs/guides/rk3288-device-acceptance.md 完成。"
    )
    artifacts             = [ordered]@{
        metrics_csv = $csvPath
    }
    # 仅表示采集到有效样本；不代表任何验收项通过。
    collection_ok         = $true
}

$summary | ConvertTo-Json -Depth 10 | Set-Content -Path $jsonPath -Encoding UTF8

Write-Output "regression_report_dir=$outDir"
Write-Output "input_mode=$inputMode"
Write-Output "summary_json=$jsonPath"
Write-Output "metrics_csv=$csvPath"
Write-Output "sample_count=$sampleCount"
Write-Output "steady_sample_count=$steadyTotal"
Write-Output "fps_steady_metric=$fpsKey"
Write-Output "fps_steady_p50=$fpsSteadyP50"
Write-Output "fps_criterion=$fpsConclusion"
Write-Output "missing_model_count=$([int]($reasonCounts['missing_model'] | ForEach-Object { $_ }))"
