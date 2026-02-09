# RK3288 AI Engine - 构建与部署指南

**版本**: 2.0
**最后更新**: 2026-02-08

本项目支持两种构建模式，分别适用于不同的应用场景：
1.  **Native Executable (CLI)**: 纯 C++ 可执行文件，适用于调试、无头设备（Headless）或极低资源环境。
2.  **Android APK**: 标准 Android 应用，包含图形界面（GUI），适用于最终用户展示或带屏设备。

---

## 1. 准备工作 (Prerequisites)

无论选择哪种构建模式，请确保满足以下环境要求：

### 1.1 基础依赖
*   **操作系统**: Windows 10/11 (推荐) 或 Linux。
*   **JDK**: 建议 17+（用于 Gradle/Android Studio）。
*   **Android NDK**: 建议使用项目中已验证的版本（例如 r23 系列 LTS）。通过环境变量或脚本变量配置路径（不要硬编码到源码）。  
*   **OpenCV (Android/NDK)**: 与项目 CMake 配置匹配的版本。第三方源码/SDK 请放在本机可控目录，并通过环境变量/配置文件传入（不要写死路径）。

### 1.2 调试与日志
*   请在项目根目录建立 **【ErrorLog】** 文件夹，用于存放构建错误日志或运行时崩溃日志，便于后续调试分析。

---

## 2. 模式 A: Native Executable (CLI)

此模式直接编译生成 Linux/Android 可执行文件，不依赖 Java 虚拟机。

### 2.1 编译步骤
1.  打开项目根目录下的 `build_android.bat` 文件。
2.  检查并修改以下变量以匹配您的环境：
    ```bat
    set "NDK_ROOT=..."    :: 指向您的 NDK r23c 路径
    set "OPENCV_DIR=..."  :: 指向 OpenCV SDK 的 sdk/native/jni 路径
    ```
3.  双击运行 `build_android.bat`。
4.  脚本将自动创建 `build_android_rk3288` 目录并调用 CMake 进行交叉编译。
5.  **输出**: 编译成功后，可执行文件 `main` 将生成在 `build_android_rk3288` 目录下。

### 2.2 部署与运行
建议将程序推送到 Android 设备的 `/data/local/tmp` 目录（通常具有执行权限）。

```cmd
:: 1. 推送文件
adb push build_android_rk3288\main /data/local/tmp/rk3288_engine

:: 2. 赋予执行权限
adb shell "chmod +x /data/local/tmp/rk3288_engine"

:: 3. 运行程序 (默认使用摄像头 ID 0)
adb shell "export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/data/local/tmp && /data/local/tmp/rk3288_engine"

:: [可选] 指定摄像头 ID 运行 (例如使用 ID 1)
adb shell "export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/data/local/tmp && /data/local/tmp/rk3288_engine 1"
```

---

## 3. 模式 B: Android APK (GUI)

此模式生成标准的 `.apk` 安装包，包含完整的 Activity 生命周期管理和权限申请逻辑。

### 3.1 编译步骤
1.  **使用 Android Studio**:
    *   打开项目根目录。
    *   等待 Gradle Sync 完成（确保网络通畅以下载 Gradle 依赖）。
    *   点击菜单栏 **Build -> Build Bundle(s) / APK(s) -> Build APK(s)**。
    *   或者直接点击工具栏的 **Run 'app'** 按钮。

2.  **使用命令行 (Gradle Wrapper)**:
    ```cmd
    cd rk3288_opencv
    .\gradlew assembleDebug
    ```
3.  **输出**: APK 文件通常位于 `app\build\outputs\apk\debug\app-debug.apk`。

### 3.2 部署与运行
```cmd
:: 安装 APK
adb install -r app\build\outputs\apk\debug\app-debug.apk

:: 启动应用 (或者在设备上手动点击图标)
adb shell am start -n com.example.rk3288_opencv/.MainActivity
```

---

## 4. 常见问题 (Troubleshooting)

*   **CMake 找不到 OpenCV**:
    *   检查 `build_android.bat` (CLI模式) 或 `app/build.gradle` (APK模式) 中的路径配置。
    *   APK 模式下，CMake 路径通常在 `app/src/main/cpp/CMakeLists.txt` 中通过环境变量或硬编码指定，请确保 `OPENCV_DIR` 已正确传递。

*   **运行时权限拒绝 (Permission Denied)**:
    *   **Native 模式**: 访问 `/dev/video0` 可能需要 root 权限。
        ```cmd
        adb shell "su -c /data/local/tmp/rk3288_engine"
        ```
    *   **APK 模式**: 权限不足会进入安全模式（不闪退、不黑屏），监控功能被禁用。若永久拒绝，请到系统设置中手动开启。

*   **日志文件在哪里**:
    *   内部：`/data/data/<package>/files/logs/`
    *   外部：`/sdcard/Android/data/<package>/logs/`（受限制时回退到 `.../files/logs/`）

*   **截图离线分析器**:
    ```cmd
    .\\gradlew :screenshot-analyzer:run --args="--dir ErrorLog --out ErrorLog/SCREENSHOT_REPORT.md --crops ErrorLog/crops"
    ```

*   **架构不兼容**:
    *   本项目针对 **RK3288 (armeabi-v7a)** 优化。
    *   若在模拟器运行，请确保构建包含了 **x86_64** 架构 (已在 `build.gradle` 的 `abiFilters` 中配置)。

*   **缺少共享库 (.so)**:
    *   Native 模式采用静态链接 (`-static-libstdc++`, `OpenCV_STATIC`)，通常不需要额外库。
    *   APK 模式下，Gradle 会自动将 `libc++_shared.so` 打包进 APK。
