# RK3288 AI Engine — Agent Guide

## Four independent subsystems

| Subsystem | Build | Entrypoint | Tests |
|-----------|-------|------------|-------|
| Android App (`app/` + `src/java/`) | `gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug` | `src/cpp/native-lib.cpp` (JNI), `MainActivity.java` | `tests/unit/java/` via Gradle |
| Windows Service (`src/win/`) | CMake + VS 2022 | `win_local_service_main.cpp` (service, **current default**), `win_camera_face_recognition_main.cpp` (legacy UI, OFF by default) | `win_unit_tests`, `win_face_database_perf`, `win_face_eval_cli`, `win_face_bench_cli` |
| Web SPA (`web/`) | `pnpm -C web` (build/lint/dev) | `web/src/` (Vite + React + Ant Design) | `pnpm -C web e2e:run` (Cypress), `pnpm -C web e2e:run:coverage`; E2E needs `pnpm preview` or `scripts/run-web-e2e.ps1` for backend |
| Native C++ Core (`src/cpp/`) | CMake (cross-platform) | `Engine::initialize()` → `FaceInferencePipeline::process()` | `core_unit_tests` (no OpenCV), `face_infer_unit_tests` (needs OpenCV) |

## Essential build quirks

- **`OPENCV_ROOT`** must point to OpenCV **source** (not install), set via env or `-D` flag; CMakeLists.txt validates presence and checks for `CMakeLists.txt` inside. **`OPENCV_CONTRIB_ROOT`** is also required for the `opencv_face` module (face recognition); set both for full builds.
- **`RK_SKIP_OPENCV=ON`** — for building `core_unit_tests` without OpenCV (CI uses this); in Gradle pass as `-PRK_SKIP_OPENCV=true`. Uses mock stubs at `deps/opencv/` to satisfy linking without real OpenCV headers.
- **`RK_ENABLE_NCNN=ON`** — enables ncnn; `RK_NCNN_FETCHCONTENT=ON` for auto-download
- **MSVC** must use `-G "Visual Studio 17 2022" -A x64`; MSVC needs `/utf-8` compile flag (set in CMakeLists.txt)
- **Android**: separate `NCNN_DIR_ARMv7`/`NCNN_DIR_ARM64` for per-ABI ncnn paths (hardcoded `D:/ProgramData/NCNN/ncnn-20260113-android-vulkan` in `app/build.gradle`); default ABIs are `armeabi-v7a` + `arm64-v8a` only — pass `-PRK_ENABLE_X86_64=true` to add x86_64. Java source/target is 11.
- **FFmpegKit AAR** (`app/libs/ffmpeg-kit.aar`) is optional — only needed for RTMP streaming; if absent, RTMP is disabled but face recognition still works.
- **RK MPP**: set `RK_MPP_HOME` env or files go to `deps/rk_mpp/` (known path `D:\ProgramData\rkmpp\mpp-1.0.11`); auto‑fallback to CPU on missing headers (`RK_HAVE_MPP=0`)
- **Gradle wrapper** is `gradle-9.0-milestone-1` (pre‑release, experimental); CI uses Java 17 (Temurin) for the Android job.
- **Web build outputs** to `src/win/app/webroot/` (consumed by Windows local service at runtime); these built assets **are tracked in git**; `pnpm -C web dev` proxies `/api` to `http://127.0.0.1:8080` (override via `VITE_DEV_PROXY_TARGET` env)

## CI pipeline (`.github/workflows/ci.yml`)

6 jobs, all on `master` branch PR/push (ignores `*.md` and `docs/`):
1. **repo-hygiene** — `node scripts/clean-repo-junk.js scan --ci`
2. **unit-tests** — Linux CMake + Ninja, `RK_SKIP_OPENCV=ON`, builds/runs `core_unit_tests`
3. **docs-audit** — `node scripts/docs-sync-audit.js`
4. **web** — `pnpm install` → `pnpm lint` → `pnpm build` → `pnpm e2e:run:coverage` (E2E failures are non-blocking)
5. **android** — `./gradlew -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`
6. **windows** — only on push (not PR) with "Windows" in commit msg or `workflow_dispatch`; full OpenCV build from source (cached via `_deps/`)

## Test commands (quick reference)

```powershell
# C++ unit tests (no OpenCV)
cmake -S . -B build_ci -G "Ninja" -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target core_unit_tests
ctest --test-dir build_ci --output-on-failure

# Windows unit tests (needs OPENCV_ROOT)
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT="path\to\opencv"
cmake --build build_win --config Release --target win_unit_tests
ctest --test-dir build_win -C Release

# Face inference tests (needs OpenCV)
cmake --build build_win --config Release --target face_infer_unit_tests

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

- **Acceleration contract**: every accelerator (MPP, ncnn, libyuv, Qualcomm, OpenCL) uses `requested`/`effective`/`evidence`/`reason` pattern in `Engine::performAccelSelfCheck()` or `inference_bench_cli`; fixed reason codes: `ok`, `build_disabled`, `unsupported_platform`, `missing_dependency`, `missing_model`, `runtime_init_failed`, `unsupported_input`
- **Windows config**: `%APPDATA%\rk_wcfr\config.json` (source of truth, JSON Schema at `docs/windows-web-spa/config.schema.json`); legacy `config/windows_camera_face_recognition.ini` for migration only
- **Logging**: Android — `adb logcat | grep rk3288_opencv`; Windows — `%APPDATA%\rk_wcfr\logs\`
- **RK3288 target**: `armeabi-v7a`, Cortex-A17, NEON required (`-mcpu=cortex-a17 -mfpu=neon-vfpv4`); no hardware acceleration available in practice (CPU-only fallback)
- **Security**: `/GS` (MSVC), `-fstack-protector-strong -Wl,-z,relro -Wl,-z,now -fPIE -pie` (GCC)
- **Java source**: lives in `../src/java` (referenced from `app/build.gradle` via `sourceSets.main.java.srcDirs`)
- **Java unit tests**: in `tests/unit/java/` (referenced via `sourceSets.test.java.srcDirs`)
- **Test fixtures**: `tests/fixtures/mock/` has corrupt/incomplete images for mock preflight guard tests
- **Model quantization**: `scripts/quantize_ncnn_int8.py` for FP32 → INT8 ncnn model conversion
- **Build/CI report outputs** (gitignored): `tests/metrics/`, `tests/reports/`, `tests/data/`, `tests/test_set01/`
- **OpenCV built as subdirectory**: not found via `find_package`; CMakeLists.txt uses `add_subdirectory("${OPENCV_ROOT}" ...)` directly
- **libyuv**: auto-fetched via FetchContent on Android (`RK_ENABLE_LIBYUV=ON` by default for Android); host builds default OFF
- **Qualcomm SNPE**: optional, falls back to CPU if headers not in `deps/qualcomm_snpe/include` or `$QUALCOMM_HOME`
- **OpenAPI spec** at `docs/windows-web-spa/openapi.yaml` documents the REST API
- **Missing DNN model files**: legacy INI (`config/windows_camera_face_recognition.ini`) references `opencv_face_detector_uint8.pb` and `.pbtxt` — Windows DNN face detection is non-functional without manual download
- **Other AI-agent working dirs** (`.trae/specs/`, `.Jules/`, `.codegraph/`): these are other agents' scratch/spec history, not source of truth; don't treat them as authoritative docs

## Conventions

- UTF-8 encoding enforced throughout (Gradle `options.encoding`, MSVC `/utf-8`)
- C++17 standard (`CMAKE_CXX_STANDARD 17`)
- Algorithm-critical logic, JNI boundaries, and hardware-difference handling must have **Chinese comments**
- No hardcoded secrets/keys in code or logs
- Branch strategy: `master` (stable), `feature/*`, `hotfix/*`, semantic version tags
- Release steps: `node scripts/docs-sync-audit.js` → update CHANGELOG.md → tag version

## Agent 工作规则（持久化指令）

### 语言与表达
- 全过程必须使用中文（ZH_CN），包括回复与生成内容（除非明确要求英文）。
- 假设用户无编程经验：避免术语堆砌；给出可复制的完整命令与明确路径；步骤要编号。
- 必须为"为什么这样做/有什么坑/影响范围"写注释；禁止只复述代码行为的无意义注释；修改逻辑时同步更新相关注释。

### 行动与确认方式
- 遇到报错：优先解释"错误含义→最可能原因→最短修复路径→如何验证"。
- 需要改配置/删文件/覆盖文件时：先给"将修改什么 + 风险 + 回滚方式"，再执行。

### 安全与隐私
- 不输出/不记录密钥、令牌、隐私信息；日志贴出前提醒打码。
- 默认不执行高风险操作（强制推送、删除大目录、覆盖系统路径等），除非明确要求。

### 工具优先级（减少绕路）
- 本地文件/终端/环境检查优先走本机工具链。
- 网页资料才用搜索/抓取类工具。
- 多步骤复杂任务先给计划再执行。

### 编码原则
1. 编码前思考：遇到歧义必须先问、先呈现权衡，不许默不作声猜需求。
2. 简洁优先：50行能写完绝不写200行，不加未请求的功能，不做一次性抽象。
3. 精准修改：只改必须改的地方，不许借"顺手优化"之名碰相邻代码。
4. 目标驱动执行：把"修复Bug"改成"先写一个能复现Bug的测试，再让它通过"。
