<!-- DOCSYNC_DOC
type=runbook
status=living
as_of=2026-10-17
-->

# Android只读采样与离线校验工具

这些工具从[缓存迁入方案](../plans/2026-10-17-cache-tools-integration.md)任务1–3整理，Python仅使用标准库。不会自动连接历史地址、安装/启动/停止应用、开启相机、清logcat或改防火墙；设备状态校验也不会代替操作者执行清理。所有设备必须显式指定serial，当前任务只用本地替身验证，未访问真实设备。

CLI输出只含固定状态或错误码；JSON只保留白名单数值、必要摘要和判据，不保存原始ADB输出、映射路径、画面或身份。每次使用新的输出文件，已有文件（含链接）会被拒绝，不自动覆盖。

## 文档门禁

仓库任意工作目录执行 `scripts/run-docs-checks.ps1` 均从脚本位置定位仓库根。以仓库根为例：

```powershell
pwsh -NoProfile -File scripts/run-docs-checks.ps1 -OutputDir cache/docs-checks/my-run
```

可用 `-NodePath` 显式指定Node。包装器依次运行现有文档审计和卫生扫描，分别输出 `AUDIT_EXIT`、`HYGIENE_EXIT`；审计失败优先返回其退出码，否则返回卫生扫描码，命令不可用为127。原始子进程输出不转储；文档审计细节在显式报告目录查看。

## 只读运行时采样

调用前由操作者按已授权方式准备目标应用；本工具不启动应用。下面命令会询问本机ADB路径和明确serial：

```powershell
$adb = Read-Host 'ADB可执行文件完整路径'
$serial = Read-Host '目标ADB serial'
python scripts/android/collect-runtime-metrics.py --adb $adb --serial $serial `
  --package com.example.rk3288_opencv --duration 60 --interval 5 `
  --output cache/android-validation/my-run/runtime.json
```

采样覆盖进程CPU（单核百分比，允许超过100%）、RSS/HWM、线程、PSS、CPU0–3频率和thermal_zone0原始整数。温度不猜板级单位或阈值；不代表完整识别性能或端到端时延。

PSS优先读取dumpsys正值；零值或缺测时只在可读的smaps中汇总Pss，逐样本标明 `pss_source` 和dumpsys零值/缺测状态，不将0当低内存通过。没有权限时不提权。CPU先读getconf，缺失时读取同一cat工具ELF类及auxv的AT_CLKTCK；字宽/时钟无法确认则不计算CPU。

PID改变或进程退出会结束窗口；`completed`只表示采样时长完成，`measurements_complete`另表明所需样本是否齐全。退出0要求两者通过；缺测/中途退出为1，参数、ADB或输出错误为2。首个CPU样本没有差分基线，不伪造0。短于一个采样间隔的窗口无法给出完整CPU差分。

## 离线数值汇总

```powershell
python scripts/android/summarize-runtime-metrics.py --inputs `
  cache/android-validation/my-run/runtime.json `
  --output cache/android-validation/my-run/summary.json
```

支持显式传入多份采样JSON、旧版独立smaps诊断JSON或带 `pss_kib_samples` 的旧驱动结果；不搜索cache、不依赖日期或A/B标签。每个输入分别生成window，输入以序号和SHA256标识；不复制原文件路径、设备连接地址或任意附加字段。

min/median/p95/max按窗口计算，p95为最近秩；PSS按dumpsys/smaps来源分别汇总，不回填其他窗口的缺测，旧格式PSS数组中的零值和负值也不计入有效样本。在包含RSS的运行窗口中，无正RSS的退出样本会排除并计入 `excluded_rss_samples`；无RSS的独立PSS诊断仍单列保留。汇总成功不代表推流或完整验收通过。

## 离线流证据门禁

```powershell
python scripts/android/validate-stream-evidence.py --inputs `
  cache/android-validation/my-run/camera-result.json `
  --output cache/android-validation/my-run/stream-validation.json
```

输入沿用现有驱动结果：mode、seconds_requested、events、receivers、instrument_exit及四项清理事实（进程退出、桌面、helper移除、自有进程结束）。只支持selftest/file/camera/restart/retry；不运行原临时控制器。

门禁独立核对结果事件、事件顺序、实际帧与进度、监听状态、解码错误、接收端退出码和时长；camera至少300秒。restart须有有序停止/重启事件，retry第一接收器须因受控断流非零退出、最后接收器仍须exit0并收到帧。输入中的 `passed=true` 不构成通过依据，`full_acceptance_passed`始终不由局部流证据推成true。退出0为所传功能证据全部通过，1为证据拒绝，2为输入/输出错误。

## APK静态与构建绑定校验

```powershell
python scripts/android/verify-apk-native.py --apk candidate.apk --aar ffmpeg-kit.aar `
  --platform-libraries platform-libraries.json `
  --output cache/android-validation/my-run/apk-static.json
```

platform-libraries.json由调用者从对应NDK目标API目录导出，格式为ABI到系统SO文件名数组，必须包含armeabi-v7a和arm64-v8a。不能拿Windows库或另一API的猜测名单替代。工具只验证DT_NEEDED名称闭包，不验证全部符号版本或最低API运行兼容。

工具拒绝损坏/重复ZIP条目、错误ELF类型/ABI、缺失依赖、TEXTREL、疑似桩核心、APK内置face_models和缺失必需DEX描述符；AAR原生库通过.text摘要对应APK。大小与JNI字符串的桩检查以及DEX描述符均为静态启发式，不证明运行时可用或来源可信。

默认 `signature_checked=false`、`ncnn_build_binding_verified=false`、`runtime_verified=false`。需要签名验证时显式追加 `--java` 和 `--apksigner-jar`，通过Java执行SDK的apksigner.jar，不拼接cmd/bat字符串，也不输出证书主体或原始诊断。验签成功仅保留证书SHA256。

需要构建绑定时传 `--build-binding` JSON：顶层分别为两ABI，每项包含 `core_library`、`cmake_cache`、`build_ninja` 三个本机构建输入路径。核对核心.text/ABI、RK_SKIP_OPENCV=OFF、RK_ENABLE_NCNN=ON及实际native-lib编译定义；绑定通过也不把 `runtime_verified` 改为true，不代表Gradle全量检查或识别精度通过。

## 设备状态只读核对

操作者须知道实际桌面和helper包名，不采用名字包含launcher的模糊匹配：

```powershell
$adb = Read-Host 'ADB可执行文件完整路径'
$serial = Read-Host '目标ADB serial'
$launcherPackage = Read-Host '实际桌面包名'
$helper = Read-Host '需确认已卸载的helper包名'
$sha = Read-Host '候选APK的SHA256'
python scripts/android/verify-device-state.py --adb $adb --serial $serial `
  --package com.example.rk3288_opencv --launcher-package $launcherPackage --helper-package $helper `
  --expected-sha256 $sha --output cache/android-validation/my-run/device-state.json
```

校验已装单个base.apk身份、目标进程退出、明确桌面包恢复和helper不存在；split APK或无法确认的返回值拒绝通过。设备缺sha256sum时，仅把已装代码包读入主机内存做摘要，无固定长度限制、不新增APK副本。

可另传 `--temporary-input` 指定本次专用cache文件的安全basename，仅通过run-as检查是否存在，不列目录、不读取内容或删除。未请求时报告 `temporary_input_checked=false`，不冒充已清理该输入。结果只保证本次读到的状态，不保证随后不发生改变；ADB查询失败不能被当作“应用已退出”。

## 验证与维护

使用合成输入及本地ADB/Node替身：

```powershell
python -m pytest tests/scripts/test_docs_check_driver.py tests/scripts/test_android_runtime_metrics.py tests/scripts/test_stream_evidence.py tests/scripts/test_apk_native_inspection.py tests/scripts/test_device_state_verification.py -q
```

沙箱默认临时目录不可写时，追加一个新建且位于仓库内的 `--basetemp`；不要指定历史证据或拟清理目录。任务4的FFmpegKit重建链、缓存删除与正式隐藏预览不属于这些工具的迁入内容。
