<!-- DOCSYNC_DOC
type=security
status=living
as_of=2026-08-13
-->

# 外部二进制来源账本（Supply Chain Ledger）

治理任务 **SEC-15（S-SEC-B10-006）**：外部二进制缺少完整来源、许可证、摘要与受控发布链。本账本为字段级台账，保证外部二进制/模型**来源可追溯、摘要可复算、发布链受控**。

**范围与衔接**（引用而非重复既有账本）：
- **ncnn** 账本由 SEC-01 维护于 `CREDITS.md`「ncnn 依赖账本与安全」（版本/来源/许可证/3 个官方资产 SHA-256/已知安全公告处置）。
- **CivetWeb** 账本由 SEC-17 维护于 `src/win/third_party/civetweb/LEDGER.md`（上游 commit 锚点 + 各文件 git blob SHA-256 + 本地补丁集）。
- **Gradle wrapper** 完整性锁定由 SEC-13 维护（`gradle/wrapper/gradle-wrapper.jar.sha256` + `distributionSha256Sum`）。

## 外部二进制清单（字段级台账）

表头：依赖 / 来源 URL / 版本 / 许可证 / 摘要（SHA-256 或可复算方式）/ 发布渠道 / 本地部署状态。

| 依赖 | 来源 URL | 版本 | 许可证 | 摘要（SHA-256 或可复算方式） | 发布渠道 | 本地部署状态 |
|:-----|:-----|:-----|:-----|:-----|:-----|:-----|
| **OpenCV** | https://github.com/opencv/opencv | 4.10.0 | Apache 2.0 | `[部署时验证]` 官方源码归档（tag 4.10.0）下载后 `sha256sum` 校验；构建期 CMake 校验 `OPENCV_ROOT` 内含真实 `CMakeLists.txt` | GitHub Release tag `4.10.0` | 已集成：`OPENCV_ROOT` 指向源码；`deps/opencv/` 仅为 `RK_SKIP_OPENCV` mock stub（`project(mock_opencv)`），非真实 OpenCV |
| **OpenCV-contrib** | https://github.com/opencv/opencv_contrib | 4.10.0（须与 OpenCV 同版本） | Apache 2.0 | `[部署时验证]` 同 OpenCV，`sha256sum` 校验；提供 `opencv_face`（LBPH）模块 | GitHub Release tag `4.10.0` | 已集成：`OPENCV_CONTRIB_ROOT` 指向 contrib 源码，经 `add_subdirectory` 引入 |
| **ncnn** | https://github.com/Tencent/ncnn | 1.0.20260113 | BSD 3-Clause | `[参考 SEC-01]` 3 个官方资产 SHA-256 见 `CREDITS.md`「ncnn 依赖账本与安全」 | GitHub Release tag `20260113`，commit `e956fbf` | 已下载：`NCNN_DIR` 或 `D:/ProgramData/NCNN/ncnn-20260113-android-vulkan`；Windows 侧 `ncnn-20260113-windows-vs2022` |
| **CivetWeb** | https://github.com/civetweb/civetweb | 1.16 | MIT | `[参考 SEC-17]` git blob SHA-256 与本地补丁集见 `src/win/third_party/civetweb/LEDGER.md` | GitHub Release tag `v1.16`，commit `d7ba35b` | 归档快照：`src/win/third_party/civetweb/`，未编入构建目标 |
| **LBP 级联模型** | https://github.com/opencv/opencv/tree/master/data/lbpcascades | 随 OpenCV 4.10.0 | 3-Clause BSD | git blob `529f217132809f287aaed5cd35dc00d9bc9b2afebe46dd1fe90ecb67f1daad0d`（见下方「可复算摘要」） | OpenCV 官方 data 目录 | 已入库：`app/src/main/assets/lbpcascade_frontalface.xml` |
| **SCRFD 检测模型** | https://github.com/deepinsight/insightface（`det_10g.onnx` 经 PNNX optlevel=2 转换） | `[未定]` 未固定（随上游 det_10g） | 代码 Apache-2.0（Open-MMLab）/ 模型权重：insightface 非商用（仅限非商业研究，见 `CREDITS.md`「⚠️ 商用限制结论（必读）」） | `[部署时生成]` 运行 `scripts/quantize_ncnn_int8.py` 产出后，对 `models/scrfd_face_detect_ncnn/` 与 `models/int8/` 下 `.param`/`.bin` 执行 `sha256sum`（启动自检日志亦打印模型 hash） | InsightFace 上游发布 / 本仓库 PNNX 转换产物 | gitignored，本地生成（`models/`，仅 `.gitkeep` 入库） |
| **ArcFace 识别模型** | https://github.com/deepinsight/insightface（LResNet100E-IR 风格，经 PNNX 转换） | `[未定]` 未固定 | 代码 MIT / 模型权重：insightface 非商用（仅限非商业研究，见 `CREDITS.md`「⚠️ 商用限制结论（必读）」） | `[部署时生成]` 同 SCRFD，对 `models/arcface_ncnn/`、`models/arcface_int8_ncnn/` 执行 `sha256sum` | InsightFace 上游发布 / 本仓库转换产物 | gitignored，本地生成 |
| **MobileFaceNet 模型** | `[未定]` 仓库标注「自训练/转换」，无上游 URL 可取证（BLOCKED_DATA） | `[未定]` | `[未定]` | `[部署时生成]` 同 SCRFD，对 `models/mobilefacenet_ncnn/`、`models/mobilefacenet_int8_ncnn/` 执行 `sha256sum` | `[未定]` 需人工确认 FP32 来源 | gitignored，本地生成 |
| **YOLO Face 模型** | `[未定]` 仓库标注「自训练/转换」，无上游 URL 可取证（BLOCKED_DATA） | `[未定]` | `[未定]` | `[部署时生成]` 同 SCRFD，对 `models/yolo_face_int8_ncnn/` 执行 `sha256sum` | `[未定]` 需人工确认 FP32 来源 | gitignored，本地生成 |
| **FFmpegKit AAR** | https://github.com/arthenica/ffmpeg-kit | `[未集成]` 未固定 | LGPL 3.0 / GPL 3.0 | `[未集成]` 集成时对 `app/libs/ffmpeg-kit.aar` 执行 `sha256sum` 并记录版本 | GitHub Release | 未集成：`app/libs/` 不存在，RTMP 推流禁用（build.gradle 可选分支） |
| **RK MPP** | https://github.com/rockchip-linux/mpp | `[未随附]`（已知本地路径 `D:/ProgramData/rkmpp/mpp-1.0.11`） | Apache 2.0 | `[未随附]` 引入时按 `deps/rk_mpp/README.md` 记录包校验值 | Rockchip GitHub Release / 官方 SDK | 可选：头文件未随附，CMake 缺失时 CPU 解码回退 |
| **Qualcomm SNPE** | https://github.com/quic/aimet（SNPE 由供应商 SDK 分发） | `[未随附]` | `[供应商专有]` | `[未随附]` 引入时记录 SDK 包校验值 | 供应商 SDK 渠道 | 可选：未检测到头文件，CPU 推理回退 |
| **libyuv** | https://chromium.googlesource.com/libyuv/libyuv | `[未启用]` 未固定 | BSD 3-Clause | `[未启用]` 启用时记录版本与 `sha256sum` | chromium.googlesource（FetchContent） | 未启用：Android 显式 `-DRK_ENABLE_LIBYUV=OFF`（国内网络原因） |

> **「许可证」列注记（勿误判）**：本列同时承载**代码许可**与**模型权重许可**两个法律维度，
> 用 ` / ` 分隔（例：「代码 MIT / 模型权重：insightface 非商用」）。按值匹配 `MIT`/`Apache` 等
> 做自动化时，**不可**把整列当作单一代码许可——须按 `/` 拆分，并识别「模型权重：…」维度。

## 可复算摘要（仓库内 git blob，行尾无关）

规范哈希 = git blob（LF 归一化入库字节）。复算命令：`git cat-file blob HEAD:<路径> | sha256sum`。

| 文件 | SHA-256（git blob） |
|:-----|:-----|
| `app/src/main/assets/lbpcascade_frontalface.xml` | `529f217132809f287aaed5cd35dc00d9bc9b2afebe46dd1fe90ecb67f1daad0d` |

ncnn 官方资产（3 个 zip）SHA-256 与 CivetWeb 各文件 git blob SHA-256 分别见 `CREDITS.md`（SEC-01）与 `src/win/third_party/civetweb/LEDGER.md`（SEC-17），不在本文件重复。

## 部署状态与可复算性小结

- **可复算（仓库内）**：LBP 级联模型（git blob）、CivetWeb 快照（SEC-17）、ncnn 官方资产（SEC-01，本地 `D:/ProgramData/NCNN/`）。
- **部署时生成（gitignored）**：全部 ncnn 模型（SCRFD / ArcFace / MobileFaceNet / YOLO Face 的 `.param`/`.bin`），由 `scripts/quantize_ncnn_int8.py` 产出。复算方式 = 运行脚本后对产物 `sha256sum`；INT8 量化使用校准数据，逐次运行 hash 可能不同，以当次部署产物为准并记录。
- **部署时验证（来源在仓库外）**：OpenCV / OpenCV-contrib 源码归档，官方 Release 下载后校验。
- **未集成 / 未随附 / 未启用**：FFmpegKit AAR、RK MPP、Qualcomm SNPE、libyuv。

## BLOCKED_DATA（来源无法从仓库取证）

- **MobileFaceNet 模型**：仓库仅标注「自训练/转换」，无上游 URL、训练脚本或校验值记录，无法从仓库取证 FP32 权重来源。**不编造来源**。部署前需人工确认并回填来源/版本/许可证。
- **YOLO Face 模型**：同上，来源未定。

## 维护说明

- 任何外部二进制的新增/升级/替换都必须同步更新本账本对应行（来源/版本/许可证/摘要/发布渠道/本地状态）。
- 验证：`node --test tests/scripts/supply-chain-ledger.test.js`（CI 目录级 `node --test tests/scripts/*.test.js` 已接入）。
- 摘要约定与哈希复算方式同 `tests/scripts/civetweb-ledger.test.js`（git blob 为规范值，行尾无关）。
