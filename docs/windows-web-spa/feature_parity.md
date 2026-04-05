# 功能对照清单（Win32 UI → 浏览器 SPA）

本清单用于保证新方案覆盖旧版 Win32 客户端全部功能点，并作为验收与迁移跟踪依据。

| 功能点 | 旧入口（Win32） | 新入口（Web） | 后端 API | 状态 | 备注/差异 |
|---|---|---|---|---|---|
| 启动/退出服务 | 运行 `win_camera_face_recognition.exe` | 运行 `win_local_service.exe` + 浏览器访问 | `GET /api/v1/health` | 已迁移 | 旧版为 Win32 窗口；新版为本地服务进程 |
| 健康检查 | 无单独入口 | 顶部状态区 | `GET /api/v1/health` | 已迁移 | 仅监听 127.0.0.1 |
| OpenAPI 文档 | 无单独入口 | “API 文档”链接 | `GET /api/v1/openapi` | 已迁移 | 另保留 `/openapi.json` 兼容 |
| 基础设置：主题 | 无（跟随系统/无） | 设置页 → 基础设置 |（浏览器本地） | 已迁移 | 存储于浏览器本地偏好；不写入后端配置 |
| 基础设置：语言 | 无 | 设置页 → 基础设置 |（浏览器本地） | 已迁移 | zh-CN / en-US |
| 基础设置：启动页 | 无 | 设置页 → 基础设置 |（浏览器本地） | 已迁移 | 影响 SPA 启动时跳转 |
| 高级设置：API 超时/日志级别/缓存策略 | 无 | 设置页 → 高级设置 |（浏览器本地） | 已迁移 | 其中 API 超时/缓存策略作用于前端请求 |
| 后端设置：配置读取（脱敏） | 读取 INI | 设置页 → 后端设置（查看） | `GET /api/v1/settings` | 已迁移 | 敏感字段脱敏输出 |
| 后端设置：配置写入（热生效） | 通过按钮“应用显示/应用相机”等写 INI | 设置页 → 后端设置（保存） | `PUT /api/v1/settings` | 已迁移 | 写入到 `%APPDATA%\\rk_wcfr\\config.json` 并热重载 |
| 相机选择/切换 | 相机下拉 + “应用相机”按钮 | 预览页 → 摄像头设备 | `GET /api/v1/cameras` + `PUT /api/v1/settings` | 已迁移 | 通过写入 `camera.preferredDeviceId` 与采集规格触发后端应用 |
| 分辨率/FPS 切换与能力校验 | 分辨率/帧率下拉 + “应用相机” | 预览页 → 分辨率/FPS | `GET /api/v1/cameras` + `PUT /api/v1/settings` | 已迁移 | 后端保留“失败回滚/错误原因”口径 |
| 翻转 X/Y | 复选框 | 预览页 → 翻转 X/Y | `PUT /api/v1/camera/flip` | 已迁移 | 运行时生效（当前未持久化到配置） |
| 人脸识别结果 JSON | 无独立页（在日志/叠加绘制体现） |（待补：结果页/调试页） | `GET /api/faces` / `GET /api/faces/stream` | 已迁移 | 后续可版本化到 `/api/v1/faces` |
| 预览画面 | 预览子窗口（D3D11） | 预览页 | `GET /api/v1/preview.mjpeg` / `GET /api/v1/preview.jpg` | 已迁移 | 通过 MJPEG 输出预览（浏览器直接显示） |
| 注册（Enroll personId） | personId 输入框 + “注册”按钮 | 预览页 → 注册 personId | `POST /api/v1/actions/enroll` | 已迁移 | |
| 清空人脸库 | “清空库”按钮 | 预览页 → 清空库 | `POST /api/v1/actions/db/clear` | 已迁移 | |
| 打开隐私设置 | “隐私设置”按钮 | 预览页 → 打开隐私设置 | `POST /api/v1/actions/privacy/open` | 已迁移 | |
| 运行稳定性/Soak/性能报告 | 命令行参数 + 最小化运行 |（不需要 UI） |（保留 CLI） | 已迁移 | 新版继续通过本地服务进程 CLI 参数驱动 |
