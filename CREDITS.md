# 致谢与许可证（CREDITS）

本文件用于记录本项目使用的第三方依赖、来源与许可证信息，便于审计与合规。

## 核心第三方组件

### OpenCV / opencv_contrib
- 用途：图像采集、处理、特征提取等（Native C++）
- 来源：OpenCV 官方项目
- 许可证：Apache License 2.0

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

## 工具与构建

### Gradle / Android Gradle Plugin
- 用途：构建系统
- 许可证：Apache License 2.0

### Android NDK / SDK
- 用途：Native 编译与 Android 平台开发
- 许可证：按 Android 官方发布条款

## 备注
- 如新增/升级/替换依赖，请同时更新本文件，并在 `DEVELOP.md` 中记录变更路径与影响面。

