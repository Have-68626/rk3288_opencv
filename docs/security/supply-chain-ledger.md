<!-- DOCSYNC_DOC
type=security
status=living
as_of=2026-10-16
-->

# 外部二进制来源账本（Supply Chain Ledger）

治理任务 **SEC-15（S-SEC-B10-006）**：外部二进制缺少完整来源、许可证、摘要与受控发布链。本账本为字段级台账，保证外部二进制/模型**来源可追溯、摘要可复算、发布链受控**。

**范围与衔接**（引用而非重复既有账本）：
- **ncnn** 账本由 SEC-01 维护于 `CREDITS.md`「ncnn 依赖账本与安全」（版本/来源/许可证/3 个官方资产 SHA-256/已知安全公告处置）。
- **CivetWeb** 账本由 SEC-17 维护于 `src/win/third_party/civetweb/LEDGER.md`（上游 commit 锚点 + 各文件 git blob SHA-256 + 本地补丁集）。
- **Gradle wrapper** 完整性锁定由 SEC-13 维护（`gradle/wrapper/gradle-wrapper.jar.sha256` + `distributionSha256Sum`）。

## 外部二进制清单（字段级台账）

SCRFD 代码许可证的直接来源为 [InsightFace `detection/scrfd/LICENSE`](https://github.com/deepinsight/insightface/blob/master/detection/scrfd/LICENSE)（2026-10-15 核验，Open-MMLab / Apache-2.0）；本表单独记录模型权重许可。

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
| **MobileFaceNet 模型** | https://github.com/deepinsight/insightface/releases/tag/v0.7（`w600k_mbf.onnx`，buffalo_s/sc） | v0.7；代码 `3e6486942a1be2da0e5b475fac375ea73264bd21` | 代码 MIT / 新候选权重：insightface 非商用，仅限非商业研究 | 父 ONNX `9cc6e4a75f0e2bf0b1aed94578f144d15175f357bdc05e815e5c4a02b319eb4f`；候选与工具摘要见下方 F32 | 用户认可本地父图，PNNX 离线重建新候选链；非远端字节认证 | 仅显式调用、非默认、无自动回落；候选仅cache，未替换models；fp16=1重建件与现存param/bin字节一致 |
| **YOLO Face 模型** | `[未定]` 仓库标注「自训练/转换」，无上游 URL 可取证（BLOCKED_DATA） | `[未定]` | `[未定]` | `[部署时生成]` 同 SCRFD，对 `models/yolo_face_int8_ncnn/` 执行 `sha256sum` | `[未定]` 需人工确认 FP32 来源 | gitignored，本地生成 |
| **FFmpegKit AAR** | https://github.com/arthenica/ffmpeg-kit （用户提供源码 ZIP，未另行比对远端归档字节） | 6.0.LTS 自编译；FFmpeg n6.0、x264、OpenSSL 3.1.1 | 本次启用 x264/GPL，FFmpeg 为 GPL 3.0 或更新版本；OpenSSL Apache-2.0；smart-exception BSD-3-Clause | AAR SHA-256：`07dd0dcffbe52023b614e7455694093e9d17003626247f3ac9361b28aaf57eda`；输入与构建见[记录](../plans/2026-10-16-ffmpeg-kit-build.md) | 本机源码编译，二进制不入库 | 已放入 `app/libs/ffmpeg-kit.aar`；API21+、ARMv7 NEON / ARM64；未作设备加载或真实推流验收 |
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
- **本机自编译（gitignored）**：FFmpegKit AAR，摘要、来源与许可证见上表；构建集成不等于设备推流通过。
- **未集成 / 未随附 / 未启用**：RK MPP、Qualcomm SNPE、libyuv。

## BLOCKED_DATA（来源无法从仓库取证）

- **MobileFaceNet INT8 旧部署件**：下方 F32 已建立指定父图到现存非 INT8 ncnn 件的可复现转换关系；旧 INT8 校准数据、工具和当次命令未在此补证，须用统一预设另行重建并记录，不能沿用旧校准表。
- **YOLO Face 模型**：同上，来源未定。

## F32：MobileFaceNet 指定父链与迁移

2026-10-16，经用户认可，父图固定为本地 `deps/insightface/w600k_mbf.onnx`，SHA256 `9cc6e4a75f0e2bf0b1aed94578f144d15175f357bdc05e815e5c4a02b319eb4f`（13,616,099 字节）。本地 buffalo_s/sc ZIP 成员同摘要；未下载远端权重，不将本地一致性称为远端字节认证。来源与许可见 [CREDITS.md 脚注 ¹](../../CREDITS.md)。固定官方代码的输入为 RGB、mean=127.5、norm=1/127.5、112×112；本次只统一 MobileFaceNet 生产与校准，不改变 ArcFace/SCRFD。

离线工具为本地 `pnnx-20260526-windows.zip`（版本按包名记录），ZIP SHA256 `4e188e7606c887ac550820322f34b144140df877b73292b25e48a2ba38f297df`；提取的 `pnnx.exe` SHA256 `16165dc5fcc53d31f0339b996c15fd2dc26b8d7db43753f72eeab0a0e576116e`。转换时不输入真人、图库或私有身份数据，PNNX 使用指定输入形状的合成数据；产物均在本地缓存目录。

从仓库根设置以下路径并执行（PowerShell；输入父图只读，输出仅cache）：

```powershell
$f32Root = (Get-Location).Path
$f32Out = "$f32Root/cache/audit-fixes-2026-10-13/f32-final/converted"
& "$f32Root/cache/audit-fixes-2026-10-13/f32-final/tools/pnnx.exe" `
  "$f32Root/deps/insightface/w600k_mbf.onnx" `
  'inputshape=[1,3,112,112]' optlevel=2 fp16=0 device=cpu `
  "pnnxparam=$f32Out/w600k_mbf.pnnx.param" "pnnxbin=$f32Out/w600k_mbf.pnnx.bin" `
  "pnnxpy=$f32Out/w600k_mbf_pnnx.py" "pnnxonnx=$f32Out/w600k_mbf.pnnx.onnx" `
  "ncnnparam=$f32Out/w600k_mbf.ncnn.param" "ncnnbin=$f32Out/w600k_mbf.ncnn.bin" `
  "ncnnpy=$f32Out/w600k_mbf_ncnn.py"
```

| 新候选产物 | 字节数 | SHA256 |
|:---|---:|:---|
| `w600k_mbf.ncnn.bin` | 13593800 | `61175413a1cc9f936cd78e007668e4f55173e5e79f57d79044251c931570f725` |
| `w600k_mbf.ncnn.param` | 9710 | `2639653a8dc037167f6380932634b0c94a76700ce1c6c4050af6332c7bb3259b` |
| `w600k_mbf.pnnx.bin` | 13616430 | `c07eae061b7a13786c80d3d85f432a24e2b8cc12be6c73ee180870d1d79579ce` |
| `w600k_mbf.pnnx.onnx` | 13615924 | `665b16d00cfb025670c54dc3722ea25c8cae4164847d9e7c55c1bbbfaaf9ac45` |
| `w600k_mbf.pnnx.param` | 20933 | `b7a2daa39f04cc3cdd1385224726c1eed4a17bd786605aba26e3e3ebe0f2ee81` |
| `w600k_mbf_ncnn.py` | 823 | `175c01a78ac748b0709eaf5013d1256a0469ee5428fae469c9b78b08e42d93b8` |
| `w600k_mbf_pnnx.py` | 31360 | `b71d87119526cb043e55c8ad64e41a9535da5b26da196b979c977a934f03991f` |

上述 `fp16=0` 候选 param 与现存件一致，bin 为 `61175413a1cc9f936cd78e007668e4f55173e5e79f57d79044251c931570f725`。随后另在 `f32-final/converted-fp16/` 使用同一父图、同一工具与同组参数，仅将 `fp16=0` 改为 `fp16=1`，并将全部输出路径改到该独立目录。该次重建的 param SHA256=`2639653a8dc037167f6380932634b0c94a76700ce1c6c4050af6332c7bb3259b`（9,710字节）、bin SHA256=`4cdb441a85243ab3349f3ec7d03745998eb78ad803cfd802a66ade29bb0dda66`（6,835,400字节），**两者均与现存 `models/mobilefacenet_ncnn/mobilefacenet.*` 及 `deps/insightface/w600k_mbf.ncnn.*` 逐字节一致**。由此闭合指定父图到现存件的可复现转换关系，不声称恢复历史实际执行记录；不同 bin 是本次所选权重存储精度不同，不能解读为不同父模型。`fp16=1` 控制权重存储，不代表 INT8 量化。两条转换均未覆盖 `models/`。

INT8 权重须另用统一后的 mobilefacenet 校准预设重建，旧校准表不可复用；本次不把随机合成输入视为真人校准或精度验收。两种候选均经生产适配器对确定性合成彩色图做512维/有限值/单位模长冒烟，未输出嵌入。适配器既有 basename 推导要求单点文件名；冒烟在cache复制为 `converted-adapter/mobilefacenet.param/.bin` 与 `converted-fp16-adapter/mobilefacenet.param/.bin`（字节不变），没有改命名逻辑或生产文件。

实际消费是 `FaceInferencePipeline` 按显式 `embedderId=mobilefacenet/mobilefacenet_int8` 经 `ModelRegistry` 创建适配器；实时 profile 默认 `loadEmbedder` 仍装配 ArcFace，没有新自动回落。旧 mean=0 生成的模板必须在新空间重登记。切换时更新模型/预处理版本（可保留已复现的非 INT8 权重）并使用新非空 `templateSpaceId`，并将外部 `gallerySnapshotSpaceId` 绑定同一值；`FaceInferStages` 已按 ID 拒绝外部图库错配、将磁盘库隔离在 `galleryDir/<templateSpaceId>`。建议发行方对 `mobilefacenet_rgb_127.5_1over127.5_align112_v1`、新 FP32 param/bin 摘要及实际检测/对齐契约组成的固定记录取 SHA256，生成独立64位小写十六进制ID，禁止沿用旧ID或空ID。现有 `composeTemplateSpaceHashInput` 固定为 ArcFace 契约，不能直接当作 MobileFaceNet 发行算法；本次不扩大修改它。直接调用适配器的宿主须自行隔离模板，不存在自动模板迁移/来源鉴别。旧库保留，不删除、不搬迁、不导入。

本次验证范围为真实适配器的合成像素/ncnn小网络预处理与校准argv、离线转换链和候选合成输入冒烟；不代表真人识别精度、RK3288性能或部署验收。完整命令、日志和摘要在本地 `cache/audit-fixes-2026-10-13/f32-final/`，其中 `conversion.json`、`conversion-fp16.json` 保存两条实际绝对命令与全部产物摘要。

## 维护说明

- 任何外部二进制的新增/升级/替换都必须同步更新本账本对应行（来源/版本/许可证/摘要/发布渠道/本地状态）。
- 验证：`node --test tests/scripts/supply-chain-ledger.test.js`（CI 目录级 `node --test tests/scripts/*.test.js` 已接入）。
- 摘要约定与哈希复算方式同 `tests/scripts/civetweb-ledger.test.js`（git blob 为规范值，行尾无关）。
