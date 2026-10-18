param([string]$Script, [string]$Mode, [string]$Case, [string]$OutputRoot, [int]$DeviceCount = 1)
$ErrorActionPreference = 'Stop'
# 仅在本进程注入函数替身，不能接触真实ADB或宿主SDK。
$env:ANDROID_SDK_ROOT = ''
$env:ANDROID_HOME = ''
$script:atHome = $false
function global:adb {
    $global:LASTEXITCODE = 0
    if ($args[0] -eq 'devices') {
        'List of devices attached'
        if ($DeviceCount -ge 1) { "fixture-device`tdevice" }
        if ($DeviceCount -ge 2) { "fixture-second`tdevice" }
        return
    }
    if ($args -contains 'start' -and $Case -eq 'start-failed') { $global:LASTEXITCODE = 1; return }
    if ($args -contains 'logcat' -and $args -contains '-d') {
        if ($Case -eq 'read-failed') { $global:LASTEXITCODE = 1; return }
        if ($Case -eq 'empty-log') { return }
        if ($Case -eq 'header-only') { '--------- beginning of main'; '--------- beginning of system'; return }
        if ($Mode -eq 'bench') {
            if ($Case -eq 'partial') { 'I/BENCH_CAMERA(1): TTFF_MS=20'; return }
            'I/BENCH_CAMERA(1): TTFF_MS=20 CAPTURE_OK=2 CAPTURE_FAIL=0 CAMERA_SERVICE_RESTART=0'
        } elseif ($Case -eq 'black-screen') { 'I/fixture(1): black screen' }
        else { 'I/fixture(1): frame_ready=true' }
        return
    }
    if ($args -contains 'shell' -and ($args -contains 'dumpsys activity activities' -or $args -contains 'dumpsys window windows')) {
        if ($Case -eq 'missing-focus') { return }
        if ($Case -eq 'invalid-back' -and $script:atHome) { 'mResumedActivity: null'; return }
        if ($Case -eq 'valid-focus-after-null') { 'mResumedActivity: null' }
        if ($script:atHome) { 'mResumedActivity: ActivityRecord{fixture u0 fixture.launcher/.Home t1}' }
        else { 'mResumedActivity: ActivityRecord{fixture u0 fixture.application/.Main t1}' }
        return
    }
    if ($args -contains 'start') { $script:atHome = $false; return }
    if ($args -contains 'keyevent') { $script:atHome = $true; return }
    if ($args -contains 'shell') { 'fixture' }
}
if ($Mode -eq 'bench') {
    & $Script -PackageName fixture.application -ActivityName .Main -Serial fixture-device -Iterations 1 -TimeoutSec 1 -OutputDir $OutputRoot
} else {
    & $Script -PackageName fixture.application -ActivityName .Main -Serial fixture-device -Iterations 1 -ForegroundSec 0 -BackgroundSec 0 -SettleSec 0 -OutputRoot $OutputRoot
}
exit $LASTEXITCODE
