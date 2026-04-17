# 致谢与许可证（CREDITS）

本文件用于记录本项目使用的第三方依赖、来源与许可证信息，便于审计与合规。

## 依赖状态检查清单

### ✅ 已满足（已安装或已在仓库）
| 依赖 | 位置 | 备注 |
|--|--|--|
| OpenCV 4.10.0 | `D:\ProgramData\OpenCV\opencv-4.10.0` | Android/Windows 编译可用 |
| NCNN | `D:\ProgramData\NCNN\ncnn-20260113-*` | 已下载 Windows & Android 版本 |
| Android NDK 27.0 | `D:\ProgramData\AndroidStudioSDK\ndk\27.0.12077973` | Android 编译 ✅ |
| Gradle 9.0 | 内置 `gradlew.bat` | Android 构建 ✅ |
| Node.js 22.17.1 + pnpm | 系统环境 | Web 前端 ✅ |
| CMake 3.22.1 | 系统环境 | 编译配置 ✅ |
| LBP Cascade File | `app/src/main/assets/lbpcascade_frontalface.xml` | Android/Windows 人脸识别可用 |
| RK MPP (可选) | `D:\ProgramData\rkmpp\mpp-1.0.11` | 硬件加速选项，已自动回退 |

### ❌ 缺失（阻断部分功能，需手动补充）
| 缺口 | 影响范围 | 需求操作 | 参考位置 |
|--|--|--|--|
| **DNN 模型文件** | Windows 人脸检测（DNN 后端） | 下载 `.pb` + `.pbtxt` 文件至 `storage/models/` 或通过 Web UI 配置路径 | [Model_Inventory.md](Model_Inventory.md), [config/windows_camera_face_recognition.ini](config/windows_camera_face_recognition.ini) |
| **Windows CMake HDF5 架构冲突** | Windows 原生构建（可选） | 方案 A：重建 x64 生成器目录；方案 B：`-DBUILD_opencv_hdf=OFF` | [CMakeLists.txt](CMakeLists.txt) |

### ⚠️ 可选（缺失时自动回退，不阻断核心功能）
| 依赖 | 用途 | 现状 | 回退方案 |
|--|--|--|--|
| **Qualcomm SDK** | Android Qualcomm 推理加速 | 未检测到 | 自动使用 CPU 推理（CMake 第 48 行警告） |
| **RK MPP 头文件** | RK3288 硬件解码加速 | 已下载但未配置环境变量 | 自动使用 CPU 解码（CMake 第 34 行警告） |
| **FFmpegKit AAR** | Android RTMP 推流功能 | `app/libs/ffmpeg-kit.aar` 不存在 | 禁用 RTMP 推流（可选功能，不影响核心识别） |

## 模型台账与依赖 (Model Inventory)

本项目在运行人脸检测与识别功能时，依赖多个人工智能模型。
详细的模型列表、来源、哈希及下载说明，请参阅单独的 [模型台账 (Model_Inventory.md)](./Model_Inventory.md)。

模型许可证信息：
- **LBP Frontal Face**: 3-Clause BSD / OpenCV License
- **ResNet SSD Face Detector (8-bit)**: Apache 2.0
- **ResNet SSD Config**: Apache 2.0

> **⚠️ 注意：** DNN 模型文件由于体积较大，默认不包含在代码库中。Windows 环境首次部署时，需将上述 `.pb` 与 `.pbtxt` 文件下载并放置于 `storage/models/` 目录下（或通过 Web UI 的 `/api/v1/settings` 接口修改 `dnn.modelPath` 配置）。详见上方"缺失（❌）"清单。

## 核心第三方组件

### OpenCV / opencv_contrib
- 用途：图像采集、处理、特征提取等（Native C++）
- 来源：OpenCV 官方项目
- 许可证：Apache License 2.0

### OpenCV 人脸检测模型与级联文件
- 用途：Windows 摄像头人脸检测（DNN）与测试数据（级联）
- 文件与来源：
  - `app/src/main/assets/lbpcascade_frontalface.xml`（原 `tests/data/lbpcascade_frontalface.xml`）：来源于 OpenCV 数据集（随 OpenCV 发布）
  - `opencv_face_detector_uint8.pb` + `opencv_face_detector.pbtxt`：来源于 OpenCV DNN 示例/模型包（随 OpenCV 发布；用户自行下载并在 ini/env 配置路径）
- 许可证：Apache License 2.0（随 OpenCV 发布）

### ncnn
- 用途：端侧推理后端（可选；RK3288/Android 侧优先方案）
- 来源：Tencent 开源项目 ncnn
- 许可证：BSD 3-Clause License

### libyuv
- 用途：YUV/RGB 像素格式转换与缩放（用于外部帧/JNI 通道性能优化）
- 来源：https://chromium.googlesource.com/libyuv/libyuv
- 许可证：BSD 3-Clause License

### Rockchip MPP
- 用途：RK3288 硬件加速解码与图像处理（按需编译）
- 来源：Rockchip 官方开源组件（本仓库 vendor：`deps/rk_mpp/`，含 headers/libs）
- 许可证：Apache License 2.0（以其官方开源许可为准）

### CivetWeb
- 用途：Windows 本地 HTTP 服务（localhost REST + 静态托管 + OpenAPI）
- 来源：https://github.com/civetweb/civetweb（本仓库 vendor：`src/win/third_party/civetweb/`，版本：v1.16）
- 许可证：MIT License
  - *注意*：附带的 MD5 实现文件 `src/win/third_party/civetweb/src/md5.inl`（由 L. Peter Deutsch 编写）使用 **zlib License**。

### FFmpegKit
- 用途：Android 侧 RTMP 推流（`FfmpegRtmpPusher` 按需动态加载）
- 来源：https://github.com/arthenica/ffmpeg-kit
- 许可证：LGPL 3.0 或 GPL 3.0（取决于用户提供的预编译版本，本项目默认不包含或仅使用其 API 包装）
- 依赖位置：预期置于 `app/libs/ffmpeg-kit.aar` 供 Android Gradle 项目集成

### Web 前端（React + Ant Design + Vite）
- 用途：浏览器 SPA（本地控制台 UI）
- 目录：`web/`（构建产物输出到 `src/win/app/webroot/` 供本地服务托管）
- 依赖与许可证（以各自仓库 LICENSE 为准）：
  - React / ReactDOM：MIT License
  - Ant Design：MIT License
  - React Router：MIT License
  - Vite：MIT License
  - Cypress：MIT License

### 端侧模型（默认不入库，部署侧提供）
- 用途：人脸检测（YOLO）与人脸识别（ArcFace）推理模型（例如：storage/models/opencv_face_detector_uint8.pb）
- 来源与许可证：模型文件默认不随仓库提交；由部署/交付环节提供并登记来源、版本号、用途范围与许可证
- 风险提示：不同模型的许可证与使用限制差异很大；在量产前必须完成审计并在交付文档中固化

### AndroidX AppCompat
- 用途：兼容性支持、基础 UI 组件
- 组件：`androidx.appcompat:appcompat`
- 许可证：Apache License 2.0

### Google Material Components for Android
- 用途：Material 风格 UI 组件
- 组件：`com.google.android.material:material`
- 许可证：Apache License 2.0

### AndroidX ConstraintLayout
- 用途：布局系统
- 组件：`androidx.constraintlayout:constraintlayout`
- 许可证：Apache License 2.0

### AndroidX CameraX
- 用途：Android 相机采集（CameraX ImageAnalysis，可作为 Camera2 的替代/降级路径）
- 发布渠道：Google Maven（`https://maven.google.com/`）
- 上游来源：
  - AndroidX（Jetpack）CameraX Release Notes：`https://developer.android.com/jetpack/androidx/releases/camera`
  - 源码仓库（镜像）：`https://github.com/androidx/androidx`（CameraX 位于 `camera/` 目录）
- 组件（本项目当前版本：`1.3.4`，见 `app/build.gradle`）：
  - `androidx.camera:camera-core:1.3.4`
  - `androidx.camera:camera-camera2:1.3.4`
  - `androidx.camera:camera-lifecycle:1.3.4`
- 许可证：Apache License 2.0
- CameraX 相关传递依赖（基于本项目 `debugRuntimeClasspath` 解析结果）：
  - `androidx.concurrent:concurrent-futures:1.1.0`：Apache License 2.0
  - `com.google.guava:listenablefuture:1.0`：Apache License 2.0
  - `androidx.lifecycle:lifecycle-*:2.6.1`：Apache License 2.0
  - 说明：传递依赖可能随版本升级变化；如需精确复核，请运行 `.\gradlew.bat :app:dependencyInsight --configuration debugRuntimeClasspath --dependency <artifact>`。

## 工具与构建

### Gradle / Android Gradle Plugin
- 用途：构建系统
- 许可证：Apache License 2.0

### Android NDK / SDK
- 用途：Native 编译与 Android 平台开发
- 许可证：按 Android 官方发布条款

## Windows 依赖（系统组件）

### Microsoft Windows SDK（Win32 API / Media Foundation）
- 用途：Windows 摄像头采集（Media Foundation）、Win32 原生窗口 UI、系统能力调用
- 来源：Microsoft Windows SDK（随 Visual Studio / Windows SDK 安装）
- 许可证：按 Microsoft Windows SDK 发布条款（系统组件与 SDK 许可）

## 外部文档与模板

### Linux Kernel 配置模板（RK3288 BSP）
- 用途：记录设备运行态内核配置快照，仅用于合规审计与基准对齐参考，不参与实际编译。
- 文件与来源：
  - `docs/bsp/defconfig/rk3288_defconfig`
  - `docs/bsp/kernel-config/kernel.config`
  - 来源：设备运行态内核快照（基于 Linux Kernel 4.4.143 导出）。
- 许可证：GPL-2.0 License（随 Linux Kernel 发布；仅作为文档存留，不造成衍生污染）。

## 备注
- 如新增/升级/替换依赖，请同时更新本文件，并在 `DEVELOP.md` 中记录变更路径与影响面。

