# RK3288 机器视觉引擎 (AI Engine)

**更新日期**: 2026-07-06

![Platform](https://img.shields.io/badge/Platform-RK3288%20%7C%20ARMv7%20%7C%20x86_64-blue)
![Language](https://img.shields.io/badge/Language-C%2B%2B17-green)
![OpenCV](https://img.shields.io/badge/OpenCV-4.10.0-orange)
![Tests](https://img.shields.io/badge/Tests-GTest%20%7C%20Vitest-green)

## 项目简介与运行模式

跨平台机器视觉应用，支持在资源受限设备（RK3288, Cortex-A17, 2GB RAM）上进行稳定的视频监控与边缘生物识别：

- **Android App (Gradle)**: `app/` + `src/java/` — 摄像头预览、人脸识别 UI、JNI 引擎绑定
- **C++ 核心引擎 (CMake)**: `src/cpp/` — 人脸检测/识别/追踪管线、帧处理、性能监控
- **Windows 本地服务 (CMake/Windows)**: `src/win/` — HTTP REST API + SSE/MJPEG 流、配置管理、摄像头管理
- **Web SPA (Vite/React)**: `web/` — 浏览器端配置面板与预览

> 三层重构（PR #427）：`Engine`、`FramePipeline`、`HttpFacesServer` 已拆分为纯函数管线 + 副作用隔离的架构。

## 项目结构

```
rk3288_opencv/
├── app/                          # Android 应用（UI + 业务）
├── src/
│   ├── cpp/
│   │   ├── include/              # 头文件
│   │   ├── src/                  # 核心引擎 + pipeline/
│   │   ├── jni/                  # JNI 函数（按领域拆分）
│   │   └── tools/                # CLI 工具
│   ├── win/                      # Windows 本地服务
│   └── java/                     # Android Java 源码
├── web/                          # Web SPA 前端
├── cmake/                        # 模块化 CMake（5 模块）
├── docs/
│   ├── architecture/             # 架构契约（ARCHITECTURE.md）
│   └── superpowers/              # 设计文档 + 实施计划
├── tests/                        # 单元测试 + 集成测试
└── scripts/                      # 构建/测试/验证脚本
```

## 架构契约

项目遵循 5 条跨子系统架构契约（详见 [ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md)）：

| 契约 | 规则 |
|------|------|
| #1 RAII 资源封装 | 所有 lock/unlock 必须 RAII 封装 |
| #2 纯函数管线 | 业务逻辑为纯函数，副作用在边界集中处理 |
| #3 原子状态提交 | 共享状态使用 transactional copy 模式 |
| #4 错误通道 | 帧路径禁止 throw，使用 `std::optional` |
| #5 接口隔离 | 公共 API 必须是纯抽象接口 |

## 依赖

**必需**：OpenCV 4.10.0（源码构建）、Android NDK 27.0、CMake 3.22.1+、Gradle 9.0-milestone-1、Node.js 22.x、pnpm

**可选**：ncnn（推理加速）、RK MPP（硬解码）、Qualcomm SNPE、libyuv

> 完整依赖清单与模型台账见 [CREDITS.md](CREDITS.md)。

## 快速开始

### C++ 单元测试（无 OpenCV）

```powershell
cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target core_unit_tests
ctest --test-dir build_ci --output-on-failure
```

### Windows 全量构建（需 OpenCV 源码）

```powershell
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 `
  -DOPENCV_ROOT="path\to\opencv" -DOPENCV_CONTRIB_ROOT="path\to\opencv_contrib"
cmake --build build_win --config Release --target win_local_service
ctest --test-dir build_win -C Release
```

### Android

```powershell
gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

### Web SPA

```powershell
pnpm -C web install
pnpm -C web lint
pnpm -C web test        # Vitest 单元测试
pnpm -C web build
pnpm -C web e2e:run     # Cypress E2E
```

## 测试框架

| 测试套件 | 框架 | 位置 | 依赖 |
|----------|------|------|------|
| core_unit_tests | GTest | `tests/cpp/` | 无（使用 OpenCV mock stubs） |
| face_infer_unit_tests | GTest | `tests/cpp/` | 需 OpenCV |
| win_unit_tests | GTest | `tests/win/` | 需 OpenCV |
| core_gtest_tests | GTest | `tests/cpp/` | 需 OpenCV |
| ncnn_precision_test | GTest | `tests/cpp/` | 需 ncnn + OpenCV |
| Web 单元测试 | Vitest | `web/src/` | 无 |
| Web E2E | Cypress | `web/cypress/` | 需后端运行 |

## CI 流水线

`.github/workflows/ci.yml` — 6 个 job：

| Job | 触发 | 内容 |
|:----|:----:|:-----|
| repo-hygiene | PR/push → master | 仓库垃圾文件扫描 |
| unit-tests | PR/push → master | CMake + Ninja, `RK_SKIP_OPENCV=ON` |
| docs-audit | PR/push → master | 文档同步状态审计 |
| web | PR/push → master | `pnpm install` → `lint` → `build` → `e2e:run:coverage` |
| android | PR/push → master | `assembleDebug` + `testDebugUnitTest` + `lintDebug` |
| windows | push only | 完整 OpenCV 源码构建 + ctest + INT8 精度测试 |

## 治理进度

| 批次 | 模块 | 状态 |
|:----:|:-----|:----:|
| Batch 1 | 法律合规 + 构建基础设施 | ✅ 完成 |
| Batch 2 | C++ 核心引擎 | ✅ 完成 |
| Batch 3 | Windows 本地服务 | ✅ 完成 |
| Batch 4 | Web SPA + Android 治理 | ✅ 完成 |
| Batch 4.7 | MainActivity 拆分 | ✅ 完成 |
| Batch 5 | 测试覆盖治理（GTest 迁移） | ✅ 完成 |
| 架构治理 | 架构契约 + CMake 模块化 + JNI 拆分 | ✅ 完成 |
| 持续 | 文档合规 | 🟡 持续 |

## 脚本工具

`scripts/` 目录：

| 脚本 | 用途 |
| :--- | :--- |
| `clean-repo-junk.js` | 仓库垃圾文件扫描 |
| `docs-sync-audit.js` | 文档同步状态审计 |
| `quantize_ncnn_int8.py` | INT8 模型量化（ncnn2table → ncnn2int8） |
| `check-raii-violations.sh` | 架构契约 #1 RAII 合规检查 |
| `run-web-e2e.ps1` | Web E2E 测试 + 覆盖率 |
| `bench_camera_adb.ps1` / `.sh` | Android 摄像头基准测试 |
| `stability_switch_50_adb.ps1` | Android 前后台 50 次稳定性验收 |

## 许可证

MIT License

### 密码学声明

本项目使用 AES-256-GCM（通过 Android KeyStore / Windows BCrypt）进行人脸特征模板加密。密码学功能仅用于本地数据保护，不涉及通信加密或数字签名。

## 审计与设计文档

- [全项目审计报告](AUDIT_REPORT.md) — 综合评分 2.9/5
- [架构契约](docs/architecture/ARCHITECTURE.md) — 5 条跨子系统规则
- [架构治理设计](docs/superpowers/specs/2026-07-06-architecture-governance-design.md)
- [架构治理实施计划](docs/superpowers/plans/2026-07-06-architecture-governance-plan.md)
- [远程分支审计](REMOTE_BRANCH_AUDIT.md) — 19→3 分支清理
