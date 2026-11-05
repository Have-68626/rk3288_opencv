<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-11-05
-->

# Linux 服务部署（初版）

本页描述 `linux_local_service` 的构建、配置与部署方式。**当前为初版**：已完成构建、健康接口与 SIGTERM 退出验证；真实摄像头、systemd 实机、ARM 运行均未验收。设计见 [Linux 运行时服务设计](../designs/linux-runtime-service.md)。

## 前提

- Ubuntu 24.04（GCC 13）、OpenCV 4.10 源码（`OPENCV_ROOT` / `OPENCV_CONTRIB_ROOT`）、`libssl-dev`、`libcurl4-openssl-dev`。
- 安装位置建议 `/opt/rk_wcfr/`（二进制 + `webroot/`），配置文件放运行账号的 XDG 目录。
- 服务默认只绑定 `127.0.0.1`；对外暴露须自行评估并在配置里显式设置 Token。

## 构建与安装

```bash
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export OPENCV_ROOT=/path/to/opencv-4.10.0
export OPENCV_CONTRIB_ROOT=/path/to/opencv_contrib-4.10.0
cmake --preset linux
cmake --build --preset linux -j "$(nproc)" --target linux_local_service
sudo install -d /opt/rk_wcfr
sudo cp cache/builds/linux-gcc13/bin/linux_local_service /opt/rk_wcfr/
sudo cp -r cache/builds/linux-gcc13/bin/webroot /opt/rk_wcfr/
```

模型与底库不放包内：按[模型存储与消费](model-storage-and-consumers.md)在配置里显式指向外部目录；缺模型时服务以 `degraded` 状态运行，健康接口返回原因码。

## 配置与密钥

- 配置路径：`RK_WCFR_CONFIG`（显式指定）→ `$XDG_CONFIG_HOME/rk_wcfr/config.json` → `~/.config/rk_wcfr/config.json`。格式与 Windows 共用同一 JSON Schema。
- 密钥：同目录 `config.key`，**权限 0600**（启动时创建；`stat -c '%a' config.key` 应为 `600`）。Linux 没有 DPAPI 等价物：同机同账号的其他进程可读取该密钥，这是与 Windows 的威胁差异。
- 配置正文（`config.json`）本身为 0644，其中敏感字段（`poster.postUrl`）在落盘时用 AES-256-GCM 加密，AAD 与 Windows 一致并计入 tag（2026-11-05 起）。Windows 旧版本写入的密文（AAD 未认证）在读取时按空 AAD 回退解密，下次保存自动升级；跨平台互通由 `tests/cpp/test_crypto_vectors.cpp` 的固定向量回归覆盖。

## systemd

示例 unit（`docs/guides/linux-service/rk-wcfr.service`）：

```ini
[Unit]
Description=rk_wcfr face recognition service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=rk-wcfr
Group=rk-wcfr
ExecStart=/opt/rk_wcfr/linux_local_service
Environment=XDG_CONFIG_HOME=/var/lib/rk-wcfr/.config
Environment=HOME=/var/lib/rk-wcfr
Restart=on-failure
RestartSec=3
# 服务自行处理 SIGTERM：停止时先等待进程退出码 0
TimeoutStopSec=15
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=/var/lib/rk-wcfr

[Install]
WantedBy=multi-user.target
```

```bash
sudo install -m 0644 docs/guides/linux-service/rk-wcfr.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rk-wcfr.service
curl -s http://127.0.0.1:8080/api/v1/health
```

## 验证与回滚

- 冒烟：`curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/api/v1/health` 期望 `200`；`systemctl stop rk-wcfr` 应在 `TimeoutStopSec` 内退出（SIGTERM → 退出码 0）。
- 日志：`journalctl -u rk-wcfr -f`；文件日志位置见配置 `log.logDir`。
- 回滚：保留上一版二进制与 `webroot/`；配置与密钥**不要**跨机器复制（Linux 密钥按文件权限保护，复制后属主与权限必须重建）。

## 未覆盖

真实摄像头采集、ARM 实机运行、TLS 终止、多实例与文件锁、跨平台密文向量回归均未验证；不要把本页当作运行验收证据。
