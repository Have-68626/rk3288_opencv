<!-- DOCSYNC_DOC
type=runbook
status=living
as_of=2026-09-23
-->

﻿# 部署与回滚指南（Windows 本地服务 + 浏览器 SPA）



## 运行方式（推荐）
1) 启动本地服务（无 UI）
- 可执行文件：`win_local_service.exe`
- 监听地址：`127.0.0.1:<port>`

2) 打开浏览器访问
- `http://127.0.0.1:<port>/`（SPA）
- `http://127.0.0.1:<port>/api/v1/health`（健康检查）

## 前端构建与静态托管
- 前端源码：`web/`
- Vite 构建输出目录：`src/win/app/webroot/`
- CMake 在构建 Windows 二进制后，会把 `src/win/app/webroot/` 复制到 exe 同级 `webroot/`，作为本地 HTTP 服务的静态资源根目录。

## 配置与数据目录
- 配置文件：`%APPDATA%\rk_wcfr\config.json`（支持热重载）
- 密钥文件：`%APPDATA%\rk_wcfr\config.key.dpapi`（DPAPI 保护）
- 配置备份：同目录 `config.json.bak` 与 `.bak.1` 至 `.bak.5`
- 坏配置证据：同目录 `config.json.corrupt.<毫秒时间戳>` —— 启动时 `config.json` 存在但无法装载（不可读 / JSON 解析失败 / schema 被拒）时，坏文件被改名另存于此（服务随之以默认值启动；若隔离失败则保留原文件不覆盖）。该类文件不会被自动清理，需人工处置。
- `ErrorLog/` 是仓库内人工收集用户报错与复现材料的位置，不是本地服务自动生成的运行时数据目录。

## 运行期所需文件清单（部署机须手工布置）

⚠️ `install(TARGETS win_local_service RUNTIME DESTINATION bin)`（`cmake/windows.cmake` 的
`── Install targets ──` 段）**只安装可执行文件**。开发机构建另在 POST_BUILD 阶段把 `webroot/`、
`assets/`、`config/` 与 `models/`（若源树存在）拷到 exe 同级（`cmake/windows.cmake` 中
`TARGET win_local_service POST_BUILD` 的四段拷贝）——**部署机拿不到这些拷贝**，
必须人工布置；否则服务会以「找不到级联 / 无前端 / 无模型」的降级状态启动。

⚠️ 但本清单此前**只列数据/资源目录，完全不含任何 DLL** —— 而 DLL 缺失是比「降级状态启动」
更硬的一种失败：**进程根本不启动**。本仓以 `add_subdirectory` 从源码构建 OpenCV，
`cmake/opencv.cmake` 设 `BUILD_SHARED_LIBS ON` 且 `BUILD_opencv_world OFF` ⇒ 模块是
**分离 DLL**，必须与 exe 一同布置（见下方清单首行）。
（本文件**优先**写符号（函数 / 类 / 常量名），**不写行号**：本仓行号随改动持续漂移——同一批改动
即可让 `src/win/src/WinJsonConfig.cpp` 的规模变动数百行——写行号等于制造下一处失真。
极少数确需行号处（如下方 `webroot/` 一行的 `HttpFacesServer.cpp:203`）一律就地标注「核实时点」；
行号一旦对不上，以符号为准。）

以 exe 同级目录（记作 `<exeDir>`）为基准：

| 路径 | 必需性 | 内容与缺失后果 |
|---|---|---|
| **OpenCV 运行时 DLL**（`opencv_core*.dll` 等）+ 其第三方依赖 DLL | **必需**（否则**进程不启动**） | 本仓 CMake **没有任何 `.dll` 拷贝或 install 规则**：`CMakeLists.txt` 与全部 `cmake/` 模块对 `.dll` / `RUNTIME_DLLS` / `RUNTIME_DEPENDENCIES` / `IMPORTED_RUNTIME_ARTIFACTS` 零命中，`install(TARGETS win_local_service …)` 只装 exe。因 `BUILD_SHARED_LIBS ON` + `BUILD_opencv_world OFF`，需布置的是一组**分离模块 DLL**（`opencv_core` / `opencv_imgproc` / `opencv_objdetect` / `opencv_dnn` / `opencv_face` / `opencv_videoio` / `opencv_imgcodecs` / `opencv_video` / `opencv_calib3d` / `opencv_features2d` / `opencv_flann` / `opencv_ml` / `opencv_photo` / `opencv_highgui` / `opencv_stitching`）**及它们各自的第三方依赖**（libjpeg-turbo / libpng / zlib / openjpeg / libwebp / `opencv_videoio_ffmpeg*.dll` 等，落地名随 OpenCV 版本与 `BUILD_LIST` 而变，须以实际构建产物为准）。缺失任一个 ⇒ Windows 加载器在**进程启动前**报「找不到 opencv_core*.dll」⇒ **不是**上段那种「以降级状态启动」，而是**服务根本起不来**。放在 exe 同级目录或 `PATH` 均可（加载器搜索顺序）；`opencv_face` 来自 `OPENCV_CONTRIB_ROOT`，未设该变量时构建本身不含它，但识别器改用 LBPH 时仍需要。补充（避免相反方向的夸大）：`cmake/opencv.cmake` 的 `add_subdirectory("${OPENCV_ROOT}" …)` **未带 `EXCLUDE_FROM_ALL`**，故 OpenCV 自带的 install 规则会一并进入本工程——若执行 `cmake --install`，OpenCV 的 DLL 落点由其自身规则决定、**不由本仓库保证**；手工布置时一律以实际构建产物为准。 |
| `assets/lbpcascade_frontalface.xml` | **必需** | 默认 `recognition.cascadePath`（`WinJsonConfig.cpp` 的 `defaultAppConfig()`）。缺失 ⇒ LBPH 检测/识别不可用。仓库内源文件为 `app/src/main/assets/lbpcascade_frontalface.xml`。 |
| `webroot/` | **必需**（需要浏览器界面时） | SPA 静态资源根（`HttpFacesServer.cpp:203`，**核实时点 2026-09-23**：该行当前为 `docRoot_ = resolvePathFromExeDir(L"webroot");`；行号漂移时以符号 `docRoot_` 的赋值为准；`pnpm -C web build` 产物，见「前端构建与静态托管」）。缺失 ⇒ 静态读取 fail-closed，`/` 返回 404（仅 REST API 可用）。 |
| `config/windows_camera_face_recognition.ini` | 可选（迁移源） | 旧 INI 迁移来源；新部署以 `%APPDATA%\rk_wcfr\config.json` 为唯一事实源，可不放。 |
| `storage/` | 运行时自动创建，**权限须受限** | 数据库 + 日志 + 默认 DNN 模型目录，详见下方「敏感数据」。 |
| `models/` | 可选 | 默认 `recognition.arcFaceModelPath` = `models/arcface_w600k_r50.onnx`（`WinJsonConfig.cpp` 的 `defaultAppConfig()`）。该权重**不入库**、须自行获取；缺失 ⇒ ArcFace 识别路径不可用（回落 LBPH）。 |
| `%APPDATA%\rk_wcfr\` | 运行时自动创建 | `config.json` / `config.key.dpapi` / `config.json.bak*` / `config.json.corrupt.*`，见「配置与数据目录」。 |

### `storage/` 的内容与敏感数据须知

`storage/` 下的默认路径（均在 `<exeDir>` 下解析，见 `WinJsonConfig.cpp` 的 `defaultAppConfig()`）：

- `storage/win_face_db.yml` —— 人脸特征库（**AES-256-GCM 加密容器**）；
  同名 `storage/win_face_db.yml.key.dpapi` 为 DPAPI 保护的密钥；
  迁移成功后残留的 `storage/win_face_db.yml.pre-encrypt` 是**完整明文库副本**（有意保留的回滚点），
  须由运维人工删除。规格见 `docs/security/face-db-encryption.md` §2–§3。
- `storage/win_logs/` —— 结构化识别日志目录（`log.logDir` 默认值，见 `WinJsonConfig.cpp` 的 `defaultAppConfig()`）。
  该目录下**实际只有三份文件集**，另有各自的滚动档位（`<文件名>.1` … `<文件名>.N`，最新在 `.1`）：
  - `recognition.csv` / `recognition.jsonl`（`StructuredLogger`）——**含明文 `person_id`**（PII），
    且**不经过**任何脱敏函数（写入链只有 `escapeJson` / `escapeCsv`，仅转义、不掩码）；
  - `event_log.jsonl`（`EventLogger`）——本批起与前者共用同一 `log.maxFileBytes` / `log.maxRollFiles`。
  ⚠️ **更正旧说法**（原文：本目录「两个日志链共用」——`rklog::` 常规日志 + 结构化识别日志）：
  **本目录没有 `rklog::` 日志**。`src/win/` **根本未接入 rklog**（全仓 `src/win/` 对 `rklog` 只命中
  `WinJsonConfig.cpp` 里那句「本服务未接入 rklog」的注释，无任何 `rklog::logXxx` 调用），而 rklog 的
  文件落盘需先调 `rklog::setLogDirs`（`src/cpp/src/NativeLog.cpp`），其调用方只有 Android JNI
  （`src/cpp/jni/config.cpp`）与两个测试 ⇒ **Windows 服务下不产生任何 rklog 文件**。
  连带地，落盘面的记账口径（`WinConfig.h` 的 `kLogFileSetCount`、`WinJsonConfig.cpp` 的钳制告警文案）
  是**三份文件集**、已如实含 `event_log.jsonl`，**不含** rklog，无需再加一份。
- `storage/models/` —— 默认 DNN 模型路径（`storage/models/opencv_face_detector_uint8.pb` 与
  `.pbtxt`；缺失时 DNN 检测不可用）。

⚠️ **PII 处置要求**：`storage/win_logs/` 含明文 `person_id`，敏感级别与
`storage/win_face_db.yml.key.dpapi`、`%APPDATA%\rk_wcfr\config.key.dpapi` 相同。
部署时须把该目录 ACL 限制为服务账户与管理员；**备份与排障外发前须脱敏**。
掩码策略本身尚未拍板，完整事实与决策依据见 `docs/security/face-db-encryption.md` §7（FACE-LOG-002）。

## 端口调整
- 推荐通过后端 settings 修改：`PUT /api/v1/settings` 写入 `http.port`
- 配置轮询应用变更后，服务会停止旧 HTTP 监听并尝试在新端口启动；客户端需改用新端口重新连接。
  实现见 `src/win/app/win_local_service_main.cpp` 的主循环（`settings.pollReloadOnce` 后按
  `generation` 判定，`http.enable` / `http.port` 变化时 `http.stop()` 再 `http.start()`）。
  ⚠️ `http.start()` 与 `http.stop()` 同在**该变更分支内**——只点到 `http.stop()` 的引用会漏掉
  自己点名的重启动作，故此处只写符号、不写行号区间（行号区间正是漏掉 `http.start()` 的成因）。
  ⚠️ 该端点本身的监听端口若被改掉，**旧的 health/settings 地址立即失效**，无法再从旧端口「改回去」；
  此时需改 `%APPDATA%\rk_wcfr\config.json` 后重启服务。

## IE11（可选降级）
- Ant Design 5 与现代构建产物不支持 IE11。
- 方案：HTML 级别检测 IE11 并跳转到静态降级页 `ie11.html`（提供提示与引导）。

## 回滚到旧版 Win32 UI
旧版 Win32 UI 仅作为回滚目标保留（默认不构建）。

1) CMake 打开旧 UI 构建开关：
- `-DRK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI=ON`

2) 构建 target：
- `win_camera_face_recognition`

3) 运行旧 UI：
- `win_camera_face_recognition.exe`

