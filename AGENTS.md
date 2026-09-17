<!-- DOCSYNC_DOC
type=process
status=living
as_of=2026-09-17
-->

# RK3288 AI Engine — Agent Guide

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

## 开发目标平台

**当前只开发 Android 与 Windows 两个子系统；Linux 暂不作为开发目标。**

- 「不作为开发目标」= 不为它写代码、不要求为它做本地编译验证；**不等于删除 Linux CI**。
- 判断是否属于「Linux 开发」要看**是否把 Linux 当目标平台**，而不是看 runner 的操作系统：
  `.github/workflows/ci.yml` 中有 6 个 job 跑在 `ubuntu-latest`，其中只有 **`unit-tests`** 具有
  Linux 目标性质（CMake + Ninja 编译 `core_unit_tests`，并用 `g++` 跨平台编译检查
  `src/win/src/*.cpp`）；`changes` / `repo-hygiene` / `docs-audit` / `web` / `android` 只是
  Node / Gradle 工具链借用了 ubuntu runner，**不在「Linux 开发」之列**。
- `unit-tests` job **保留**：此时它的身份是「`core_unit_tests` 的快通道」
  （`RK_SKIP_OPENCV=ON`，1–2 分钟出结果）。删除它不会丢覆盖——`windows` job 同样构建并运行
  `core_unit_tests`——但会让 PR 反馈明显变慢。
- 相应地，该 job 中 `g++` 对 `src/win/src/*.cpp` 的跨平台编译检查**不再作为验收依据**；
  新增 C++ 代码只须在 Windows（MSVC）与 Android 两侧验证。
- ⚠️ **不要**把 `CMakeLists.txt:121` 的 `if(UNIX AND NOT APPLE)` 当作 Linux 残留删掉——
  **Android 也是 UNIX**，该分支在为 Android 加 `-fPIE -pie` 安全加固，删掉会让加固静默失效。

## 已评估但不采纳的技术方向

以下方向经调研后**明确不采纳**，附理由。完整论证与出处见
`docs/research/opensource-integration-feasibility-2026-09-16.md`。
**不要重新论证这些结论，除非有新的实测数据。**

- **1:N 检索引入 ANN（hnswlib / faiss）** —— 10,000×512 维全库余弦仅约 10 MFLOP，
  Cortex-A17 走 NEON 是**几毫秒**量级，ANN 换不到加速。且 hnswlib 在 `armeabi-v7a`
  上索引加载失败（issue #516，32 位 `size_t` 所致），faiss 维护者亦明确 32 位平台
  「更麻烦」。**保持 `FaceSearchLinearIndex` 线性检索**；底库超约 10 万条再议。
- **替换加密库（libsodium / mbedTLS / OpenSSL）** —— `libsodium` 的 AES-256-GCM 要求
  Intel AES-NI 或 **ARM Crypto Extensions**，Cortex-A17 二者皆无，官方明确「无计划支持
  非硬件加速实现」；RK3288 硬件加密引擎支持 AES 的 ECB/CBC/OFB/CTR/CMAC/XTS/**CCM**，
  **独缺 GCM**。三个库在本平台给出的 GCM 都是纯软件实现，**不会更快**。
- **替换视频传输层** —— MJPEG/SSE 分帧本身不是瓶颈（瓶颈在每帧 JPEG 编码）。另
  `jacksonliam/mjpg-streamer` **无任何许可证声明**，法律上属「保留所有权利」，
  仅可参考、不可用于产品。
- **`libfacedetection` 作为检测器** —— 纯 C++/无依赖/BSD-3 看着很美，但其模型以
  **C 数组内嵌**，编进 `.so` 会**绕过「APK 不内置模型」策略**与 `checkApkFaceModels` 断言。
- **把非 C/C++ 实现「翻译」成 C++ 以换取性能** —— 实测反例：`mudler/face-detect.cpp`
  的 C++17 重写版**比 onnxruntime 慢**（检测 0.69–0.83x、嵌入 0.61–0.84x）。语言替换的
  量级只有 **1.0–1.1x**；真正的收益在**模型选择**与**拷贝消除**（后者属架构缺陷，与语言无关）。

## 四个独立子系统

| 子系统 | 构建 | 入口 | 测试 |
|---|---|---|---|
| Android App (`app/` + `src/java/`) | `gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug` | `src/cpp/native-lib.cpp` (JNI)、`MainActivity.java` | `tests/unit/java/`（经 Gradle） |
| Windows 服务 (`src/win/`) | CMake + VS 2022 | `src/win/app/win_local_service_main.cpp`（服务，默认构建）、`src/win/app/win_camera_face_recognition_main.cpp`（旧版 UI，默认 OFF） | `win_unit_tests`、`win_face_database_perf`；CLI 工具 `win_face_eval_cli`、`win_face_bench_cli` |
| Web SPA (`web/`) | `pnpm -C web`（build/lint/dev） | `web/src/`（Vite + React + Ant Design） | `pnpm -C web e2e:run`（Cypress）、`pnpm -C web e2e:run:coverage`；E2E 需 `pnpm preview` 或 `scripts/run-web-e2e.ps1` 提供后端 |
| C++ 核心引擎 (`src/cpp/`) | CMake（跨平台） | `Engine::initialize()` → `FaceInferencePipeline::process()` | `core_unit_tests`（无需 OpenCV）、`face_infer_unit_tests`（需 OpenCV） |

## 构建与测试命令

```powershell
# C++ 单测（无需 OpenCV）——与 CI 一致
cmake -S . -B build_ci -G "Ninja" -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target core_unit_tests
ctest --test-dir build_ci --output-on-failure

# Windows（需 OPENCV_ROOT + OPENCV_CONTRIB_ROOT）
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 `
  -DOPENCV_ROOT="path\to\opencv" -DOPENCV_CONTRIB_ROOT="path\to\opencv_contrib"
cmake --build build_win --config Release --target win_unit_tests
ctest --test-dir build_win -C Release

# 人脸推理测试（需 OpenCV + OPENCV_CONTRIB_ROOT）
cmake --build build_win --config Release --target face_infer_unit_tests

# ncnn 精度测试（独立目标，静态 CRT /MT，需 ncnn + OpenCV）
cmake --build build_win --config Release --target ncnn_precision_test

# Web
pnpm -C web install
pnpm -C web lint
pnpm -C web build
pnpm -C web e2e:run

# Android
gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug

# 基准
cmake --build build_win --config Release --target win_face_eval_cli win_face_bench_cli
```

## Essential build quirks

- **`OPENCV_ROOT`** 必须指向 OpenCV **源码目录**（不是安装目录），经环境变量或 `-D` 传入；CMake 校验 `${OPENCV_ROOT}/CMakeLists.txt` 存在，否则直接 FATAL_ERROR。**`OPENCV_CONTRIB_ROOT`** 提供 `opencv_face` 模块（LBPH 识别器依赖）；未设置时 CMake 不报错，仅相关功能不参与构建。
- **`RK_SKIP_OPENCV=ON`** —— 不依赖 OpenCV 构建 `core_unit_tests`（CI 使用此模式）。Gradle 侧传 `-PRK_SKIP_OPENCV=true`。链接使用 `deps/opencv/` 下的 mock stub。
- **`RK_ENABLE_NCNN`**（默认 OFF）启用 ncnn；`RK_NCNN_FETCHCONTENT`（默认 OFF）允许本机未安装时经 FetchContent 拉取。Android 侧 ncnn 路径硬编码在 `app/build.gradle`。
- **MSVC** 必须用 `-G "Visual Studio 17 2022" -A x64`；`/utf-8` 是**按 target 逐个开启**（见 `CMakeLists.txt`、`cmake/core.cmake`），并非全局设置。若遇 HDF5 链接错误，以 `-DBUILD_opencv_hdf=OFF` 重建（x64/arm64 builder 不匹配）。
- **Android**：`NCNN_DIR_ARMv7` / `NCNN_DIR_ARM64` 按 ABI 分别指定 ncnn 路径（`app/build.gradle` 默认 `D:/ProgramData/NCNN/ncnn-20260113-android-vulkan`）。默认 ABI 仅 `armeabi-v7a` + `arm64-v8a`；传 `-PRK_ENABLE_X86_64=true` 才追加 x86_64。Java source/target **11**。
- **FFmpegKit AAR**（`app/libs/ffmpeg-kit.aar`）为可选件；缺失时 RTMP 推流禁用，不影响核心人脸识别。
- **RK MPP**：设置 `RK_MPP_HOME` 环境变量，否则查找 `deps/rk_mpp/include`、`deps/rk_mpp/inc`（本机已知路径 `D:\ProgramData\rkmpp\mpp-1.0.11`）。缺头文件时置 `RK_HAVE_MPP=0` 并自动回落 CPU。
  - ⚠️ **目标设备实测（2026-09-17）**：设备暴露的是**旧 `vpu-service` / `hevc-service` 设备节点**，**没有** `/dev/mpp*`，也**没有** `/proc/mpp_service` —— **设备侧并非新版 MPP 框架**。`mpp-1.0.11` 与设备驱动的匹配性**未经真机验证**；不要把「本机能编过」当作「设备上能跑」。详见 `docs/research/rk3288-device-capability-survey-2026-09-17.md`。
- **Gradle wrapper** 为 `gradle-9.0`（wrapper 已锁定 `distributionSha256Sum`）；CI 用 Temurin Java 21。始终传 `--no-daemon`。
- **Web 构建产物**输出到 `src/win/app/webroot/`（由 Windows 本地服务托管），**产物已纳入 git**。`pnpm -C web dev` 把 `/api` 代理到 `http://127.0.0.1:8080`，可用 `VITE_DEV_PROXY_TARGET` 覆盖。

## CI pipeline

仓库有**两个 workflow**。

**`.github/workflows/ci.yml`** —— 7 个 job，触发于 `master` 的 PR / push 及 `workflow_dispatch`：

1. **changes**（`classify-changes`）—— 比对变更路径并输出 `docs_only`；纯 `docs/` + `*.md` 变更时为 `true`。无法可靠比对时（手动触发、首个 push）保守地置 `false`。
2. **repo-hygiene** —— `node scripts/clean-repo-junk.js scan --ci`，另跑 `node --test tests/scripts/*.test.js` 与量化配对 pytest。
3. **unit-tests** —— Linux CMake + Ninja，`RK_SKIP_OPENCV=ON`，构建并运行 `core_unit_tests`，同时对 `src/win/src/*.cpp` 做 `g++` 跨平台编译检查。
4. **docs-audit** —— `node scripts/docs-sync-audit.js --skip-links --out-dir tests/reports/docs-sync-audit`。门禁要求 high=0 且 medium=0，不满足时以退出码 2 阻断。**本 job 不因 `docs_only` 跳过**——docs 变更正是它的审查对象（其余 job 在 pure-docs 变更时全部跳过）。
5. **web** —— `pnpm install` → `lint` → `build` → `e2e:run:coverage`（E2E 失败非阻断）。
6. **android** —— `./gradlew -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`（Temurin Java 21）。
7. **windows** —— 仅 `pull_request` / `push` 触发（`workflow_dispatch` 时跳过），且要求 `docs_only != true`。从源码完整构建 OpenCV（缓存于 `_deps/`）。INT8 量化与精度步骤仅在 `models/`、`scripts/quantize_ncnn_int8.py` 或 `deps/WIDER_train/` 发生变更时运行，且为 `continue-on-error` 非阻断。

**`.github/workflows/docs-audit.yml`** —— 独立 workflow，先跑 `node --test tests/scripts/docs-sync-audit.test.js`，再执行与 ci.yml 完全相同的 `--skip-links` 审计命令。

## Test framework

本项目**同时**使用自定义 `bool` 测试和 GoogleTest；新增测试必须沿用所在 target 的既有框架，不能假定全项目禁用 `TEST()` / `TEST_F()`。

**GoogleTest**：`core_unit_tests`、`core_gtest_tests`，以及 `tests/win/test_http_faces_server.cpp` 使用 `TEST()` / `TEST_F()`，入口为 `RUN_ALL_TESTS()`。注意 `win_unit_tests` 是**混合 target**——runner 为自定义 `bool` 表驱动，同时链接了 `test_http_faces_server.cpp` 的 GTest 用例。

**自定义 `bool` 测试**：测试文件声明 `bool test_xxx()`，并在对应 `*_main.cpp` 的 `TestCase` 表中注册：

```cpp
using TestFn = bool (*)();
struct TestCase { const char* name; TestFn fn; };
```

输出格式：`TEST_PASS name=...` / `TEST_FAIL name=...` / `TEST_SUMMARY pass=N fail=N total=N`。

- `face_infer_unit_tests` —— 用例位于 `tests/cpp/`，注册于 `face_infer_unit_tests_main.cpp`
- `win_unit_tests` —— 自定义用例注册于 `win_unit_tests_main.cpp`
- `ncnn_precision_test` —— 独立的自定义 `bool` 测试运行器

## Architecture notes

### 核心引擎分层（`src/cpp/`）

`Engine`（`src/cpp/src/Engine.cpp`）本身仍是单体类，但已把三块职责抽为持有对象，声明位于 `src/cpp/include/pipeline/`：

- `pipeline::TrackCoordinator`（成员 `trackCoordinator_`）—— 纯函数 IoU 匹配 + stableId + TTL，**零 OpenCV 依赖，纳入 `core_unit_tests`**
- `pipeline::ResultPublisher`（成员 `publisher_`）—— 回调节流 + 渲染帧发布
- `pipeline::PerfReporter`（成员 `perfReporter_`）—— 异步 P50/P95 聚合 + CSV 写入

`src/cpp/src/pipeline/` 下另有 `RealtimeFacePipeline`、`ScrfdDecoder`、`FacePostProcessor`。

> 早期文档中的 `EngineRuntime` / `FrameSource` 已不存在于代码库，勿再引用。

### Windows 侧拆分（`src/win/`）

**FramePipeline**（`src/win/src/FramePipeline.cpp`）

- `RuntimeBootstrap::build()` —— recognizer / DNN / ModelSnapshot 装配
- `FramePipeline::startCameraWithRollbackLocked()` —— 相机打开 / 回落 / 首帧超时
- `FrameProcessor::run()` —— 纯计算（detect → recognize → result）
- `SideEffectSink::publish()` —— overlay 绘制 + 结构化日志 + 渲染状态

**HttpFacesServer**（`src/win/src/HttpFacesServer.cpp`）

- `EndpointRegistry` —— 路由表 + 方法校验 + 统一错误格式
- `JsonEndpointHandlers` —— 按域划分的端点处理器（models / settings / cameras / actions）
- `StreamSessionRunner` —— SSE + MJPEG 统一流式输出

### JNI 边界

JNI 函数按领域拆分到 `src/cpp/jni/`（`camera.cpp` / `engine.cpp` / `preview.cpp` / `config.cpp`），经 `JniMethodRegistry` 统一注册；`native-lib.cpp` 只保留全局变量与 `JNI_OnLoad`。

### CMake 模块化

`CMakeLists.txt` 拆为 5 个 `cmake/` 模块：`opencv.cmake`（查找 + BUILD_LIST 裁剪）、`core.cmake`（`rk_core` INTERFACE 库 + `core_unit_tests`）、`face_infer.cmake`（推理管线源文件）、`android.cmake`（`native-lib` target + JNI 配置）、`windows.cmake`（Win32 服务 + CLI 工具）。

### 架构契约

`docs/architecture/ARCHITECTURE.md` 定义 5 条跨子系统契约：RAII 资源封装、纯函数管线、原子状态提交、错误通道、接口隔离。**该文档自身的状态表把 5 条全部标为「待修复」——它们是目标契约，不是已落地规则。** 检查脚本 `scripts/check-raii-violations.sh` 存在，但**未接入 CI**，需手动运行。

### Acceleration contract

每个加速器（MPP、ncnn、libyuv、Qualcomm、OpenCL）在 `Engine::performAccelSelfCheck()` 中使用 `requested` / `effective` / `evidence` / `reason` 四元组，输出为**单行日志**（非 JSON）。固定原因码：`ok`、`build_disabled`、`unsupported_platform`、`missing_dependency`、`missing_model`、`runtime_init_failed`、`unsupported_input`。

> 覆盖范围的实际情况：`inference_bench_cli` **只覆盖 `opencl`**（且自带一套手搓的 `backendRequested/Effective/Reason/Evidence`，与 `AccelContractStatus` 无类型级复用）；`mpp` 在 Windows 恒为 `unsupported_platform`，`qualcomm` 因项目内无 `deps/qualcomm_snpe/` 恒为 `missing_dependency`。
>
> 契约**已有 CI 行为测试**（`tests/cpp/test_acceleration_contract.cpp` 编入 `core_unit_tests`，每次 push 都跑；`test_engine.cpp` 另断言模型缺失时 `missing_model` 且零轨迹）；但**缺规格层**——无 JSON Schema、无 7 个固定码的穷尽往返测试，且"`requested ≠ effective` 即拒绝"仅存在于文档约定，无代码强制。

### 其他约定入口

- **Windows 配置**：`%APPDATA%\rk_wcfr\config.json` 为事实源（JSON Schema 见 `docs/windows-web-spa/config.schema.json`）；`config/windows_camera_face_recognition.ini` 仅供迁移。
- **日志**：Android 用 `adb logcat | grep rk3288_opencv`；Windows 默认写 exe 同级 `storage/win_logs/`，可由配置项 `log.logDir` 覆盖。
- **RK3288 目标**：`armeabi-v7a`、Cortex-A17、必须 NEON（`-mcpu=cortex-a17 -mfpu=neon-vfpv4`）。实际无可用硬件加速，走 CPU 回落。
- **Java 源码**：`../src/java`（`app/build.gradle` 的 `sourceSets.main.java.srcDirs`）；**Java 单测**：`tests/unit/java/`（`sourceSets.test.java.srcDirs`）。
- **测试夹具**：`tests/fixtures/mock/` 存放损坏 / 不完整图片，用于 preflight guard 测试。
- **CivetWeb** 1.16 位于 `src/win/third_party/civetweb/`，仅为未编入目标的第三方源码与许可证快照；Windows 本地服务由自研 Winsock `HttpFacesServer` 提供 REST API 与静态托管。
- **OpenCV 以子目录方式构建**：不经 `find_package`，CMake 直接 `add_subdirectory("${OPENCV_ROOT}" ...)`。
- **libyuv**：CMake 默认 Android=ON / Host=OFF，但 `app/build.gradle` 显式传 `-DRK_ENABLE_LIBYUV=OFF`，故标准 Android Gradle 构建不会拉取 libyuv。
- **Qualcomm SNPE**：可选；头文件不在 `deps/qualcomm_snpe/include` 或 `$QUALCOMM_HOME` 时回落 CPU。
- **OpenAPI 规范**：`docs/windows-web-spa/openapi.yaml`。

## INT8 quantization pipeline

- **脚本**：`scripts/quantize_ncnn_int8.py` —— 两步流程（ncnn2table → ncnn2int8），支持 `--table-only` / `--quant-only` 分离执行；`--calib-dir` 在完整流程或 `--table-only` 时必填。
- **模型**：量化脚本与 CI 使用 SCRFD（检测，经 PNNX 自 `det_10g.onnx` 转换）、ArcFace（识别）、MobileFaceNet（轻量）。目录名为 `scrfd`；C++ INT8 结构 / 精度测试仍引用历史 `yolo_face` 目录，**迁移完成前不得假定路径已统一**。
- **模型二进制不入库**（`models/*` 排除 `.gitkeep`），需用量化脚本本地生成。
- **校准图片**：172 张人脸图，源为 `deps/WIDER_train/`（实际路径 `deps/WIDER_train/WIDER_train/images`），落地副本在 `models/calib_images/`（同为 172 张）。`deps/WIDER_train/`、`deps/insightface/` 内容已 gitignore（仅跟踪 `.gitkeep`），需另行下载 WIDER Face 数据集。
- **精度测试**：`tests/cpp/test_int8_quantization.cpp` 内的 **8 个**结构化用例带 `fileExists()` 跳过——模型缺失时直接通过。全量推理对比由独立目标 `ncnn_precision_test` 承担，ArcFace 余弦相似度目标 ≥ 0.90。
- **CRT**：项目默认静态 CRT（`/MT`，由 CMake 顶层统一设置）。官方 ncnn `windows-vs2022` 预编译包是**动态 CRT（/MD）**，链入本项目的 `/MT` target 会报未解析的 `__imp_*` 符号（fesetround / roundf / erfcf 等）。**变通方案（本机已验证）**：自行以 `/MT` 重建 ncnn（如 `D:/ProgramData/NCNN/ncnn-20260113-mt`，`NCNN_VULKAN=OFF`），再把 `NCNN_DIR` 指向它。`ncnn_precision_test` 自身亦强制静态 CRT。
- **SCRFD → PNNX**：ncnn2int8 无法处理 `splitncnn` 节点（多尺度检测头）；只有 PNNX 直出（optlevel=2）的图才适合 INT8 量化。
- **Android**：INT8 测试仅结构化（fileExists 跳过）；`ncnn_precision_test` **不在 Android 构建**。

## PR 合并规则

合并任何 PR 到 `master` 之前：

1. **审查 comments** —— 逐条阅读 PR 的 review 与 issue comments，全部解决或明确忽略后方可合并。
2. **解决冲突** —— `git merge origin/master`，所有冲突必须解决。
3. **等待 CI** —— CI 必需检查全部通过（见上方 CI pipeline 一节）。
4. **合并方式** —— 仅用 **squash and merge**；禁止 merge commit 与 rebase merge。
5. **删除已合并的源分支**。
6. **不绕过分支保护** —— 不修改分支保护规则；`--admin` 仅限紧急修复使用。

## Conventions

- 全仓 UTF-8（Gradle `options.encoding`、MSVC `/utf-8`）
- C++17 标准（`CMAKE_CXX_STANDARD 17`）
- 算法关键逻辑、JNI 边界、硬件差异处理**必须写中文注释**
- 其他 AI agent 工作目录（`.trae/specs/`、`.Jules/`、`.codegraph/`）为草稿历史，非权威文档

## Scripts & tools

| Script | Purpose |
|:---|:---|
| `scripts/clean-repo-junk.js` | 仓库卫生扫描 |
| `scripts/docs-sync-audit.js` | 文档同步门禁（事实漂移 + 镜像一致性） |
| `scripts/quantize_ncnn_int8.py` | INT8 模型量化（ncnn2table → ncnn2int8） |
| `scripts/run-web-e2e.ps1` | Web SPA 全量 Cypress E2E + 覆盖率 |
| `scripts/bench_camera_adb.ps1` / `.sh` | Android 相机基准（经 ADB） |
| `scripts/stability_switch_50_adb.ps1` | Android 前后台切换稳定性测试（50 轮） |
| `scripts/check-raii-violations.sh` | RAII 契约违规扫描（未接入 CI，手动运行） |

## Key documents

- **DEVELOP.md** —— 模块详解、构建变体、加速架构
- **CREDITS.md** —— 完整依赖清单 + 模型清单
- **README.md** —— 项目概览、快速开始、todo 状态
- **docs/architecture/ARCHITECTURE.md** —— 5 条架构契约（含未落地状态表）
- **docs/windows-web-spa/openapi.yaml** —— REST API 规范
- **docs/windows-web-spa/config.schema.json** —— Windows 配置 JSON Schema
- **docs/specs/2026-07-03-triple-refactor-design.md** —— 三层重构设计文档
- **docs/plans/2026-07-03-triple-refactor-plan.md** —— 三层重构实施计划

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
