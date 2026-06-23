# 致谢与许可证（CREDITS）

**更新日期**: 2026-06-23

本文件用于记录本项目使用的第三方依赖、来源与许可证信息，便于审计与合规。

## 📑 快速索引

| 平台 | 章节 |
|:----|:-----|
| 跨平台 | [📦 跨平台依赖](#-跨平台依赖) |
| Android | [🤖 Android 依赖](#-android-依赖) |
| Windows | [🖥️ Windows 依赖](#️-windows-依赖) |
| 模型 | [📊 模型台账与依赖](#️-模型台账与依赖) |
| 检查 | [📋 依赖状态检查](#-依赖状态检查) |
| 模板 | [📝 外部文档模板](#-外部文档模板) |

---

## 📦 跨平台依赖

### 构建工具链

| 依赖 | 版本 | 用途 | 许可证 |
|:-----|:-----|:-----|:-------|
| CMake | 3.22.1+ | 跨平台构建配置 | BSD 3-Clause |
| Gradle | 9.0（内置 gradlew） | Android 构建系统 | Apache 2.0 |
| Android NDK | 27.0 | Native 编译 | Android SDK License |
| Node.js + pnpm | 22.17.1+ | Web 前端构建 | MIT |

### 跨平台库

| 依赖 | 版本 | 用途 | 许可证 |
|:-----|:-----|:-----|:-------|
| **OpenCV** | 4.10.0 | 图像采集、处理、特征提取 | Apache 2.0 |
| **libyuv** | — | YUV/RGB 格式转换与缩放 | BSD 3-Clause |
| **ncnn** | 20260113 | 端侧推理后端 | BSD 3-Clause |

---

## 🤖 Android 依赖

### 图像处理与推理

| 依赖 | 用途 | 许可证 | 状态 |
|:-----|:-----|:-------|:-----|
| OpenCV 4.10.0 | 图像处理核心 | Apache 2.0 | ✅ 已集成 |
| ncnn | 端侧推理加速 | BSD 3-Clause | ✅ 已下载 |
| Rockchip MPP | 硬件解码加速 | Apache 2.0 | ⚠️ 可选（自动回退） |
| Qualcomm SDK | 推理加速 | — | ⚠️ 可选（未检测到） |
| FFmpegKit | RTMP 推流 | LGPL 3.0 / GPL 3.0 | ⚠️ 可选（未集成） |

### UI 组件

| 依赖 | 版本 | 用途 | 许可证 |
|:-----|:-----|:-----|:-------|
| AndroidX AppCompat | 兼容性支持 | Apache 2.0 | ✅ |
| Material Components | Material 风格 UI | Apache 2.0 | ✅ |
| CameraX | 相机采集 | Apache 2.0 | ✅ 1.3.4 |
| AndroidX ConstraintLayout | 布局系统 | Apache 2.0 | ✅ |

### 传递依赖说明

CameraX 的传递依赖（已解析）：
- `androidx.concurrent:concurrent-futures:1.1.0` → Apache 2.0
- `com.google.guava:listenablefuture:1.0` → Apache 2.0
- `androidx.lifecycle:lifecycle-*:2.6.1` → Apache 2.0

---

## 🖥️ Windows 依赖

### Native 层

| 依赖 | 用途 | 许可证 |
|:-----|:-----|:-------|
| OpenCV 4.10.0 | 图像处理核心 | Apache 2.0 |
| ncnn | 推理加速 | BSD 3-Clause |
| Microsoft Windows SDK | 摄像头采集（Media Foundation）、Win32 UI | Microsoft License |

### HTTP 服务

| 依赖 | 版本 | 用途 | 许可证 | 位置 |
|:-----|:-----|:-----|:-------|:-----|
| [CivetWeb](https://github.com/civetweb/civetweb) | 1.16 | 本地 HTTP 服务（REST + 静态托管） | MIT | `src/win/third_party/civetweb/` |
| └─ MD5 实现 | — | L. Peter Deutsch 实现 | zlib | `src/win/third_party/civetweb/src/md5.inl` |

### Web 前端

| 依赖 | 用途 | 许可证 |
|:-----|:-----|:-------|
| React / ReactDOM | UI 框架 | MIT |
| Ant Design | UI 组件库 | MIT |
| React Router | 路由管理 | MIT |
| Vite | 构建工具 | MIT |
| Cypress | E2E 测试 | MIT |

---

## 📊 模型台账与依赖

本项目使用或支持以下机器视觉模型。对于需要在部署时下载的模型，请确保计算其 SHA-256 哈希值与下表一致，以确保安全性与精度。

| 模型名称 | 用途 | 格式 | 部署路径 | 来源 | SHA-256 | 许可证 |
|:---------|:-----|:-----|:---------|:-----|:--------|:-------|
| **LBP Frontal Face** | 人脸检测（级联） | XML | `app/src/main/assets/lbpcascade_frontalface.xml` | [OpenCV Data](https://github.com/opencv/opencv/tree/master/data/lbpcascades) | `529f217132809f287aaed5cd35dc00d9bc9b2afebe46dd1fe90ecb67f1daad0d` | 3-Clause BSD |
| **ResNet SSD Face** | 人脸检测（DNN） | PB | `storage/models/opencv_face_detector_uint8.pb` | [OpenCV 3rdparty](https://github.com/opencv/opencv_3rdparty/raw/dnn_samples_face_detector_20180205_fp16/res10_300x300_ssd_iter_140000_fp16.caffemodel) | ⚠️ 需在部署时验证 | Apache 2.0 |
| **ResNet SSD Config** | 网络结构定义 | PBTXT | `storage/models/opencv_face_detector.pbtxt` | [OpenCV Extra](https://raw.githubusercontent.com/opencv/opencv_extra/master/testdata/dnn/opencv_face_detector.pbtxt) | ⚠️ 需在部署时验证 | Apache 2.0 |
| **NCNN YOLO Face** | 端侧检测 | BIN/PARAM | `models/yolo_face_ncnn/` | 自训练/转换 | ⚠️ 需本地生成 | — |
| **NCNN SCRFD** | 端侧检测（轻量） | BIN/PARAM | `models/scrfd_ncnn/` | PNNX 转换自 `det_10g.onnx` | ⚠️ 需本地生成 | — |
| **ArcFace Embedder** | 人脸特征提取 | ONNX/BIN | `models/arcface_ncnn/` | 第三方/自训练 | ⚠️ 需本地生成 | — |
| **MobileFaceNet** | 轻量人脸特征提取 | BIN/PARAM | `models/mobilefacenet_ncnn/` | 自训练/转换 | ⚠️ 需本地生成 | — |

> **提示**：启动时程序将自动进行自检，并会在日志中打印所有加载模型的 SHA-256。请确保与上述台账保持一致。

> **⚠️ 重要**：DNN 模型文件由于体积较大，默认不包含在代码库中。Windows 环境首次部署时，需将 `.pb` 与 `.pbtxt` 文件下载并放置于 `storage/models/` 目录下（或通过 Web UI 的 `/api/v1/settings` 接口修改 `dnn.modelPath` 配置）。

---

## 📋 依赖状态检查

### ❌ 阻断性问题（需手动补充）

| 缺口 | 影响范围 | 参考位置 |
|:-----|:---------|:---------|
| **DNN 模型文件缺失** | Windows 人脸检测（DNN 后端） | 见上方"模型台账"，下载后放置于 `storage/models/` |
| **Windows CMake HDF5 冲突** | Windows 原生构建（可选） | 方案 A：重建 x64 生成器；方案 B：`BUILD_opencv_hdf=OFF` |

### ⚠️ 可选组件（缺失时自动回退）

| 依赖 | 用途 | 回退方案 |
|:-----|:-----|:---------|
| Qualcomm SDK | 推理加速 | 自动使用 CPU 推理 |
| RK MPP 头文件 | 硬件解码 | 自动使用 CPU 解码 |
| FFmpegKit AAR | RTMP 推流 | 禁用推流功能（不影响核心识别） |

---

## 📝 外部文档模板

### Linux Kernel 配置模板（RK3288 BSP）

| 文件 | 用途 | 来源 | 许可证 |
|:-----|:-----|:-----|:-------|
| `docs/bsp/defconfig/rk3288_defconfig` | 内核配置快照 | Linux Kernel 4.4.143 导出 | GPL-2.0 |
| `docs/bsp/kernel-config/kernel.config` | 内核配置参考 | 设备运行态快照 | GPL-2.0 |

> **说明**：以上文件仅用于合规审计与基准对齐参考，不参与实际编译。

---

## 备注

- 如新增/升级/替换依赖，请同时更新本文件，并在 `DEVELOP.md` 中记录变更路径与影响面。
- 如需精确复核 Android 传递依赖，请运行：`.\gradlew.bat :app:dependencyInsight --configuration debugRuntimeClasspath --dependency <artifact>`
