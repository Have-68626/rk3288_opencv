# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Documentation**: Comprehensive refactor of `DEVELOP.md` with standardized Markdown, bilingual terminology, and engineering improvements.
- **Examples**: Added `docs/examples/` directory containing 6 compilable C++ examples for RK3288 platform:
    - `01_v4l2_capture.cpp`: Direct V4L2 camera capture with MMAP.
    - `02_rkmpp_decode.cpp`: Hardware video decoding using Rockchip MPP.
    - `03_rknn_inference.cpp`: NPU inference using RKNN API.
    - `04_drm_kms_display.cpp`: Direct rendering using DRM/KMS with double buffering.
    - `05_opencv_rknn_bridge.cpp`: OpenCV preprocessing for RKNN.
    - `06_integration_demo.cpp`: Integration demo combining V4L2, RKNN, and display logic.
    - `07_camerax_jni_bridge.java`: Android CameraX with JNI bridge for zero-copy preview.
    - `07_jni_yuv_processor.cpp`: Native C++ processing of CameraX YUV streams.
- **Architecture**: Added Hybrid Architecture (CameraX + Native) section to `DEVELOP.md`.
- **Checklists**: Added `docs/checklist/acceptance.md` for development, testing, and release phases.
- **Quick Start**: Added quick start script guidelines and dependency matrix in `DEVELOP.md`.

### Removed
- **Legacy SDK**: Removed `ColorOsSdkBridge`, `PlayIntegrityChecker`, `GmsDetector`, `PrivilegedCommandGate`, and `DevicePolicy` to eliminate invalid dependencies.
- **Security**: Removed `SecurityEventLogger` as part of the SDK cleanup.

### Changed
- **Device Profiling**: Simplified `DeviceProfile` to focus solely on hardware data collection (Build info, Memory/Storage).
- **Documentation**: Added Section 4.6 to `DEVELOP.md` detailing the simplified Device Profiling task (Pending Implementation).
- **Structure**: Reorganized `DEVELOP.md` into "Overview -> Environment -> Core Development -> Advanced/Troubleshooting".
- **Content**: Expanded technical details on V4L2, MPP, RKNN, and DRM/KMS.
- **Style**: Enforced strict Markdown standards and bilingual terminology.
- **日志目录**: 错误日志目录统一为 `ErrorLog/`（区分大小写），不再兼容 `errorlog/`。

### Documented
- **Roadmap**: Migrated completed items from `README.md` Todo List into `CHANGELOG.md` for traceability.
- **README 待办（完成项 1–15）**：从 `README.md` 清理迁移至此处归档（README 仅保留未完成待办 16+）。
  - 1. **[P0] 非 RK3288 设备：高分辨率/高帧率输入的稳定性与兼容性治理**（输入端强制 ≤1080p@60fps；超规格自动降档/拒绝并提示）
  - 2. **[P0] 外部帧输入性能优化（JNI 拷贝与 YUV 转换）**（降低拷贝与转换开销；推理前统一缩放到工作分辨率）
  - 3. **[P0] Mock（文件/URL）模式：大文件/高分辨率的 ANR 与卡顿治理**（初始化后台化+可取消；超规格降档/拒绝）
  - 4. **[P0] 识别事件面板无输出：打通 Native 事件→UI 列表的闭环**（补齐回调链路并做节流）
  - 5. **[P0] 屏幕旋转后预览未回正：旋转元数据与画面布局的稳定性修复**（rotationDegrees 口径统一+配置变更后重绑）
  - 6. **[P0] 人脸识别链路“检测驱动识别”**（仅对检测到的 ROI 识别；预留策略开关）
  - 7. **[P0] 预览叠加人脸框**（渲染帧绘制人脸框，主脸高亮）
  - 8. **[P0] 预览输出上限与低开销渲染**（Surface 渲染替代 Bitmap 高频刷新；输入端仍强制 ≤1080p60）
  - 9. **[P1] 输入源“预检 + 自动降档”策略（相机/文件统一）**（开始监控前预检并记录结果）
  - 10. **[P1] App 内日志查看能力增强**（突破 100KB 限制；支持抓取/展示 logcat）
  - 11. **[P1] 稳定性回归与问题收敛（ErrorLog 证据链）**（一键导出日志+logcat+设备信息+预检结果）
  - 12. **[P1] 悬浮窗开关状态与实际悬浮窗不同步**（以服务真实运行状态回填 UI，支持回前台自动校准）
  - 13. **[P1] 新增“退出”按钮**（停止监控/释放资源/停止悬浮窗并移除任务栈）
  - 14. **[P1] 日志详情页筛选**（支持正则与快捷筛选并高亮）
  - 15. **[P1] 日志删除与保留策略可配置**（手动删除 + 自动清理保留 1/7/14/30 天）
- **DEVELOP.md 修订记录**: 将 `DEVELOP.md` 末尾“修订记录”从开发设计书中移除，并迁移到此处归档，避免文档重复与漂移。

| 日期 | 版本 | 修订来源 | 变更摘要 |
| :--- | :--- | :--- | :--- |
| 2026-03-03 | 2.0.0-rc1 | 项目初始化 | 建立基础开发指南、目录映射与核心模块说明 |
| 2026-03-22 | 2.0.0-rc2 | document-camera-face-research spec | 新增两大研究章节、图表索引、基准与脚本模板入口、风险清单与参考文献；调整章节编号以保持连续 |
| 2026-03-22 | 2.0.0-rc3 | 文档补齐 | 补充第三方库对比、Android 13+ 权限与后台限制、相机能力报告/拍照录像时序与格式、CameraService 重启检测；补齐人脸方案集成清单/PAD 指标/CI 门禁，并扩展参考文献与图表索引 |
| 2026-03-22 | 2.0.0-rc4 | 文档补齐 | 在第 5/6 章补充 Fotoapparat/CAMKit 对比；补齐 ML Kit/MediaPipe/ArcFace/Dlib/百度/优图 及 阿里/AWS/Azure 表格与集成模板；更新图表索引与参考文献编号 |
| 2026-03-22 | 2.0.0-rc5 | 文档补齐 | 5.3 增加广角/长焦/TOF/红外枚举方法与能力查询（闪光/对焦/曝光补偿等）；6.x 增加特征维度/模板大小/阈值/延迟对比表与逐方案集成模板；修正 ArcFace 表述并扩展参考文献与表索引 |
| 2026-03-22 | 2.0.0-rc6 | 文档补齐 | 6.5.1 补齐 AES-256-GCM+Keystore 关键代码模板与异常处理；6.6 补齐 Gradle7.5+NDK25 demo 构建脚本、JUnit+Robolectric 与 Espresso+MockCamera 模板、GitHub Actions 门禁（识别率≥97% 与 泄漏≤5MB）；更新表索引与参考文献 |
| 2026-03-22 | 2.0.0-rc7 | 文档补齐 | 6.5.1 增补 Java 版 AES/GCM/Keystore 示例（含 KeyStore/KeyGenerator/GCMParameterSpec）；6.6.3 增补 android-emulator-runner 模板以跑 connectedAndroidTest；更新参考文献与示例 build_id 口径 |
| 2026-03-23 | 2.0.0-rc8 | audit-camera-pipeline spec | 相机链路审计与最小修复：权限最小化、退后台释放与回前台恢复、拉帧背压与主线程卸载、Native 引擎线程 join；补齐审计报告与回归测试计划入口 |
| 2026-04-07 | 2.0.0-rc8 | fix-android-camera2-uvc-open-failure spec | 补充 Camera2/CameraX 采集方案复现/验收入口（Runbook 链接、自动/手动切换、热重启与已知限制）。 |
- **Android Compatibility**: Support Android API 21–34; declare camera/USB Host features as optional (`required=false`); SAFE MODE when key runtime permissions are missing; handle Android 13+ media permissions and legacy scoped-storage differences.
- **Permissions**: Unified permission rationale flow; handle "Don't ask again" by guiding users to system settings; gate monitoring start and engine init when permissions are missing; separate overlay permission from runtime permissions.
- **Logging & Export**: Session log naming (`rk3288_yyyyMMdd_HHmmss.log`); Java+Native co-write; dual-path persistence (internal + external when available); retention cleanup (age/count); size-based rolling; log viewer and ZIP export via SAF (CRC32 + `manifest.txt`); sensitive data masking (`SensitiveDataUtil`).
- **Camera Discovery**: Enumerate cameras via `CameraManager.getCameraIdList()`; UI switching with `SharedPreferences` persistence; USB hot-plug refresh; mock sources (system camera return + file picker).
- **Overlay Metrics**: `StatusService` overlay for FPS/CPU/MEM (500ms refresh), requiring overlay permission only when enabled.
- **Debug Tooling**: Maintain Native CLI entry (`main.cpp` / `rk3288_cli`) to run with `cameraId` or file input plus optional `cascadePath`/`storagePath`.
- **Android Capture (Camera2/CameraX)**: Use Android camera stack to produce `YUV_420_888` frames and push into Native via JNI with rotation/mirror normalization; add watchdog retry/backoff and auto downgrade path.
- **FFmpeg Mock & RTMP**: Support mock URL (MP4/HLS/RTSP); RTMP push entry; optional `ffmpeg-kit.aar` integration; loop playback (`-stream_loop -1`); dual ABI (`arm64-v8a` + `armeabi-v7a`) strategy.
- **UI Adaptation**: Portrait/landscape layouts; `onConfigurationChanged`-based rotation handling (no Activity restart) with transition animation; recognition events panel show/hide with FAB and persisted state.
- **Stability & Performance**: Reduce misleading "SYSTEM NOT READY" logs; improve media layout adaptability; enable NEON and reduce `Mat` copies where applicable; tolerate `/proc/stat` permission denial in CPU sampling.

## [1.2.0] - 2026-02-09

### Added
- **System**: `PermissionStateMachine` for robust permission handling on Android 13+.
- **Logging**: `AppLog`/`FileLogSink`/`NativeLog` with dual-path storage and automatic rollback.
- **Monitoring**: `StatsRepository` for real-time FPS/CPU/MEM monitoring via `StatusService`.

### Changed
- **UI**: Improved `LogViewerActivity` with export capability and sensitive data masking.
- **Camera**: Dynamic camera discovery and hot-plug support.

## [1.1.0] - 2026-02-09

### Added
- **Compatibility**: Optimized `AndroidManifest.xml` for non-industrial Android devices.
- **Debug**: Fixed `main.cpp` CLI build issues.

### Fixed
- **Build**: Added `<string>` header to `main.cpp` to fix compilation error.
