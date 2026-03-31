# RK3288 目标设备画像与约束清单

本文用于把“目标设备画像（可复现）”与“工程硬约束（不可突破）”固化为统一口径，供选型、实现与验收使用。

## 0. 目标设备画像（Target Device Profile）
以下画像基于已知目标设备信息（RK3288 工控机、Android 7.1.2、2GB RAM、8GB 存储、仅 CPU+GPU、前置摄像头 1 路）整理；其余字段可在设备上用第 2 节命令补齐。

### 0.1 硬件（已知）
- **SoC**：Rockchip RK3288
- **CPU**：四核 ARM Cortex-A17，最高主频约 1.8GHz
  - 架构：ARMv7-A（32 位）
  - 指令集扩展：NEON（必须启用）
- **GPU**：ARM Mali-T764 MP4
  - 图形 API：以 OpenGL ES 3.0 为基线（实际以驱动为准）
  - 通用计算：OpenCL 可能可用，但不能作为强依赖（实际以驱动为准）
- **内存**：2GB RAM
- **存储**：8GB（以 `/data` 可用空间为准）
- **摄像头**：前置摄像头 1 路（以实时预览 + 分析为主）

### 0.2 软件（已知/基线）
- **系统**：Android 7.1.2（常见 API Level 25；以 `getprop ro.build.version.sdk` 为准）
- **ABI 基线**：`armeabi-v7a`（32 位）
- **计算前提**：仅 CPU + GPU；**不依赖 NPU/NNAPI 硬件加速**

### 0.3 运行时画像（需在设备上采集）
- **Camera2 硬件级别**：`LEGACY/FULL/LEVEL_3`（决定 CameraX/Camera2 可用能力与稳定性）
- **相机输出能力**：`YUV_420_888`/`JPEG` 是否可用，720p 是否可用，FPS range
- **GPU/驱动可用性**：OpenGL ES 版本字符串、OpenCL 运行库是否存在
- **内存/存储实际可用量**：`dumpsys meminfo`、`df /data`

## 1. 工程约束清单（Constraints）

### 1.1 ABI / 指令集 / 打包
- **只支持 `armeabi-v7a`**：所有 native `.so` 必须提供 32 位版本；不得假设 `arm64-v8a` 存在。
- **强制 NEON**：所有关键路径（预处理/后处理/矩阵运算）需要确保编译与运行时能走 NEON 优化。
- **避免依赖“需要较新系统”的特性**：Android 7.1.2 为基线，不能引入仅在高版本可用的 API 作为强依赖。

### 1.2 内存预算（2GB 设备上的可控上限）
- **默认内存预算建议**：应用常态运行内存峰值控制在 **≤ 512MB**（给系统与相机栈留余量）。
- **避免大对象抖动**：禁止每帧频繁分配/释放大 `Bitmap/byte[]/Mat`；应复用 buffer，避免隐式拷贝。
- **避免大模型堆叠**：检测 + 识别应尽量串行复用中间张量；多模型并行加载要有明确开关与上限。

### 1.3 摄像头能力（必须按“可探测 + 可降级”设计）
- **必须可枚举能力**：启动自检需产出“摄像头能力报告”（建议写入 `ErrorLog/`）。
- **最低可用口径（建议）**：
  - 分析链路：至少支持 `YUV_420_888` 输出
  - 分辨率：至少支持 1280×720（若不可用则需要降级到 640×480 并明确记录）
  - 帧率：至少存在 15–30 的 FPS range（若不足则进入“降级帧率”）
- **不能假设高级能力**：不能依赖 RAW、Depth、多摄、OIS、手动曝光等能力。

### 1.4 GPU 可用性（必须可选、不可强依赖）
- **渲染基线**：OpenGL ES 3.0（用于预览叠加/绘制等）
- **通用计算**：OpenCL/GL Compute Shader 只能作为“可选加速”，必须存在 **CPU-only** 路径。
- **禁止把 Vulkan 当作前提**：目标设备与驱动栈不保证 Vulkan 可用/稳定。

### 1.5 推理后端与模型格式
- **严禁调用 NPU**：
  - 严禁引入 Rockchip NPU 相关 SDK（如 rknn-api, rknpu-ddk）
  - 严禁引入 `librknn_api.so` 或相关头文件
  - 原因：目标设备不含 NPU，相关调用会导致加载失败或运行时崩溃
- **推理后端策略（主选 + 备选）**：
  - 主选：`ncnn`（CPU 友好；GPU 加速仅作为可选项）
  - 备选：OpenCV DNN（作为回退/对照基线）
- **精度与量化**：
  - 不把 INT8 当作“性能捷径”：无 NPU 时 INT8 通常不能得到硬件加速收益
  - 默认以 FP32 为基线；若引入 FP16/量化，必须同步给出阈值/精度漂移评估与回滚口径

### 1.6 性能目标与默认降级策略（仅 CPU+GPU 前提）
以下为“选型/实现”阶段的默认目标与降级路径；最终以真机基准为准，但降级策略必须先固定口径。

- **稳定性目标**：长时间运行不崩溃、不 OOM；相机断连/服务异常可自动恢复且有限重试。
- **实时性目标（建议初始值）**：
  - 摄像头输入：720p@30fps（允许内部降采样/裁剪）
  - 检测：在可接受画质下尽量达到“可用实时”，以掉帧可控为优先
  - 识别：只对少量人脸执行（默认只识别最大人脸），避免把识别当作每帧必跑

默认降级开关顺序（从“最稳”到“最激进”）：
1. 降低检测输入尺寸（例如 416 → 320 → 256）
2. 降低检测频率（例如每 3 帧一次 → 每 5 帧一次 → 每 10 帧一次）
3. 只识别最大/中心人脸，其余仅标框
4. 关闭关键点/对齐的高成本路径（若有备选实现则切换到轻量对齐）
5. 关闭识别，仅保留检测 + 事件抓拍（进入“检测模式”）
6. 进入“不可用模式”（自检失败/能力不足时），输出明确原因并阻断算法链路

### 1.7 多线程与热设计
- **线程数上限**：计算密集型线程建议不超过 4（与 CPU 核数匹配，避免上下文切换放大抖动）。
- **降频风险**：满载发热明显，需把“性能达标”与“长期稳定”同时纳入验收；必要时在非关键路径主动让出时间片。

## 2. 设备信息采集与验证方式（可复制命令）
以下命令用于把第 0.3 节“需采集字段”补齐，并验证当前设备是否满足第 1 节约束。

### 2.1 ADB 直查（推荐）
```bash
# 1) 系统版本 / API Level
adb shell getprop ro.build.version.release
adb shell getprop ro.build.version.sdk

# 2) ABI（必须包含 armeabi-v7a）
adb shell getprop ro.product.cpu.abi
adb shell getprop ro.product.cpu.abilist

# 3) 设备指纹（用于设备画像归档）
adb shell getprop ro.product.manufacturer
adb shell getprop ro.product.brand
adb shell getprop ro.product.model
adb shell getprop ro.board.platform
adb shell getprop ro.hardware
adb shell getprop ro.build.fingerprint
adb shell getprop ro.build.type

# 4) 内存（总量）
adb shell cat /proc/meminfo | head -n 5

# 5) /data 存储（总量与可用）
adb shell df -h /data
```

### 2.2 GPU/图形能力（以设备驱动为准）
```bash
# OpenGL ES 版本/厂商/渲染器（不同 ROM 输出格式可能不同）
adb shell dumpsys SurfaceFlinger | grep -i -E "GLES|OpenGL|GL_VERSION|GL_RENDERER|GL_VENDOR" || true

# OpenCL 运行库是否存在（仅用于判断“可能可用”，不可作为强依赖）
adb shell ls -l /system/lib/libOpenCL.so /vendor/lib/libOpenCL.so 2>/dev/null || true
```

### 2.3 摄像头能力（建议按项目内口径导出到 ErrorLog）
启动应用后，建议在启动自检阶段把“cameraId / 硬件级别 / YUV&JPEG 尺寸 / FPS ranges”写入 `ErrorLog/`，以便选型与回归对比。
