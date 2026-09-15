<!-- DOCSYNC_DOC
type=reference
status=living
as_of=2026-09-12
-->

# 终端能力矩阵（Android / Windows / Web / CLI）


**状态**: 现状快照（以仓库当前代码与构建脚本为准）。本文回答“每个终端**有什么构建产物、能做什么、边界在哪**”，不表示任何终端已完成真机验收；验收口径见 `docs/guides/rk3288-device-acceptance.md`。

**范围**: Android App、Windows 本地服务、Web SPA、旧 Win32 UI，以及 5 个命令行工具（`rk3288_cli`、`opencv_verify_cli`、`inference_bench_cli`、`win_face_eval_cli`、`win_face_bench_cli`）。

## 1. 总览

| # | 终端 / 入口 | 构建产物 | 构建命令 | 平台 | 状态 |
|---|---|---|---|---|---|
| 1 | Android App | `app/build/outputs/apk/debug/app-debug.apk` | `gradlew.bat --no-daemon :app:assembleDebug` | Android（`armeabi-v7a` + `arm64-v8a`） | 主交付（现场端） |
| 2 | Windows 本地服务 | `build_win\Release\win_local_service.exe` | `cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT=... -DOPENCV_CONTRIB_ROOT=...` → `cmake --build build_win --config Release --target win_local_service` | Windows x64 | 主交付（主控端） |
| 3 | Web SPA | `src/win/app/webroot/`（Vite 产物，入库） | `pnpm -C web install && pnpm -C web build` | 浏览器（Chrome/Edge/Firefox） | 主交付（UI） |
| 4 | 旧 Win32 UI | `win_camera_face_recognition.exe` | `-DRK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI=ON` 后构建该 target | Windows x64 | **默认关闭**，仅回滚保留 |
| 5 | `rk3288_cli` | `rk3288_cli`（主机/Android） | 交叉/主机 OpenCV 构建后构建该 target | 跨平台（含 Android） | 算法与基线调试工具 |
| 6 | `opencv_verify_cli` | `opencv_verify_cli` | 同上 | 跨平台 | 静态图检测校验工具 |
| 7 | `inference_bench_cli` | `inference_bench_cli` | 同上 | 跨平台 | 推理后端基准工具 |
| 8 | `win_face_eval_cli` | `win_face_eval_cli.exe` | Windows 全量构建后 `--target win_face_eval_cli` | Windows only | 识别精度评估工具 |
| 9 | `win_face_bench_cli` | `win_face_bench_cli.exe` | Windows 全量构建后 `--target win_face_bench_cli` | Windows only | 识别吞吐基准工具 |

构建门禁说明：

- 目标 5–7 仅在 `NOT RK_SKIP_OPENCV` 时创建（`CMakeLists.txt`）；`rk3288_cli` 需要 OpenCV，`--yolo-ncnn-*` / ncnn 后端需 `RK_HAVE_NCNN`，libyuv 需 `RK_HAVE_LIBYUV`。
- 目标 2、8、9 位于 `cmake/windows.cmake`，仅当 `WIN32 AND NOT RK_SKIP_OPENCV AND RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION`（默认 **ON**）时纳入构建。
- 目标 4 由独立选项 `RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI` 控制，默认 **OFF**（`CMakeLists.txt` 注释为 rollback only），与上面那个默认 ON 的 `RK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION` 不是同一个开关。
- `core_unit_tests` 可用 `-DRK_SKIP_OPENCV=ON` 在无 OpenCV 环境构建，用于 CI；模型缺失的 INT8 结构测试走 `fileExists()` 跳过。

## 2. 交付终端

### 2.1 Android App（现场端）

- **产物 / 构建**：`app/build/outputs/apk/debug/app-debug.apk`；`gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`。默认只打 `armeabi-v7a` + `arm64-v8a`，`x86_64` 需 `-PRK_ENABLE_X86_64=true`；`splits.abi.enable=false`、`universalApk=false`。
- **入口**：`src/cpp/native-lib.cpp` 与拆分后的 `src/cpp/jni/{engine,camera,config,preview,registry,native_face_bridge}.cpp`（`System.loadLibrary("native-lib")`，`JNINativeMethod` 注册）；Java 入口 `src/java/com/example/rk3288_opencv/MainActivity.java`，前台服务 `StatusService.java`，相机 CameraX 1.3.4。
- **能力**：相机预览、人脸检测/识别、八样本注册会话与加密模板仓库、外部/模拟输入（`nativeConfigureExternalInput`）、运行时加速能力文本（`buildRuntimeCapabilityText`）、可选 RTMP 推流（仅当 `app/libs/ffmpeg-kit.aar` 存在）。
- **限制**：目标机 RK3288 为 32 位 `armeabi-v7a`、Cortex-A17、2GB RAM、Android 7.1.2，**无 NPU/NNAPI**，实际 CPU-only；内存峰值预算 ≤512MB；Gradle 显式传 `-DRK_ENABLE_LIBYUV=OFF`，故不启用 libyuv；**APK 默认不内置模型**（`checkApkFaceModels` 会在 APK 中出现 `assets/face_models/` 时让构建失败）；`ncnn_precision_test` 不在 Android 构建；Java source/target 11。
- **模型策略**：模型由使用者自行获取并经 ZIP 导入，详见 `docs/designs/model-acquisition-import.md`。

### 2.2 Windows 本地服务（主控端）

- **产物 / 构建**：`build_win\Release\win_local_service.exe`（target `win_local_service`，入口 `src/win/app/win_local_service_main.cpp`）；由 `RK_BUILD_WINDOWS_LOCAL_WEB_SERVICE`（默认 ON）门控。
- **能力**：自有 Winsock 的 `HttpFacesServer` 提供 `/api/v1/*` REST 与 `src/win/app/webroot/` 静态托管、MJPEG/SSE 预览、相机采集与人脸识别管线（`FramePipeline`）、结果推送（`HttpFacesPoster`）、JSON 配置热重载与回滚（`WinJsonConfigStore`，按变更分类 Camera/Model/Preview/FullRestart）。
- **配置**：`%APPDATA%\rk_wcfr\config.json` 为唯一事实来源（Schema：`docs/windows-web-spa/config.schema.json`）；旧 INI 仅用于迁移；敏感字段 `poster.postUrl` 以 AES-256-GCM + DPAPI 落盘；`FaceDatabase` 已按 `docs/security/face-db-encryption.md` 做 DPAPI 密钥 + AES-256-GCM 版本化迁移。
- **限制**：仅监听 `127.0.0.1`（不暴露局域网/公网）；需要 OpenCV **源码树** 与 contrib；MSVC x64 / VS2022；OpenCL、ncnn、MPP、Qualcomm SNPE 均为可选，缺失时回退 CPU 并在加速契约中给出 reason。
- **变体**：`win_unit_tests`（+ `win_face_database_perf`）、`win_face_eval_cli`、`win_face_bench_cli`。

### 2.3 Web SPA

- **产物 / 构建**：`source → ../src/win/app/webroot`（Vite `emptyOutDir: true`、`base: './'`），构建产物入库供 Windows 服务托管；`pnpm -C web install && pnpm -C web build`（`tsc -b && vite build`）。
- **开发**：`pnpm -C web dev`（Vite），`/api` 代理到 `http://127.0.0.1:8080`，可用 `VITE_DEV_PROXY_TARGET` 覆盖。
- **页面**：`HomePage`、`SplashPage`、`PreviewPage`、`SettingsPage`（含 `LocalSettingsTab`/`ServerSettingsTab`）；栈为 React 18 + TypeScript 5.9 + Ant Design 5 + react-router 6。
- **能力**：MJPEG 预览、相机/分辨率/FPS、翻转 X/Y、注册 `personId`、清空库、模型状态/重载、设置读写（写 `%APPDATA%\rk_wcfr\config.json` 并热生效）、打开隐私设置、主题/语言/启动页本地偏好。
- **限制**：**没有独立的人脸识别结果页**（`GET /api/faces`、`/api/faces/stream` 后端已就绪，前端待补，见 `docs/windows-web-spa/feature_parity.md`）；主题/语言/API 超时/缓存策略仅存浏览器本地，不写后端；不接触硬件与密钥；E2E 需要运行中的后端。

### 2.4 旧 Win32 UI（回滚保留）

- **产物 / 入口**：`win_camera_face_recognition.exe`，源码 `src/win/app/win_camera_face_recognition_main.cpp`（WIN32 target）。
- **启用**：默认关闭，需 `-DRK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI=ON`。
- **状态**：旧版原生窗口 UI，仅作回归/回滚保留；现行主链路为 `win_local_service.exe` + Web SPA（`docs/windows-web-spa/architecture.md`）。不应作为新功能落点。

## 3. 命令行工具

### 3.1 `rk3288_cli`（跨平台）

- **入口**：`src/cpp/main.cpp`。无子命令时进入实时引擎循环：`rk3288_cli [cameraId|file] [cascade] [storage] [maxFrames]`。
- **子命令**：
  - `--list-models`：打印 `ModelRegistry` 模型表。
  - `--analyze <imagePath> <cascadePath> [outDir]`：Cascade 检测 + 关键点，输出标注图 `annotated_*.jpg`。
  - `--yolo-face <imagePath> --model <model.onnx> [--backend opencv|ncnn] [--out-dir <dir>] [--out-prefix <name>]`：单图检测，写检测 JSON（默认 `tests/metrics`，失败时 `ErrorLog`）。
  - `--face-infer <imagePath> [...]`：单图检测+对齐+识别比对，JSON 到 stdout；支持 `--gallery-dir`、`--topk`、`--threshold`、`--fake-detect`、`--fake-embedding`。
  - `--face-baseline <imagePath|dir> [...]`：性能基线，写 `<stem>_raw.csv`、`<stem>_summary.csv`、`<stem>.md`（默认 `tests/reports/face_baseline`）。
- **限制**：依赖 OpenCV；YOLO/Arc 模式需对应模型；`--fake-*` 允许无模型跑通链路；ncnn 后端编译门控 `RK_HAVE_NCNN`。

### 3.2 `opencv_verify_cli`（跨平台）

- **入口**：`src/cpp/opencv_verify_cli.cpp`，无子命令，纯参数。
- **常用参数**：`--input --cascade --manifest --out --json --vis-dir --orb --scale --neighbors --min --max --nms-iou --param-list --top`。
- **输出**：默认 CSV `opencv_verify_results.csv`，`--json` 汇总，`--vis-dir` 标注 PNG；`--param-list` 做参数网格扫描并额外写 `grid_summary.md` 与逐次 CSV/JSON。
- **限制**：需要 OpenCV 头；无头构建下是 stub，打印 `VERIFY_ERROR opencv_headers_not_found` 退出码 2；cascade 加载失败退出 3。

### 3.3 `inference_bench_cli`（跨平台）

- **入口**：`src/cpp/tools/inference_bench_cli.cpp`，纯参数。
- **参数**：`--backend both|opencv|ncnn|qualcomm`、各后端模型/配置/输入/输出/线程/lightmode、`--w --h --scale --mean-* --swap-rb`、`--warmup --iters --seed --use-opencl`、`--out-dir tests/metrics`、`--out-prefix infer_bench`、`--format both`。
- **输出**：`<prefix>_<ts>.csv` + `.json`（p50/p95、CPU/RSS、OpenCL 设备证据），stdout `BENCH_ENV`/`BENCH_RESULT`。
- **限制**：ncnn 未编入时明确报“当前构建未启用 ncnn”；Qualcomm SDK 缺失打印 `Fallback reason: QUALCOMM_SDK_UNAVAILABLE_SANDBOX` 并继续（不记录）；OpenCV 后端失败为致命（退出 3）且必须给 `--opencv-model`。该项目不依赖 NPU（RK3288 约束）。

### 3.4 `win_face_eval_cli`（Windows only）

- **入口**：`src/win/tools/win_face_eval_cli.cpp`。
- **参数**：`--enroll --probe --cascade --out_csv --out_json --db --min_face --threshold --target_accuracy`。
- **行为**：按人目录注册、逐一探针，输出 `storage/win_eval_results.csv`（ts/gt/pred/accepted/distance/confidence/image）与 `storage/win_eval_summary.json`（混淆矩阵、准确率、调参建议）；准确率低于 `--target_accuracy` 退出 5。
- **限制**：需 cascade XML、LBPH 库（会被清空重写）、OpenCV imgcodecs；无 usage 字符串，未知参数被忽略。

### 3.5 `win_face_bench_cli`（Windows only）

- **入口**：`src/win/tools/win_face_bench_cli.cpp`。
- **参数**：`--input --cascade --db --min_face --threshold --iters`。
- **输出**：不落文件，stdout 打印 `BENCH_SUMMARY images= iters= frames= faces= ms_total= fps= avg_ms= rss_bytes=`（RSS 取自 psapi）。
- **限制**：需 cascade 且至少一张 jpg/png/bmp（无图退出 2 `no_images`；初始化失败退出 3）；无 usage 字符串。

## 4. 共性边界

- **模型不随产品分发**：APK 不内置模型，Windows/CLI 需本地具备模型文件；模型缺失时不得静默回退 LBP，应显式给出 `missing_model`（见 `docs/designs/model-acquisition-import.md` 与加速契约 reason 码）。
- **加速契约**：MPP、ncnn、libyuv、Qualcomm、OpenCL 一律遵循 `requested`/`effective`/`evidence`/`reason`，reason 取固定码 `ok`、`build_disabled`、`unsupported_platform`、`missing_dependency`、`missing_model`、`runtime_init_failed`、`unsupported_input`。
- **平台目标**：RK3288 目标 ABI 为 `armeabi-v7a`、NEON 必需、无 NPU；任何终端不得引入 rknn/rknpu 依赖。
- **测试框架**：`core_unit_tests`/`core_gtest_tests` 用 GoogleTest；`face_infer_unit_tests`/`win_unit_tests`/`ncnn_precision_test` 用自定义 `bool` 测试；新增测试沿用所在 target 框架。
- **相关文档**：`docs/RK3288_CONSTRAINTS.md`、`docs/windows-web-spa/architecture.md`、`docs/windows-web-spa/feature_parity.md`、`docs/guides/windows-service/performance.md`、`DEVELOP.md`、`CREDITS.md`。
