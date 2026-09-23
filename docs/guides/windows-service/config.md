<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-06-23
-->

﻿# 配置说明（config.json）



## 文件位置
- 配置文件：`%APPDATA%\rk_wcfr\config.json` （可通过 `RK_WCFR_CONFIG` 环境变量覆盖）
- 备份文件：`%APPDATA%\rk_wcfr\config.json.bak`，并轮转保留 `.bak.1` 至 `.bak.5`（若覆盖配置路径，则备份文件在其同级目录）
- 坏配置证据：同目录 `config.json.corrupt.<毫秒时间戳>` —— 这是**另一类**文件，区别于上述 `.bak` 轮转副本：`config.json` 在启动时存在但无法装载时，坏文件被改名另存于此以保全证据，**不会被自动清理**（需人工处置）。
- 密钥文件（DPAPI 保护）：`%APPDATA%\rk_wcfr\config.key.dpapi` （同上，伴随配置路径生成）

### 其它支持的环境变量覆盖
除了 `RK_WCFR_CONFIG` 外，C++ 后端还支持以下环境变量覆盖特定配置，以便于容器化或临时调试：
- `RK_WCFR_DNN_MODEL`：覆盖 `dnn.modelPath`
- `RK_WCFR_DNN_CONFIG`：覆盖 `dnn.configPath`
- `RK_WCFR_HTTP_PORT`：覆盖 `http.port`
- `RK_WCFR_POST_URL`：覆盖 `poster.postUrl`

## 校验（JSON Schema）
- Schema 文件：`docs/windows-web-spa/config.schema.json`
- 服务端会在启动与每次写入/热重载时对配置进行校验；校验失败会拒绝写入，并在热重载场景回滚到上一次有效配置。
- **启动时若 `config.json` 存在但无法装载**（不可读 / JSON 解析失败 / schema 校验被拒）：服务会把坏文件**改名另存**为同目录 `config.json.corrupt.<毫秒时间戳>`（同目录、保全证据、不覆盖不丢弃），随后**以默认值启动**；若改名（隔离）失败，则**保留原文件不覆盖**。
- 该启动期失败有**两个独立可见通道**：进程 **stderr**（零配置依赖）与 **`GET /api/v1/health`**（匿名可读、无需 token）——health 响应中的 `configStatus`（`ok` / `rejected`）、`configWarning`（告警文本，仅有告警时出现）、`configQuarantinedTo`（被隔离的坏文件名，仅发生隔离时出现）三个字段。

## 敏感字段加密

当前实现纳入加密与轮换的配置字段是 `poster.postUrl`。下述格式不是对任意字符串字段自动生效；新增敏感字段必须显式接入 Schema 与 `WinJsonConfigStore`。
### 加密算法
- AES-256-GCM（随机 nonce；包含完整性校验 tag）
- AES 主密钥通过 Windows DPAPI（Current User）保护后落盘到 `config.key.dpapi`

### 落盘格式
当字段为密文对象时，形态如下（示例）：
```json
{
  "v": 1,
  "alg": "AES-256-GCM",
  "nonce": "<base64>",
  "ciphertext": "<base64>",
  "tag": "<base64>"
}
```

### 轮换（Key Rotation）
- API：`POST /api/v1/actions/crypto/rotate`
- 行为：生成新 AES 主密钥（写入 DPAPI 保护后的 key 文件），并将敏感字段重新加密后写回 config.json

## 常用字段（摘要）
- `http.port`：本地服务端口（仅监听 127.0.0.1）
- `camera.preferredDeviceId` / `camera.width` / `camera.height` / `camera.fps`：相机选择与目标采集规格
- `poster.enable` / `poster.postUrl`：外部上报开关与地址（落盘默认加密）
- `log.logDir`：日志目录（相对路径按 exe 目录解析）

