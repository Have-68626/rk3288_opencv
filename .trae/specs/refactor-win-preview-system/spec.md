# Windows 视频流预览系统重构与修复 Spec（Win32+C++）

## Why
当前 Windows 端预览系统需要在窗口缩放、分辨率/FPS 频繁切换、多脸检测、长时间运行与第三方订阅输出等场景下保持稳定、无闪屏、无阻塞且可量化。现有实现缺少完整的自适应布局与“保持宽高比不变形”的渲染策略，分辨率/FPS 切换缺少统一的验证与重启口径，识别结果对外输出缺少标准化端口，并需要引入更轻量的检测模型与性能降级机制以保证实时性。

## What Changes
- 重构 Win32 UI 布局：引入响应式栅格/布局管理器，窗口缩放时控件按比例自适应；预览区强制保持原始宽高比，默认采用“居中裁剪填充”避免黑边，并提供“信箱模式（letterbox）”可选。
- 控制面板新增：
  - 分辨率下拉：1920×1080、1280×720、640×480
  - 帧率选择：15/30/60 fps
  - 切换时执行：能力验证 → 实时重启采集 → 重新初始化检测模型 → 自动回滚（失败时）
- 识别结果对外输出端口（两种同时提供）：
  - 本机 HTTP 服务：`GET http://localhost:8080/api/faces` 返回最新人脸数组；提供 `GET /api/faces/stream`（SSE）供第三方订阅（可选但推荐）
  - 外部推送：将 JSON 结果以 HTTP POST 推送到配置的 `post_url`（默认 `http://localhost:8080/api/faces`）
- 集成轻量化人脸检测模型：
  - 优先：OpenCV DNN + SSD MobileNet（模型文件由配置指定，不硬编码路径）
  - 运行时每帧推理（以“最新帧优先”的背压策略保证不堆积），输出 bbox/confidence/timestamp
  - 将检测框坐标按当前显示缩放比例映射后绘制：半透明白色描边矩形（线宽 2px、圆角 5px），左上角标注置信度百分比
- 性能监控与自动降级：
  - 若检测延迟 > 100ms 或丢帧率 > 5%，自动降级为“跳过非关键帧”（动态调整检测 stride），并记录事件
- 测试与交付：
  - 提供自动化/半自动化测试模式与日志，覆盖窗口缩放、切换稳定性、1 小时稳定运行输出一致性
  - 更新用户手册与开发文档，明确测试步骤与指标口径

## Impact
- Affected specs: Windows 预览 UI/渲染、采集重启与能力验证、检测模型与性能降级、HTTP 输出端口、稳定性测试
- Affected code（预期）:
  - `src/win/app/win_camera_face_recognition_main.cpp`
  - `src/win/src/MfCamera.cpp`（能力枚举/切换验证）
  - `src/win/src/FramePipeline.cpp`（背压、重启、降级、结果发布）
  - 新增 `src/win/src/HttpFacesServer.*`（本地 GET/SSE）
  - 新增 `src/win/src/HttpFacesPoster.*`（外部 POST 推送）
  - `src/win/src/FaceDetector.*`（新增 DNN 实现或策略选择）
  - `config/windows_camera_face_recognition.ini`
  - `docs/windows-camera-face-recognition/*`
  - `CMakeLists.txt`（Windows 目标增加网络/线程所需链接；按需启用 opencv_dnn）

## ADDED Requirements
### Requirement: 响应式布局与宽高比保护
系统 SHALL 在窗口缩放时保持 UI 布局不重叠、不溢出；预览区 SHALL 保持源画面宽高比，禁止拉伸变形。

#### Scenario: 任意窗口尺寸
- **WHEN** 用户将窗口从 800×600 拖动到 4K
- **THEN** 控件布局稳定，无错位或遮挡
- **AND** 预览画面不变形（可裁剪填充或 letterbox，但不得拉伸）

### Requirement: 分辨率/FPS 动态切换（实时生效）
系统 SHALL 提供分辨率与 FPS 的运行时切换，并在变更后实时重启采集并重新初始化检测模型。

#### Scenario: 切换成功
- **WHEN** 用户选择 1920×1080/1280×720/640×480 与 15/30/60 fps 的组合并确认
- **THEN** 系统在 300ms 内完成采集重启并恢复稳定渲染
- **AND** 检测模型重新初始化完成并继续输出结果

#### Scenario: 切换失败回滚
- **WHEN** 所选分辨率/FPS 组合在当前设备上不可用
- **THEN** 系统展示明确失败原因
- **AND** 自动回滚到上一次可用配置并继续预览

### Requirement: 结果输出端口（本地服务 + 外部推送）
系统 SHALL 对外提供当前帧检测到的人脸数组输出，字段至少包含：
- `bbox`（x,y,w,h，基于源帧坐标与显示坐标两套均可，但必须在文档中明确）
- `confidence`（0~1）
- `timestamp`（毫秒或 ISO8601）

#### Scenario: 本地查询与订阅
- **WHEN** 第三方请求 `GET /api/faces`
- **THEN** 返回最新 faces 数组（JSON）
- **AND** 若启用 SSE，`GET /api/faces/stream` SHALL 推送连续结果

#### Scenario: 外部 POST 推送
- **WHEN** 配置启用 `post_url`
- **THEN** 每帧或按节流策略将 faces JSON POST 到该 URL
- **AND** 失败时按退避重试，并记录错误事件但不阻塞预览

### Requirement: 轻量化检测模型与叠加绘制
系统 SHALL 集成轻量检测模型并在每帧执行推理（含自动降级），并在渲染叠加层绘制检测框与置信度。

#### Scenario: 多人脸
- **WHEN** 同一帧包含多张人脸
- **THEN** 输出数组包含每张人脸的 bbox/confidence
- **AND** 叠加绘制为半透明白色描边（线宽 2、圆角 5）并标注置信度百分比

### Requirement: 性能监控与自动降级
系统 SHALL 监测检测延迟与丢帧率，并在超阈值时自动降级（跳过非关键帧）以保证交互稳定。

#### Scenario: 延迟超标
- **WHEN** 单帧检测延迟 > 100ms 或丢帧率 > 5%
- **THEN** 自动提高 stride/降频检测直到回到阈值以内
- **AND** 记录降级事件与恢复事件

## MODIFIED Requirements
### Requirement: 配置与可复现
所有路径与端口 SHALL 可通过 ini/环境变量配置，禁止硬编码绝对路径；并在文档中提供复现命令与测试步骤。

## REMOVED Requirements
### Requirement: 无
**Reason**: 以重构与增强为主，不移除既有能力。
**Migration**: 保持原有默认行为可运行；新能力默认关闭或提供合理默认值。

