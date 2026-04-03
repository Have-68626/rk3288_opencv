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
