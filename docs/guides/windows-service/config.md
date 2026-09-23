<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-06-23
-->

﻿# 配置说明（config.json）



## 文件位置
- 配置文件：`%APPDATA%\rk_wcfr\config.json` （可通过 `RK_WCFR_CONFIG` 环境变量覆盖）
- 备份文件：`%APPDATA%\rk_wcfr\config.json.bak`，并轮转保留 `.bak.1` 至 `.bak.5`（若覆盖配置路径，则备份文件在其同级目录）
- 坏配置证据：同目录 `config.json.corrupt.<毫秒时间戳>` —— 这是**另一类**文件，区别于上述 `.bak` 轮转副本：`config.json` 在**启动或热重载**时存在但无法装载时，坏文件被改名另存于此以保全证据，**不会被自动清理**（需人工处置）。
- 回滚临时文件：同目录 `config.json.rollback.tmp` —— 热重载被拒后写回内存配置时用的**固定名**临时文件；正常完成即消失，但进程在写盘中途崩溃会**残留**（不被自动清理）。残留无正确性影响（下次回滚以 trunc 覆盖它），仅占少量磁盘空间。
- 密钥文件（DPAPI 保护）：`%APPDATA%\rk_wcfr\config.key.dpapi` （同上，伴随配置路径生成）

### 其它支持的环境变量覆盖
除了 `RK_WCFR_CONFIG` 外，C++ 后端还支持以下环境变量覆盖特定配置，以便于容器化或临时调试：
- `RK_WCFR_DNN_MODEL`：覆盖 `dnn.modelPath`
- `RK_WCFR_DNN_CONFIG`：覆盖 `dnn.configPath`
- `RK_WCFR_HTTP_PORT`：覆盖 `http.port`
- `RK_WCFR_POST_URL`：覆盖 `poster.postUrl`

## 校验（JSON Schema）
- Schema 文件：`docs/windows-web-spa/config.schema.json`
- 服务端会在启动与每次写入/热重载时对配置进行校验；校验失败会拒绝写入。**启动**与**热重载**两条路径都会先做**证据保全**——把坏文件**改名另存**为同目录 `config.json.corrupt.<毫秒时间戳>`（不被自动清理），再按各自路径继续：启动路径**以默认值启动**；热重载路径用**内存中当前生效的配置**写回 `config.json`（使磁盘与内存**语义一致（而非字节一致）**——`postUrl` 会被重加密出新 nonce、`inference.*` 走归一化/钳制、且不序列化 `configPath`；以此不丢失用户最近一次成功 PUT），但**不把任何回滚内容采纳进内存、也不推进配置代数**（避免一次外部坏写静默撤销用户最近一次成功 PUT 并经代数变化触发管线重启）。写回动作与 PUT、密钥轮换的写盘路径**由同一把互斥锁串行化**，杜绝「回滚用旧快照覆盖已提交的新 PUT」的竞态。⚠️ 「坏配置不被就地覆盖」**仅保证到本次装载/处理结束**：原位置的后续写入（PUT、下一次热重载）仍按各自语义进行。若无法隔离（如文件被占用），则保留原文件、**不写盘**。
- **回退 `.bak` 的真实条件（订正）**：只有当**内存配置序列化产物未通过 schema 且密钥仍可用**（即加密失败这一极罕见情形）时，热重载路径才会回退到 `.bak` 上一次写入前的副本。若**密钥不可用**（`config.key.dpapi` 损坏 / 跨用户复制 / DPAPI 不可用），`.bak` 因需同一把密钥解密会被**一并判为不可用** ⇒ **两臂皆不可用 ⇒ 不写盘**（而非回退 `.bak`）。
- ⚠️ **热重载两臂皆不可用（不写盘）的后果**：坏文件已被改名为 `config.json.corrupt.<毫秒时间戳>` ⇒ `config.json`**不存在** ⇒ 下次启动走「不存在」分支 ⇒ **不**判定为 rejected、以**默认值运行**、`configStatus` 报 **`ok`**（密钥也坏时为 `degraded`）⇒ **用户原配置不会被自动恢复**，**需人工**把 `config.json.corrupt.<ts>` 改名回 `config.json` 才能找回。
- **启动时若 `config.json` 存在但无法装载**（不可读 / JSON 解析失败 / schema 校验被拒）：服务会把坏文件**改名另存**为同目录 `config.json.corrupt.<毫秒时间戳>`（同目录、保全证据、不覆盖不丢弃），随后**以默认值启动**；若改名（隔离）失败，则**保留原文件不覆盖**。
- **默认配置本身无法生成时不落盘**：密钥不可用（`config.key.dpapi` 损坏 / 跨用户复制 / DPAPI 不可用）时序列化会得到错误 stub，服务会在**落盘前**用 schema 校验产物，不通过则**拒绝写入**（避免写入非法 stub 引发「下次启动再隔离、再写同一 stub」的自持续循环），并把原因并入 `configWarning` 以保持可见性。
- 该启动期失败有**两个独立可见通道**：进程 **stderr**（零配置依赖）与 **`GET /api/v1/health`**（匿名可读、无需 token）——health 响应中的 `configStatus`（`ok` / `degraded` / `rejected` / `unknown`；`degraded` = 装载完成但本次启动有告警）、`configWarning`（告警文本，仅有告警时出现）、`configQuarantinedTo`（被隔离的坏文件名，仅发生隔离时出现）三个字段。

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

