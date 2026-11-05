<!-- DOCSYNC_DOC
type=design
status=living
as_of=2026-11-05
-->

# Linux 运行时服务设计

> **状态：设计已定，尚未实现（未落地）。** 9 项决策已于 2026-11-05 拍板（见下表），阶段 4 的实现按 4a–4e 推进。未实现前不得把本文当作已具备 Linux 服务能力的证据。

## 范围

把 Windows 本地服务的能力带到 Linux：HTTP 人脸服务（REST + SSE/MJPEG + 静态托管）、相机采集、配置读写、人脸库加密与上报。

非目标：D3D11 预览与旧相机 UI 不移植（Linux 用 Web 预览）；不为 RK3288 刷 Linux；不接 MPP 硬解码；不宣称 RK3288 AES-GCM 硬件加速。

## 现状（2026-10-28 只读盘点；文件:行号为盘点时读数，实施前须复核）

- `cmake/windows.cmake` 的 7 个 target 全在 `if(WIN32 …)` 内；`CMakeLists.txt` 只为 Windows 装配服务。
- 源码三分类：可直接跨平台约 28 个；需平台层 7 个（`SetConsoleCtrlHandler`、`GetEnvironmentVariableW`/`GetModuleFileNameW`、INI 迁移、`MoveFileExW`、DPAPI、Winsock、`ShellExecuteW`、`GetProcessMemoryInfo`）；需换实现 4 个（BCrypt/wincrypt、WinHTTP、Media Foundation、`NtCreateFile`）。
- 现有 `#else` 分支基本是失败桩：`getEnvW` 恒空、宽窄转换逐字节截断、原子替换恒失败、`WinCrypto` 全返 false、上报不启动、`readFileWithinRoot` 恒 false。⇒ 需要写实现，不是补分支。
- `HttpFacesServer` 的 POSIX 垫片很小（`SOCKET`/`INVALID_SOCKET`/`closesocket`/`SD_BOTH`/`socklen_t`），Host 白名单、CSRF、Token 校验与 socket 层解耦；但 Token 取自 `getEnvW`（Linux 恒空 ⇒ 会静默关闭认证）。`send` 未加 `MSG_NOSIGNAL`。
- 相机：`FramePipeline` 只依赖 `ICameraSource` 接口，生产代码只有一处硬绑定（构造 `MfCamera` + 静态 `enumerateDevices`）；头号阻断是 `MfCamera.h` 把接口也包进 `#ifdef _WIN32`。
- 配置：`RK_WCFR_CONFIG` → `%APPDATA%\rk_wcfr\config.json` → 当前目录；`.bak.1-5` 轮转 + 临时文件 + `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`；无权限处理；1s 轮询热重载；无文件锁。
- 加密：AES-256-GCM（nonce 12 / tag 16 / key 32），密钥用 `CryptProtectData` 保护；人脸库为带版本头的二进制容器。AAD 经链式调用参与认证（见 D3），Linux 实现使用相同 AAD 字节即可保持密文互通。
- `tests/win` 14 个文件中 6 个可直接移植、5 个加小垫片、`test_http_faces_server` 非 Windows 段全是 SKIP。

## 需要拍板的决策

| # | 决策 | 选项 | 结论 | 状态 |
|---|---|---|---|---|
| D1 | 加密密钥保护 | ① 0600 文件 ② kernel keyring ③ systemd-creds ④ 口令派生 | ① 0600 文件；文档写明弱于 DPAPI（同机同用户进程可读） | 已定 |
| D2 | 加密库 | OpenSSL libcrypto / mbedTLS / libsodium | OpenSSL libcrypto；不得宣称硬件加速 | 已定 |
| D3 | 密文互通 | 与 Windows 互通（AAD 按现状） / 修正后升 v2 | **按代码判定：AAD 参与认证**（`WinCrypto.cpp` 先用 `BCRYPT_AUTHENTICATED_CIPHER_MODE_CHAIN_CALL_FLAG` 以 AAD 为输入做一次链式调用，再以相同 `BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO` 加密数据），Linux 实现使用相同 AAD 字节即可互通；跨平台向量测试待补 | 待向量测试 |
| D4 | 配置路径 | XDG（`$XDG_CONFIG_HOME/rk_wcfr`） / `/etc` + `/var/lib` | XDG，沿用同一 JSON Schema | 已定 |
| D5 | 字符串模型 | 保留 `wstring` / 内部统一 UTF-8 | 内部统一 UTF-8，接口收窄处再转换 | 已定 |
| D6 | 上报客户端 | libcurl / 自研 POSIX HTTP（无 HTTPS） | libcurl（HTTPS、代理、超时齐全） | 已定 |
| D7 | 相机接入 | `VideoCapture(CAP_V4L2)` 适配器 / 直接 V4L2 | `ICameraSource` 适配器，设备 ID 用 `/dev/videoN` + by-id 兜底 | 已定 |
| D8 | 服务生命周期 | 前台 + SIGTERM / systemd notify | 前台进程 + SIGTERM 优雅退出，附 systemd unit 示例 | 已定 |
| D9 | 代码组织 | 原地 `#ifdef` / 拆公共层 | 拆最小平台层（socket/路径/进程），不做大规模改名 | 已定 |

## 实施切分（决策通过后）

- **4a 平台层**：socket、路径与 exe 目录、进程信号；`HttpFacesServer` 改走平台层，Windows 行为与既有测试不变。安全项：`MSG_NOSIGNAL`、`shutdown` 唤醒 accept、`socklen_t`、认证 Token 的 Linux 来源。
- **4b 相机**：抽出 `ICameraSource` 到跨平台头，新增 V4L2 实现；`FramePipeline` 用平台工厂；设备枚举自写。
- **4c 配置与加密**：XDG 路径、`rename` + `fsync` 原子写、权限位；Linux 加密实现（D1/D2 定后），补权限与密文格式回归。
- **4d 服务入口**：`linux_local_service`、`cmake/linux.cmake`、可移植测试 `linux_unit_tests`，接入 `linux` 与 `linux-arm-cross` job。
- **4e 文档与部署**：部署指南、OpenAPI/配置 Schema 平台说明。

## 验收（每步都必须满足）

- Windows 全量回归不退化（对应 job 全绿）。
- 安全语义与 Windows 对齐：默认只绑回环、Host 白名单、CSRF、Token 校验，且每项有跨平台回归。
- 无 GCM 硬件加速声明；上报超时、重试、脱敏与 Windows 一致。
- 交叉编译通过不等于运行可用；Linux 运行验收只在 x86_64 上进行，ARM 仍只做编译检查。
