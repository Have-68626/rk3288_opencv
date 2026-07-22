# CLAUDE.md — 项目 AI 助手指南

<!-- DOCSYNC_AGENT_GUIDE_FACTS
gradle_wrapper=gradle-9.0
android_ci_java=21
windows_ci_events=pull_request,push
windows_ci_workflow_dispatch=false
test_frameworks=custom_bool,googletest
merge_strategy=squash_and_merge
-->
<!-- DOCSYNC_SHARED_GUIDE_START -->
## docs-sync-audit 关键事实

- `gradle_wrapper=gradle-9.0`
- `android_ci_java=21`
- `windows_ci_events=pull_request,push`
- `windows_ci_workflow_dispatch=false`
- `test_frameworks=custom_bool,googletest`
- `merge_strategy=squash_and_merge`
<!-- DOCSYNC_SHARED_GUIDE_END -->

## 项目简介
rk3288_opencv — 面向 Rockchip RK3288 平台的多模态人脸识别系统。

## 四个独立子系统
- Android App (Java/Gradle) — app/ + src/java/
- Windows 服务 (C++/CMake) — src/win/
- Web SPA (React/TypeScript) — web/
- C++ 核心引擎 — src/cpp/

## 构建命令
- core_unit_tests: cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON && cmake --build build_ci --target core_unit_tests
- Windows 全量: cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT=... -DOPENCV_CONTRIB_ROOT=...
- Web: pnpm -C web install && pnpm -C web build
- Android: gradlew.bat --no-daemon :app:assembleDebug

## 测试框架
同时使用自定义 `bool` 测试和 GoogleTest；新增测试必须沿用所在 target 的既有框架。
测试文件位于 `tests/cpp/` 和 `tests/win/`。
| Subsystem | Build | Entrypoint | Tests |
|-----------|-------|------------|-------|
| Android App (`app/` + `src/java/`) | `gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug` | `src/cpp/native-lib.cpp` (JNI), `MainActivity.java` | `tests/unit/java/` via Gradle |
| Windows Service (`src/win/`) | CMake + VS 2022 | `src/win/app/win_local_service_main.cpp` (service, default), `src/win/app/win_camera_face_recognition_main.cpp` (legacy UI, OFF by default) | `win_unit_tests`, `win_face_database_perf`; CLI tools: `win_face_eval_cli`, `win_face_bench_cli` |
| Web SPA (`web/`) | `pnpm -C web` (build/lint/dev) | `web/src/` (Vite + React + Ant Design) | `pnpm -C web e2e:run` (Cypress), `pnpm -C web e2e:run:coverage`; E2E needs `pnpm preview` or `scripts/run-web-e2e.ps1` for backend |
| Native C++ Core (`src/cpp/`) | CMake (cross-platform) | `Engine::initialize()` → `FaceInferencePipeline::process()` | `core_unit_tests` (no OpenCV), `face_infer_unit_tests` (needs OpenCV) |

## Essential build quirks

- **`OPENCV_ROOT`** must point to OpenCV **source** (not install), set via env or `-D` flag; CMake validates `CMakeLists.txt` inside. **`OPENCV_CONTRIB_ROOT`** is also required (provides `opencv_face` module for `LBPH` recognizer); set both for full builds.
- **`RK_SKIP_OPENCV=ON`** — build `core_unit_tests` without OpenCV (CI uses this). In Gradle pass as `-PRK_SKIP_OPENCV=true`. Uses mock stubs at `deps/opencv/` for linking.
- **`RK_ENABLE_NCNN=ON`** — enables ncnn; `RK_NCNN_FETCHCONTENT=ON` for auto-download. Android uses hardcoded paths in `app/build.gradle`.
- **MSVC** must use `-G "Visual Studio 17 2022" -A x64`; `/utf-8` enforced in CMakeLists.txt. If HDF5 link errors occur, rebuild with `-DBUILD_opencv_hdf=OFF` (x64/arm64 builder mismatch).
- **Android**: separate `NCNN_DIR_ARMv7`/`NCNN_DIR_ARM64` for per-ABI ncnn paths (hardcoded `D:/ProgramData/NCNN/ncnn-20260113-android-vulkan` in `app/build.gradle`). Default ABIs `armeabi-v7a` + `arm64-v8a` only — pass `-PRK_ENABLE_X86_64=true` for x86_64. Java source/target 11.
- **FFmpegKit AAR** (`app/libs/ffmpeg-kit.aar`) is optional; RTMP streaming disabled when absent.
- **RK MPP**: set `RK_MPP_HOME` env or files go to `deps/rk_mpp/` (known path `D:\ProgramData\rkmpp\mpp-1.0.11`). Auto-fallback to CPU on missing headers (`RK_HAVE_MPP=0`).
- **Gradle wrapper** 使用 `gradle-9.0`；CI 使用 Temurin Java 21。始终传入 `--no-daemon`。
- **Web build outputs** to `src/win/app/webroot/` (consumed by Windows local service). Built assets **are tracked in git**. `pnpm -C web dev` proxies `/api` to `http://127.0.0.1:8080` (override via `VITE_DEV_PROXY_TARGET`).

## CI pipeline (`.github/workflows/ci.yml`)

6 jobs on `master` branch PR/push (ignores `*.md` and `docs/`):
1. **repo-hygiene** — `node scripts/clean-repo-junk.js scan --ci`
2. **unit-tests** — Linux CMake + Ninja, `RK_SKIP_OPENCV=ON`, builds/runs `core_unit_tests` + cross-platform compile-check for `src/win/src/*.cpp` via `g++`
3. **docs-audit** — `node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit --link-cache tests/reports/docs-sync-audit/link-cache.json`
4. **web** — `pnpm install` → `pnpm lint` → `pnpm build` → `pnpm e2e:run:coverage` (E2E failures non-blocking)
5. **android** — `./gradlew -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`
6. **windows** — 在 PR 和 push 时运行，`workflow_dispatch` 时跳过该 job。完整从源码构建 OpenCV（缓存于 `_deps/`）。INT8 量化和精度步骤仅在模型、量化脚本或校准数据变更时运行，且为非阻断步骤。

## Test framework

本项目同时使用自定义 `bool` 测试和 GoogleTest；新增测试必须沿用所在 target 的既有框架，不能假定全项目禁用 `TEST()`/`TEST_F()`。

**自定义 `bool` 测试**：测试文件声明 `bool test_xxx()`，并在对应 `*_main.cpp` 的 `TestCase` 表中注册：
```cpp
using TestFn = bool (*)();
struct TestCase { const char* name; TestFn fn; };
```
Output format: `TEST_PASS name=...` / `TEST_FAIL name=...` / `TEST_SUMMARY pass=N fail=N total=N`.
- `face_infer_unit_tests` — tests in `tests/cpp/`, registered in `face_infer_unit_tests_main.cpp`
- `win_unit_tests` — 自定义用例注册于 `win_unit_tests_main.cpp`
- `ncnn_precision_test` — 独立的自定义 `bool` 测试运行器

**GoogleTest**：`core_unit_tests`、`core_gtest_tests` 及包含 `test_http_faces_server.cpp` 的测试源使用 GoogleTest；在这些 target 中使用 `TEST()`/`TEST_F()`，并保持现有的 `RUN_ALL_TESTS()` 入口。

## Test commands

```powershell
# C++ unit tests (no OpenCV)
cmake -S . -B build_ci -G "Ninja" -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target core_unit_tests
ctest --test-dir build_ci --output-on-failure

# Windows (needs OPENCV_ROOT + OPENCV_CONTRIB_ROOT)
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 `
  -DOPENCV_ROOT="path\to\opencv" -DOPENCV_CONTRIB_ROOT="path\to\opencv_contrib"
cmake --build build_win --config Release --target win_unit_tests
ctest --test-dir build_win -C Release

# Face inference tests (needs OpenCV + OPENCV_CONTRIB_ROOT)
cmake --build build_win --config Release --target face_infer_unit_tests

# ncnn precision test (standalone, static CRT /MT, needs ncnn + OpenCV)
cmake --build build_win --config Release --target ncnn_precision_test

# Web
pnpm -C web install
pnpm -C web lint
pnpm -C web build
pnpm -C web e2e:run

# Android
gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug

# Benchmarks
cmake --build build_win --config Release --target win_face_eval_cli win_face_bench_cli
```

## Architecture notes (post-refactor)

### Three core modules → Pipeline architecture

After the triple refactor (2026-07, PR #427), the three monolithic classes are now thin coordinators:

**EngineRuntime** (`src/cpp/src/Engine.cpp: ~280 lines, was 1294`)
- `FrameSource` — unified input (VideoManager / ExternalInput)
- `TrackCoordinator` — pure-function IoU matching + stableId + TTL (**zero OpenCV dep, in core_unit_tests**)
- `ResultPublisher` — callback throttling + render frame publishing
- `PerfReporter` — async P50/P95 aggregation + CSV write

**FramePipeline** (`src/win/src/FramePipeline.cpp: ~350 lines, was 747`)
- `RuntimeBootstrap::build()` — recognizer/DNN/ModelSnapshot assembly
- `CameraSession::switchWithRollback()` — camera open/fallback/first-frame timeout
- `FrameProcessor::run()` — pure computation (detect → recognize → result)
- `SideEffectSink::publish()` — overlay drawing + structured log + render state

**HttpFacesServer** (`src/win/src/HttpFacesServer.cpp: ~500 lines, was 1005`)
- `EndpointRegistry` — Route table + method validation + unified error formatting
- `JsonEndpointHandlers` — endpoint handlers by domain (models/settings/cameras/actions)
- `StreamSessionRunner` — SSE + MJPEG unified streaming

### Acceleration contract

- **Acceleration contract**: every accelerator (MPP, ncnn, libyuv, Qualcomm, OpenCL) uses `requested`/`effective`/`evidence`/`reason` pattern in `Engine::performAccelSelfCheck()` or `inference_bench_cli`. Fixed reason codes: `ok`, `build_disabled`, `unsupported_platform`, `missing_dependency`, `missing_model`, `runtime_init_failed`, `unsupported_input`.
- **Windows config**: `%APPDATA%\rk_wcfr\config.json` (source of truth, JSON Schema at `docs/windows-web-spa/config.schema.json`). Legacy `config/windows_camera_face_recognition.ini` for migration only.
- **Logging**: Android — `adb logcat | grep rk3288_opencv`. Windows 默认写入 exe 同级的 `storage/win_logs/`，可由 `%APPDATA%\rk_wcfr\config.json` 的 `log.logDir` 覆盖。
- **RK3288 target**: `armeabi-v7a`, Cortex-A17, NEON required (`-mcpu=cortex-a17 -mfpu=neon-vfpv4`). No hardware acceleration available in practice (CPU-only fallback).
- **Java source**: `../src/java` (referenced from `app/build.gradle` via `sourceSets.main.java.srcDirs`).
- **Java unit tests**: `tests/unit/java/` (via `sourceSets.test.java.srcDirs`).
- **Test fixtures**: `tests/fixtures/mock/` has corrupt/incomplete images for mock preflight guard tests.
- **CivetWeb** 1.16 位于 `src/win/third_party/civetweb/`，仅为未编入目标的第三方源码和许可证快照；Windows 本地服务由自有 Winsock `HttpFacesServer` 提供 REST API 与静态文件托管。
- **OpenCV built as subdirectory**: not via `find_package`; CMakeLists.txt uses `add_subdirectory("${OPENCV_ROOT}" ...)` directly.
- **libyuv**: CMake 的 Android 默认值为 `RK_ENABLE_LIBYUV=ON`、主机构建默认 OFF；但 `app/build.gradle` 当前显式传入 `-DRK_ENABLE_LIBYUV=OFF`，因此标准 Android Gradle 构建不会拉取 libyuv。
- **Qualcomm SNPE**: optional, falls back to CPU if headers not in `deps/qualcomm_snpe/include` or `$QUALCOMM_HOME`.
- **OpenAPI spec** at `docs/windows-web-spa/openapi.yaml`.

## INT8 quantization pipeline

- **Script**: `scripts/quantize_ncnn_int8.py` — two-step flow (ncnn2table → ncnn2int8). Supports `--table-only`/`--quant-only`.
- **Models**: 量化脚本和 CI 使用 SCRFD（检测，经 PNNX 转换自 `det_10g.onnx`）、ArcFace（识别）和 MobileFaceNet（轻量级），目录使用 `scrfd`。现有 C++ INT8 结构测试与精度测试仍引用历史 `yolo_face` 目录；在完成测试迁移前不得假定路径已统一。
- **Model binaries are gitignored** (`models/*` excludes `.gitkeep`). Generate locally via the quantize script.
- **Calibration images**: 172 face images from `deps/WIDER_train/` (not `tests/test_set01/` in practice). Contents of `deps/WIDER_train/` and `deps/insightface/` are gitignored (only `.gitkeep` tracked) — download WIDER Face dataset separately.
- **Precision tests**: 8 structural tests (`test_int8_*.cpp`) have `fileExists()` skip — pass when models absent. `ncnn_precision_test` (standalone) enables full inference comparison; ArcFace cosine similarity target ≥ 0.90.
- **CRT**: project default is static CRT (`/MT`). `ncnn_precision_test` also uses static CRT (`MultiThreaded`), matching the global default (no workaround needed).
- **SCRFD → PNNX note**: ncnn2int8 cannot handle `splitncnn` nodes (multi-scale detection heads). Only PNNX-direct output (optlevel=2) produces clean graphs for INT8 quantization.
- **Android**: INT8 tests are structural only (fileExists skip). `ncnn_precision_test` is NOT built on Android.

## PR 合并规则（不可修改）

合并任何 PR 之前必须：
1. **检查 PR comment** — 逐条阅读并解决所有评论意见，未解决的不得合并
2. **检查并修复 conflict** — `git merge origin/master` 如有冲突必须全部解决
3. **CI 验证** — 等待 CI 全部通过后方可合并到 `master` 分支
4. 合并方式：仅使用 squash and merge（非 merge commit，非 rebase merge）
5. 删除已合并的源分支

## Conventions

- UTF-8 encoding enforced throughout (Gradle `options.encoding`, MSVC `/utf-8`)
- C++17 standard (`CMAKE_CXX_STANDARD 17`)
- Algorithm-critical logic, JNI boundaries, and hardware-difference handling **must have Chinese comments**
- Other AI-agent working dirs (`.trae/specs/`, `.Jules/`, `.codegraph/`): scratch history, not authoritative docs

## PR workflow

Before merging any PR into `master`:
1. **审查 comments** — 检查 PR 上的 review comments 和 issue comments，确认全部已解决或已明确忽略
2. **解决冲突** — 检查并修复所有 merge conflicts
3. **等待 CI** — 等待 CI 流水线（`.github/workflows/ci.yml` 中 6 个 job）全部通过后，方可合并
4. **合并策略** — 仅使用 squash and merge；禁止 rebase merge 和 merge commit
5. **不修改 PR 规则** — 不绕开或更改分支保护规则（如 `--admin` 标志仅在紧急修复时使用）

## Scripts & tools

| Script | Purpose |
|:---|:---|
| `scripts/clean-repo-junk.js` | Repository hygiene scan |
| `scripts/quantize_ncnn_int8.py` | INT8 model quantization (ncnn2table → ncnn2int8) |
| `scripts/run-web-e2e.ps1` | Full Cypress E2E + coverage for Web SPA |
| `scripts/bench_camera_adb.ps1` / `.sh` | Android camera benchmark via ADB |
| `scripts/stability_switch_50_adb.ps1` | Android foreground/background stability test (50 cycles) |

## Key documents

- **DEVELOP.md** — detailed module descriptions, build variants, acceleration architecture
- **CREDITS.md** — full dependency checklist + model inventory
- **README.md** — project overview, quick start, todo list status
- **docs/windows-web-spa/openapi.yaml** — REST API spec
- **docs/windows-web-spa/config.schema.json** — Windows config JSON Schema
- **docs/superpowers/specs/2026-07-03-triple-refactor-design.md** — 三层重构设计文档
- **docs/superpowers/plans/2026-07-03-triple-refactor-plan.md** — 三层重构实施计划

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
