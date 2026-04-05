# 致谢与许可证（CREDITS）

本文件用于记录本项目使用的第三方依赖、来源与许可证信息，便于审计与合规。

## 核心第三方组件

### OpenCV / opencv_contrib
- 用途：图像采集、处理、特征提取等（Native C++）
- 来源：OpenCV 官方项目
- 许可证：Apache License 2.0

### OpenCV 人脸检测模型与级联文件
- 用途：Windows 摄像头人脸检测（DNN）与测试数据（级联）
- 文件与来源：
  - `tests/data/lbpcascade_frontalface.xml`：来源于 OpenCV 数据集（随 OpenCV 发布）
  - `opencv_face_detector_uint8.pb` + `opencv_face_detector.pbtxt`：来源于 OpenCV DNN 示例/模型包（随 OpenCV 发布；用户自行下载并在 ini/env 配置路径）
- 许可证：Apache License 2.0（随 OpenCV 发布）

### ncnn
- 用途：端侧推理后端（可选；RK3288/Android 侧优先方案）
- 来源：Tencent 开源项目 ncnn
- 许可证：BSD 3-Clause License

### CivetWeb
- 用途：Windows 本地 HTTP 服务（localhost REST + 静态托管 + OpenAPI）
- 来源：https://github.com/civetweb/civetweb（本仓库 vendor：`src/win/third_party/civetweb/`，版本：v1.16）
- 许可证：MIT License

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
- 用途：人脸检测（YOLO）与人脸识别（ArcFace）推理模型
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

## 备注
- 如新增/升级/替换依赖，请同时更新本文件，并在 `DEVELOP.md` 中记录变更路径与影响面。

