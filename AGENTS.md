# RK3288 AI Engine — Agent Guide

## Four independent subsystems

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
- **Gradle wrapper** is `gradle-9.0-milestone-1` (pre-release, experimental). CI uses Java 17 (Temurin). Always use `--no-daemon`.
- **Web build outputs** to `src/win/app/webroot/` (consumed by Windows local service). Built assets **are tracked in git**. `pnpm -C web dev` proxies `/api` to `http://127.0.0.1:8080` (override via `VITE_DEV_PROXY_TARGET`).

## CI pipeline (`.github/workflows/ci.yml`)

6 jobs on `master` branch PR/push (ignores `*.md` and `docs/`):
1. **repo-hygiene** — `node scripts/clean-repo-junk.js scan --ci`
2. **unit-tests** — Linux CMake + Ninja, `RK_SKIP_OPENCV=ON`, builds/runs `core_unit_tests` + cross-platform compile-check for `src/win/src/*.cpp` via `g++`
3. **docs-audit** — `node scripts/docs-sync-audit.js`
4. **web** — `pnpm install` → `pnpm lint` → `pnpm build` → `pnpm e2e:run:coverage` (E2E failures non-blocking)
5. **android** — `./gradlew -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`
6. **windows** — push only (not PR), triggered by `"Windows"` in commit msg or `workflow_dispatch`. Full OpenCV build from source (cached in `_deps/`). Includes INT8 quantization + precision test steps.

## Test framework

**Custom `bool` functions** (NOT Google Test). Each test file declares a `bool test_xxx()` function and registers it in a `TestCase` table in the `*_main.cpp` file:
```cpp
using TestFn = bool (*)();
struct TestCase { const char* name; TestFn fn; };
```
Output format: `TEST_PASS name=...` / `TEST_FAIL name=...` / `TEST_SUMMARY pass=N fail=N total=N`.
- `face_infer_unit_tests` — tests in `tests/cpp/`, registered in `face_infer_unit_tests_main.cpp`
- `core_unit_tests` — tests in `tests/cpp/`, registered in `core_unit_tests_main.cpp`
- `win_unit_tests` — tests in `tests/win/`, registered in `win_unit_tests_main.cpp`

**Do NOT write `TEST()` or `TEST_F()` macros.** Add a `bool test_xxx()` function and register it.

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

## Architecture notes

- **Acceleration contract**: every accelerator (MPP, ncnn, libyuv, Qualcomm, OpenCL) uses `requested`/`effective`/`evidence`/`reason` pattern in `Engine::performAccelSelfCheck()` or `inference_bench_cli`. Fixed reason codes: `ok`, `build_disabled`, `unsupported_platform`, `missing_dependency`, `missing_model`, `runtime_init_failed`, `unsupported_input`.
- **Windows config**: `%APPDATA%\rk_wcfr\config.json` (source of truth, JSON Schema at `docs/windows-web-spa/config.schema.json`). Legacy `config/windows_camera_face_recognition.ini` for migration only.
- **Logging**: Android — `adb logcat | grep rk3288_opencv`. Windows — `%APPDATA%\rk_wcfr\logs\`.
- **RK3288 target**: `armeabi-v7a`, Cortex-A17, NEON required (`-mcpu=cortex-a17 -mfpu=neon-vfpv4`). No hardware acceleration available in practice (CPU-only fallback).
- **Java source**: `../src/java` (referenced from `app/build.gradle` via `sourceSets.main.java.srcDirs`).
- **Java unit tests**: `tests/unit/java/` (via `sourceSets.test.java.srcDirs`).
- **Test fixtures**: `tests/fixtures/mock/` has corrupt/incomplete images for mock preflight guard tests.
- **CivetWeb** (embedded HTTP server) at `src/win/third_party/civetweb/` — used by Windows local service for REST API + static file hosting; already tracked in repo, no external fetch needed.
- **OpenCV built as subdirectory**: not via `find_package`; CMakeLists.txt uses `add_subdirectory("${OPENCV_ROOT}" ...)` directly.
- **libyuv**: auto-fetched via FetchContent on Android (`RK_ENABLE_LIBYUV=ON` by default for Android); host builds default OFF.
- **Qualcomm SNPE**: optional, falls back to CPU if headers not in `deps/qualcomm_snpe/include` or `$QUALCOMM_HOME`.
- **OpenAPI spec** at `docs/windows-web-spa/openapi.yaml`.

## INT8 quantization pipeline

- **Script**: `scripts/quantize_ncnn_int8.py` — two-step flow (ncnn2table → ncnn2int8). Supports `--table-only`/`--quant-only`.
- **Models**: SCRFD (detection, via PNNX from `det_10g.onnx`), ArcFace (recognition), MobileFaceNet (lightweight). Model directory names use `scrfd` (not `yolo_face`).
- **Model binaries are gitignored** (`models/*` excludes `.gitkeep`). Generate locally via the quantize script.
- **Calibration images**: 172 face images from `deps/WIDER_train/` (not `tests/test_set01/` in practice). Contents of `deps/WIDER_train/` and `deps/insightface/` are gitignored (only `.gitkeep` tracked) — download WIDER Face dataset separately.
- **Precision tests**: 8 structural tests (`test_int8_*.cpp`) have `fileExists()` skip — pass when models absent. `ncnn_precision_test` (standalone) enables full inference comparison; ArcFace cosine similarity target ≥ 0.90.
- **CRT**: project default is static CRT (`/MT`). `ncnn_precision_test` also uses static CRT (`MultiThreaded`), matching the global default (no workaround needed).
- **SCRFD → PNNX note**: ncnn2int8 cannot handle `splitncnn` nodes (multi-scale detection heads). Only PNNX-direct output (optlevel=2) produces clean graphs for INT8 quantization.
- **Android**: INT8 tests are structural only (fileExists skip). `ncnn_precision_test` is NOT built on Android.

## Conventions

- UTF-8 encoding enforced throughout (Gradle `options.encoding`, MSVC `/utf-8`)
- C++17 standard (`CMAKE_CXX_STANDARD 17`)
- Algorithm-critical logic, JNI boundaries, and hardware-difference handling **must have Chinese comments**
- Other AI-agent working dirs (`.trae/specs/`, `.Jules/`, `.codegraph/`): scratch history, not authoritative docs
