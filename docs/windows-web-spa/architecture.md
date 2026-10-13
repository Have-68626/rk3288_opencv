# 架构迁移说明（Win32 UI → 本地服务 + Web SPA）

**更新日期**: 2026-07-21


## 目标形态
- Windows 侧：仅保留“本地服务层”（文件系统/网络/硬件接口/业务逻辑），不再承担原生 UI 绘制。
- UI：全部迁移到浏览器访问的 SPA（React 18 + TypeScript 5 + Ant Design 5）。

当前主入口 `win_local_service` 已符合该形态；旧 `win_camera_face_recognition` 仍作为默认关闭的回滚 target 保留。因此“Windows 侧不承担原生 UI”只约束主入口，不表示旧 Win32 源码已删除。

## 模块边界
### 本地服务（C++）
- 负责：相机采集、识别管线、配置落盘/热重载/回滚、敏感字段加密、REST API、静态资源托管。
- 不负责：任何 Win32 控件创建、GDI 绘图、窗口布局、D3D 预览窗口等。

### Web SPA（React）
- 负责：路由/页面/交互、设置面板、调用本地 REST API、展示预览与识别结果。
- 不负责：硬件访问、密钥管理、敏感字段明文存储。

## 安全边界
- 服务仅监听 `127.0.0.1`，避免局域网/公网访问。
- 当前配置中的敏感字段 `poster.postUrl` 默认使用 AES-256-GCM 加密落盘，主密钥由 DPAPI 绑定到当前 Windows 用户；新增敏感字段时需同步扩展 Schema、序列化和轮换逻辑。

## 数据流（摘要）
1) SPA 通过 `GET /api/v1/settings` 拉取后端配置（敏感字段脱敏）。
2) 用户在 Web UI 修改设置后，通过 `PUT /api/v1/settings` 提交局部更新。
3) 后端对更新进行 Schema 校验 → 写入 `%APPDATA%\rk_wcfr\config.json` → 触发热重载。
4) 识别结果可通过 JSON/SSE 输出，预览通过 JPEG/MJPEG 输出；当前 SPA 主要消费 MJPEG 预览，尚无独立识别结果页。

