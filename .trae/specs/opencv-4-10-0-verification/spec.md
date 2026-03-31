# OpenCV 4.10.0 编译验证与功能测试 Spec

## Why
用户需要验证核心业务逻辑在 OpenCV 4.10.0 环境下的编译兼容性及静态图片识别功能的准确性。当前项目主要针对 Android (RK3288) 平台，但为了进行快速验证及生成详细测试报告，需要支持在 Host (Windows) 环境下进行模拟测试，或确保 Android 编译流程无误并能通过 CLI 工具验证。

## What Changes
1.  **环境配置检查**:
    -   验证 `CMakeLists.txt` 中定义的 `OPENCV_ROOT` (D:/ProgramData/OpenCV/opencv-4.10.0) 是否有效。
    -   检查 `build_android.bat` 中的路径配置。

2.  **代码适配 (Host Build Support)**:
    -   修改 `CMakeLists.txt`: 增加非 Android 环境下的构建支持，移除对 `log`、`jnigraphics`、`android` 库的强制链接，改用条件编译。
    -   修改 `src/cpp/src/NativeLog.cpp`: 增加 `#ifdef __ANDROID__` 宏，在非 Android 环境下将日志输出重定向到标准输出 (std::cout/cerr)。
    -   修改 `src/cpp/src/VideoManager.cpp`: 增加 `#ifdef` 宏，屏蔽 Windows 不支持的 `setpriority` 和 `sys/resource.h`。

3.  **测试数据准备**:
    -   创建 `tests/data` 目录。
    -   提供/生成测试用的静态图片 (JPEG, PNG, BMP)。
    -   确保人脸识别用的 Cascade 文件 (`lbpcascade_frontalface.xml`) 可被测试程序访问。

4.  **自动化测试脚本**:
    -   创建 `scripts/verify_opencv_host.bat`: 用于在 Windows 上编译并运行 `rk3288_cli`，批量测试图片并收集结果。
    -   (可选) 更新 `scripts/build_android.bat` 以确保 Android 编译通过。

5.  **报告生成**:
    -   根据测试输出生成 `TEST_REPORT_OPENCV.md`，包含编译日志、测试用例结果（成功/失败）、性能指标（耗时）。

## Impact
-   **Affected files**: `CMakeLists.txt`, `src/cpp/src/NativeLog.cpp`, `src/cpp/src/VideoManager.cpp`, `scripts/build_android.bat`.
-   **New files**: `scripts/verify_opencv_host.bat`, `tests/data/*`.

## ADDED Requirements
### Requirement: Host Compilation
系统必须支持在 Windows 主机上编译 `rk3288_cli` 可执行文件，用于开发阶段的快速逻辑验证。

### Requirement: Functional Verification
系统必须能够加载指定路径的静态图片（支持 .jpg, .png, .bmp），执行人脸检测与识别流程，并输出识别结果。

### Requirement: Reporting
系统必须记录编译警告/错误，并生成包含识别成功率、处理速度的 Markdown 报告。
