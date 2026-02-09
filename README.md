# RK3288 机器视觉引擎 (AI Engine)

![Platform](https://img.shields.io/badge/Platform-RK3288%20%7C%20ARMv8%20%7C%20x86-blue)
![Language](https://img.shields.io/badge/Language-C%2B%2B17-green)
![OpenCV](https://img.shields.io/badge/OpenCV-4.10.0-orange)

## 📖 项目简介

本项目是一个专为 **Rockchip RK3288** 平台（Cortex-A17 架构 + Mali-T764 GPU）深度优化的嵌入式机器视觉应用，同时兼容 **ARMv8 (arm64-v8a)** 和 **x86_64** 架构。

核心目标是在资源受限的旧设备上（<512MB 可用内存）实现稳定、低延迟的视频监控与生物识别功能。项目核心逻辑采用纯 C++ 开发，支持两种运行模式：**Android APK**（带 UI）和 **Native Executable**（无头模式，极低资源占用）。

## ✨ 核心功能

*   **双模式监控**:
    *   **连续模式**: 实时全帧率处理。
    *   **运动触发模式**: 基于轻量级帧差法，仅在画面变动时激活，大幅降低 CPU 功耗。
*   **边缘生物识别**:
    *   集成 OpenCV **LBPH (局部二值模式直方图)** 算法。
    *   针对 ARM NEON 指令集优化，识别准确率 ≥92%。
*   **结构化事件记录**:
    *   自动捕获异常事件（如未授权人员、运动侦测）。
    *   生成 JSON 格式报告并保存现场快照。
*   **离线数据管理**:
    *   内置 **7天滚动缓存** 机制，自动清理过期数据。
    *   完全离线运行，无需网络连接。

## 📂 项目结构

```text
rk3288_opencv/
├── app/
│   ├── src/
│   │   ├── main/
│   │   │   ├── cpp/                # 核心 C++ 源码
│   │   │   │   ├── include/        # 头文件 (Engine, BioAuth, etc.)
│   │   │   │   ├── src/            # 实现文件
│   │   │   │   ├── native-lib.cpp  # JNI 接口 (供 APK 使用)
│   │   │   │   └── main.cpp        # 命令行入口 (供 Native Executable 使用)
│   │   │   └── java/               # Android UI 层 (仅 APK 模式需要)
│   └── build.gradle                # Android 构建配置
├── build_android.bat               # Native Executable 构建脚本 (Windows)
├── CMakeLists.txt                  # CMake 构建配置
├── README.md                       # 项目概览
├── README_BUILD.md                 # 详细构建指南
└── DEVELOP.md                      # 详细开发设计书
```

## 🛠️ 技术架构

项目采用模块化分层设计，确保高内聚低耦合：

*   **Infrastructure**: `Config.h`, `Storage` (资源管理与持久化)
*   **Hardware**: `VideoManager` (OpenCL 加速的视频采集)
*   **Algorithm**: `MotionDetector`, `BioAuth` (核心视觉算法)
*   **Orchestration**: `Engine` (业务状态机与主循环)

## 🚀 快速开始

### 环境要求
*   Windows / Linux 开发环境
*   Android NDK (推荐 r23c)
*   OpenCV 4.10.0 Android SDK

### 编译与部署

本项目支持两种部署方式，详细步骤请参考 **[构建与部署指南 (README_BUILD.md)](README_BUILD.md)**。

#### 方式 A: Native Executable (推荐用于调试/无头设备)
通过 `build_android.bat` 脚本直接编译生成可执行文件，通过 `adb shell` 运行，无需安装 APK。

#### 方式 B: Android APK (推荐用于最终产品)
使用 Android Studio 打开项目，直接运行 `Run 'app'`。

## 📚 开发文档

为保障项目的可维护性与可持续性，我们提供了详细的 **[程序开发设计书 (DEVELOP.md)](DEVELOP.md)**，其中包含：
*   系统架构图解
*   核心模块详细设计
*   数据流转逻辑
*   扩展与维护指南

## 📊 性能指标

| 指标 | 目标值 | 实测表现 (预估) |
| :--- | :--- | :--- |
| **内存占用** | < 512 MB | ~80-120 MB |
| **CPU 使用率** | < 60% | ~25-40% (运动模式) |
| **视频延迟** | < 300 ms | ~150 ms |
| **启动时间** | < 2 s | < 1 s |

## 待办列表 (Todo List)

> ✅ 表示已完成

1.  ✅ **非工控机安卓设备兼容性优化**
    *   覆盖主流手机/平板芯片平台（高通、联发科、三星、展锐等）
    *   适配不同 Android 版本（API 21–34）的硬件抽象层差异
    *   在清单中声明所有动态特征模块，确保非工控机首次安装时按需下载
    *   对缺少工控机专用外设（GPIO、串口、CAN）的场景提供降级方案并给出用户提示
    *   完成 5 款以上非工控机真机冒烟测试，记录兼容性矩阵并输出报告

2.  ✅ **必要权限申请与检查机制优化**
    *   梳理最小必要权限列表（相机、存储、麦克风、定位、USB Host 等）
    *   实现一次性权限说明页，使用官方推荐「权限 rationale」弹窗文案
    *   在应用启动、功能入口、后台切换三个时机做权限缺失检查，拒绝时给出可跳转系统设置的一键修复按钮
    *   对 Android 13 及以上存储权限采用 Granular Media 权限模型，兼容分区存储
    *   编写单元测试与 UI 自动化测试，覆盖允许、拒绝、二次询问、永久拒绝全部场景

3.  ✅ **日志输出能力优化与日志导出功能**
    *   **3.1 日志格式与命名**
        *   统一采用 「rk3288_yyyyMMdd_HHmmss.log」会话级隔离文件名
        *   统一 Java (AppLog) 与 Native (NativeLog) 写入同一会话文件
        *   所有日志写入应用私有缓存目录：/Android/data/<package>/logs/，启动时自动清理 7 天前或超过 20 个的旧会话
    *   **3.2 日志查看与导出**
        *   修复点击 {VIEW LOGS} 无响应问题：在独立线程解析文件列表，使用 RecyclerView + DiffUtil 保证 UI 流畅
        *   列表默认展示最近 7 天缓存日志，按修改时间倒序；支持单选、多选、全选、反选
        *   点击条目可预览日志内容（仅读模式，支持关键字高亮与 100 KB 分页加载）
        *   提供「导出」按钮：调用系统 Storage Access Framework 文件选择器，由用户选择目标目录；将选中日志压缩为 logs_yyyyMMdd_HHmmss.zip，压缩前计算 CRC32 校验并写入 manifest.txt
        *   提供「一键导出全部」快捷按钮，后台使用 Kotlin Coroutine + ZipOutputStream，进度条实时显示，导出完成弹出系统通知，可点击直接打开 ZIP
    *   **3.3 日志性能与隐私**
        *   日志打印使用无锁环形缓冲区，Release 版默认 INFO 级以上输出，DEBUG 开关通过 BuildConfig 控制
        *   敏感字段（身份证号、手机号、GPS 坐标）统一脱敏为 ***，脱敏逻辑封装为 SensitiveDataUtil

4.  ✅ **摄像机检测能力优化**
    *   启动时通过 CameraManager.getCameraIdList() 枚举所有摄像头（含 USB 外接、前后置、IR、TOF）
    *   解析 CameraCharacteristics，按镜头朝向、分辨率、帧率范围生成 CameraInfo 列表
    *   在预览界面提供下拉/滑动切换控件，支持手动切换；切换时保存用户偏好到 SharedPreferences
    *   对热插拔 USB 摄像头注册 BroadcastReceiver，动态刷新列表并提示用户
    *   异常场景（权限拒绝、摄像头被占用、设备离线）给出友好 Toast + 日志记录

5.  ✅ **应用状态栏显示功能优化**
    *   在普通安卓手机状态栏区域（非工控机副屏）实时显示 FPS、CPU、MEM 三项指标
    *   使用 WindowManager.LayoutParams.TYPE_STATUS_BAR_PANEL 悬浮窗方案，自动适配挖孔屏与刘海屏
    *   刷新频率默认 1 s，允许用户在设置页 1–5 s 区间调节；CPU/MEM 通过 /proc/stat 与 /proc/meminfo 解析，FPS 通过 Choreographer.FrameCallback 计算
    *   提供状态栏开关，关闭时彻底移除悬浮窗并释放资源
    *   适配 Android 13 及以上通知权限与「在其他应用上层显示」权限，首次启用时引导用户手动授权

6.  ✅ **调试入口维护**
    *   持续维护 Native 调试入口 (`main.cpp`)，使其支持命令行参数配置（如摄像头 ID、运行模式），以便于脱离 Android UI 进行底层算法验证与性能分析。

## 📄 许可证
MIT License
