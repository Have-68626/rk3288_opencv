param(
    [string]$NodePath = 'node',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
# 在脚本作用域保留原生命令退出码，不受调用方的异常化偏好影响。
$PSNativeCommandUseErrorActionPreference = $false
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'tests/reports/docs-sync-audit' }
$auditExit = 127
$hygieneExit = 127
Push-Location -LiteralPath $repoRoot
try {
    # 两项都运行并分别保留退出码，避免后一项成功掩盖前一项失败。
    try {
        $null = & $NodePath 'scripts/docs-sync-audit.js' '--skip-links' '--out-dir' $OutputDir 2>&1
        $auditExit = $LASTEXITCODE
    } catch { $auditExit = 127 }
    try {
        $null = & $NodePath 'scripts/clean-repo-junk.js' 'scan' '--ci' 2>&1
        $hygieneExit = $LASTEXITCODE
    } catch { $hygieneExit = 127 }
} finally { Pop-Location }
Write-Output "AUDIT_EXIT=$auditExit"
Write-Output "HYGIENE_EXIT=$hygieneExit"
if ($auditExit -ne 0) { exit $auditExit }
exit $hygieneExit
