# RK3288 + UVC（Camera2/CameraX）验收 Runbook（30 分钟稳定性 / Mock 回归 / SAFE MODE）

本 Runbook 用于验收“Android 官方相机栈采集（Camera2/CameraX）+ 外部帧输入 + 自动/手动切换 + 热重启 + 权限 SAFE MODE”是否满足交付口径，并提供最短排障路径与可复现的日志采集方式。

## 1. 适用范围

- 目标设备：RK3288 工控机（Android 7.x 为主）
- 摄像头：UVC USB 摄像头（外接）
- 应用形态：Android APK（Debug/Release 均可；Debug 更方便导出日志）
- 采集方案：
  - 自动模式：默认优先 Camera2，异常时自动降级到 CameraX（或反向切换）
  - 手动模式：关闭自动后可固定选择 Camera2 或 CameraX
  - 热重启：运行中 stop → 重配 → start（无需重启 App）

## 2. 验收前准备（一次性）

### 2.1 设备与线缆检查

1) 设备开机后，用 USB 线连接到 Windows 电脑，确保已开启“开发者选项 → USB 调试”。  
2) 将 UVC 摄像头插入 RK3288 的 USB Host 口（尽量避免 USB HUB；如必须使用 HUB，优先有独立供电的 HUB）。  
3) 确保同一时间没有其他 App 正在占用相机（尤其是系统相机、预装摄像头测试工具）。

### 2.2 安装/重装 APK（建议每轮验收前做）

在仓库根目录打开 PowerShell，执行：

```powershell
.\gradlew.bat --no-daemon :app:assembleDebug
adb install -r .\app\build\outputs\apk\debug\app-debug.apk
```

如果需要“彻底重置权限/配置/缓存”，执行：

```powershell
adb shell pm clear com.example.rk3288_opencv
```

## 3. 日志采集（强制要求）

验收结论必须能被日志复核；若出现问题，需把日志与复现步骤一并归档到仓库的 `ErrorLog/`（日志粘贴/上传前先打码敏感信息）。

### 3.1 采集 logcat（推荐）

1) 清空历史 logcat：

```powershell
adb logcat -c
```

2) 开始抓取 logcat 到本机文件（窗口保持运行）：

```powershell
adb logcat -v time > .\ErrorLog\logcat_rk3288_uvc_acceptance.txt
```

停止抓取：在该 PowerShell 窗口按 `Ctrl+C`。

### 3.2 导出应用“会话日志文件”（强烈推荐）

应用会把 Java（AppLog）与 Native（NativeLog）写入同一会话文件，文件名形如：`rk3288_yyyyMMdd_HHmmss.log`。

外部落盘目录（优先）：

```powershell
adb shell ls /sdcard/Android/data/com.example.rk3288_opencv/logs
adb pull /sdcard/Android/data/com.example.rk3288_opencv/logs .\ErrorLog\
```

如果设备上外部目录不可用，再尝试内部目录（Debug 通常可用 `run-as`）：

```powershell
adb shell run-as com.example.rk3288_opencv ls files/logs
adb shell run-as com.example.rk3288_opencv cat files/logs/<会话文件名> > .\ErrorLog\rk3288_session.log
```

### 3.3 快速检索关键字（Windows）

```powershell
Select-String -Path .\ErrorLog\*.log -Pattern "SYSTEM READY","SYSTEM NOT READY","首帧推入","采集异常","自动降级","SAFE MODE","VideoManager","Engine"
```

## 4. Task 9 验收场景 A：RK3288 + UVC 30 分钟稳定性

### 4.1 目标

- 稳定打开 UVC 摄像头
- 持续出帧、持续推入 Native
- 连续运行 30 分钟无崩溃、无卡死

### 4.2 步骤（可执行）

1) 重置状态（推荐）：
   - 执行 `adb shell pm clear com.example.rk3288_opencv`
   - 重新打开 App
2) 首次弹出权限申请时，选择“允许”（相机权限）。
3) 在主界面相机下拉框中，选择你的 UVC 摄像头（不要选 Mock）。
4) 保持“采集方案：自动”开关为开启（默认）。
5) 点击 `START MONITORING`，进入监控运行状态。
6) 观察 10 秒内必须出现的现象：
   - 状态文本包含 `Running (Camera2 / Cam <id>)` 或 `Running (CameraX / Cam <id>)`
   - 屏幕顶部/覆盖层出现“就绪”提示（系统就绪态）
7) 让 App 前台连续运行 30 分钟（不要切后台；不要锁屏）。
8) 30 分钟后点击 `STOP MONITORING` 停止监控。
9) 导出日志（见第 3 节）。

### 4.3 通过标准（必须同时满足）

- 运行 30 分钟无崩溃、无 ANR、无黑屏卡死
- 日志至少包含以下关键字：
  - `SYSTEM READY`
  - `首帧推入 ok`
- 日志不应反复出现以下错误关键字（允许偶发 1 次但必须能自恢复且不影响 30 分钟连续运行）：
  - `采集异常:`
  - `SYSTEM NOT READY (Error)`
  - `captureError`
  - `ERROR_CAMERA_SERVICE`（系统侧关键词，若出现需排障）

### 4.4 期望日志关键字（用于复核）

- Java/AppLog：
  - `MainActivity` + `首帧推入 ok tsNs=`
  - `MainActivity` + `SYSTEM READY`
  - 若触发自动切换：`自动降级(` + `restartMonitoring`
- Native/NativeLog：
  - `Engine` + `外部帧输入通道 已启用`
  - `Engine` + `cameraId<0：跳过 VideoManager 相机打开（预期用于外部帧输入）`

## 5. Task 9 验收场景 B：Mock 回归（文件 / 系统相机拍照）

### 5.1 目标

- Mock 功能不回退：仍可离线验证推理与 UI/日志链路
- 两条 Mock 入口均可用：
  - 文件选择（Mock Source）
  - 系统相机拍照回传（Mock Camera）

### 5.2 Mock（文件选择）步骤

1) 在相机下拉框选择 `Mock Source (File Picker)`。
2) 选择一个文件：
   - 推荐：`jpg/png` 静态图（最稳定、最适合作为离线回归）
   - 也可：`mp4` 或其他视频（取决于设备解码与 OpenCV VideoIO 支持情况）
3) 看到 Toast：`Mock Source Selected: <文件名>`。
4) 点击 `START MONITORING`。
5) 导出日志。

通过标准：
- UI 显示 `Status: Engine Initialized (Mock Mode)`
- 日志包含：
  - `Mock file ready:`
  - `引擎初始化成功 (Mock)` 或等价信息
  - `SYSTEM READY`（如果 Mock 能出帧）

### 5.3 Mock（系统相机拍照回传）步骤

1) 在相机下拉框选择 `Mock Camera (System App)`。
2) 系统相机会打开，拍一张照片并确认。
3) 回到 App 后应出现 Toast：`Photo Captured`。
4) 点击 `START MONITORING`。
5) 导出日志。

通过标准：
- 日志包含：
  - `handleSystemCameraResult` + `Photo captured:`
  - `Status: Engine Initialized (Mock Mode)`

## 6. Task 9 验收场景 C：权限拒绝 / 永久拒绝 → SAFE MODE（必须稳定）

### 6.1 目标

- 用户拒绝相机权限时，稳定进入 SAFE MODE
- SAFE MODE 下不得触发 Native 相机初始化（不应进入“外部帧输入采集链路”）

### 6.2 步骤（可执行）

1) 清数据（确保重新弹权限框）：

```powershell
adb shell pm clear com.example.rk3288_opencv
```

2) 打开 App，系统弹出相机权限申请时选择：
   - 场景 1：点击“拒绝”
   - 场景 2：点击“拒绝并不再询问”（若系统有该选项）
3) 回到主界面后，确认状态文本出现：
   - `Status: SAFE MODE (Permissions Missing)` 或 `Status: SAFE MODE (Missing Permissions)`
4) 尝试点击 `START MONITORING`：
   - 预期：会提示“权限不足：已进入安全模式”，并引导申请权限或去设置页
5) 导出日志。

### 6.3 通过标准（必须同时满足）

- 日志包含：
  - `SAFE MODE`
  - `权限不足，禁止初始化引擎` 或 `安全模式:` 相关提示
- 日志不应包含以下 Native 初始化/采集关键字（任意出现均视为失败）：
  - `Engine` + `initialize`
  - `Engine` + `外部帧输入通道 已启用`
  - `VideoManager::open`

## 7. 最短排障路径（错误含义 → 最可能原因 → 最短修复 → 如何验证）

### 7.1 “能看到摄像头，但开始监控后黑屏 / 没有 SYSTEM READY”

- 含义：采集链路没出首帧或帧推入失败，watchdog 可能会触发 `首帧超时/出帧停滞`。
- 最可能原因：
  1) UVC 摄像头被其他 App 占用
  2) 摄像头供电不足（尤其接 HUB）
  3) Camera2 在该机型/HAL 下输出 YUV_420_888 异常
- 最短修复：
  1) 关闭所有相机相关 App，重新插拔 UVC
  2) 在 App 内保持“采集方案：自动”，让其自动切换到另一方案
  3) 仍失败：重启设备（最短恢复 CameraService 的方式）
- 如何验证：
  - 日志中应出现 `首帧推入 ok` 与 `SYSTEM READY`
  - 若自动切换发生，日志应出现 `自动降级(`，随后再次出现 `首帧推入 ok`

### 7.2 “不断出现 自动降级(...) / 频繁热重启”

- 含义：watchdog 反复检测到首帧超时/出帧停滞/推帧失败过多，触发自动切换与热重启。
- 最可能原因：
  1) 摄像头本身输出不稳定（线材/供电/USB 接触）
  2) 分辨率协商不稳定（当前 Camera2 固定优先选 640×480；CameraX 由系统选取）
  3) CPU/内存压力导致回调延迟（但通常会先表现为 FPS/延迟异常）
- 最短修复：
  1) 更换 USB 线与接口，避免 HUB
  2) 手动关闭“采集方案：自动”，固定使用更稳定的一条（Camera2 或 CameraX）
  3) 记录出现问题的 cameraId 与 UVC 型号，归档到 `ErrorLog/`（便于后续做设备白名单/黑名单）
- 如何验证：
  - 连续运行 5 分钟内不再出现 `采集异常:` 与 `自动降级(`

### 7.3 “拒绝权限后仍然触发了 Native 侧初始化/相机调用”

- 含义：SAFE MODE 门控失效（属于 P0 缺陷）。
- 最可能原因：权限状态机回调与 UI 状态不同步，导致启动流程绕过 gate。
- 最短修复：
  1) 先按第 6 节复现并导出日志
  2) 检索是否出现 `Engine initialize` / `外部帧输入通道 已启用`（不应出现）
  3) 若出现，将该日志与复现录像一并放入 `ErrorLog/`，用于回归修复
- 如何验证：
  - 清数据后重复拒绝权限，日志中不再出现上述 Native 初始化关键字

## 8. 已知限制（当前实现口径）

1) Camera2 分辨率选择策略优先 640×480；未提供 UI 配置入口。  
2) CameraX 绑定是异步行为：`start()` 会先返回，若后续绑定失败会走 `captureError` 与 watchdog 降级。  
3) 自动恢复策略：同一采集方案先做最多 2 次重试（退避 0.8s/1.6s，重建会话）；仍失败再做一次跨方案自动降级切换（避免无限抖动）；若两条路径都不稳定，最终会停止监控并提示失败。  
4) 30 分钟稳定性验收默认要求前台运行；若切后台/锁屏，系统可能强制释放相机，需按“恢复策略”重新启动监控。  
