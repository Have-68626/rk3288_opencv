# 部署与回滚指南（Windows 本地服务 + 浏览器 SPA）

**更新日期**: 2026-07-21


## 运行方式（推荐）
1) 启动本地服务（无 UI）
- 可执行文件：`win_local_service.exe`
- 监听地址：`127.0.0.1:<port>`

2) 打开浏览器访问
- `http://127.0.0.1:<port>/`（SPA）
- `http://127.0.0.1:<port>/api/v1/health`（健康检查）

## 前端构建与静态托管
- 前端源码：`web/`
- Vite 构建输出目录：`src/win/app/webroot/`
- CMake 在构建 Windows 二进制后，会把 `src/win/app/webroot/` 复制到 exe 同级 `webroot/`，作为本地 HTTP 服务的静态资源根目录。

## 配置与数据目录
- 配置文件：`%APPDATA%\rk_wcfr\config.json`（支持热重载）
- 密钥文件：`%APPDATA%\rk_wcfr\config.key.dpapi`（DPAPI 保护）
- 配置备份：同目录 `config.json.bak` 与 `.bak.1` 至 `.bak.5`
- `ErrorLog/` 是仓库内人工收集用户报错与复现材料的位置，不是本地服务自动生成的运行时数据目录。

## 端口调整
- 推荐通过后端 settings 修改：`PUT /api/v1/settings` 写入 `http.port`
- 配置轮询应用变更后，服务会停止旧 HTTP 监听并尝试在新端口启动；客户端需改用新端口重新连接。

## IE11（可选降级）
- Ant Design 5 与现代构建产物不支持 IE11。
- 方案：HTML 级别检测 IE11 并跳转到静态降级页 `ie11.html`（提供提示与引导）。

## 回滚到旧版 Win32 UI
旧版 Win32 UI 仅作为回滚目标保留（默认不构建）。

1) CMake 打开旧 UI 构建开关：
- `-DRK_BUILD_WINDOWS_CAMERA_FACE_RECOGNITION_UI=ON`

2) 构建 target：
- `win_camera_face_recognition`

3) 运行旧 UI：
- `win_camera_face_recognition.exe`

