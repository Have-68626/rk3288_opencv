# 配置说明（config.json）

## 文件位置
- 配置文件：`%APPDATA%\rk_wcfr\config.json`
- 备份文件：`%APPDATA%\rk_wcfr\config.json.bak`
- 密钥文件（DPAPI 保护）：`%APPDATA%\rk_wcfr\config.key.dpapi`

## 校验（JSON Schema）
- Schema 文件：`docs/windows-web-spa/config.schema.json`
- 服务端会在启动与每次写入/热重载时对配置进行校验；校验失败会拒绝写入，并在热重载场景回滚到上一次有效配置。

## 敏感字段加密
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

