# FaceDatabase 加密安全规格（S-SEC-B09-002）

> 状态：**已冻结**（密钥边界与迁移方案在实现前书面锁定，未经批准不越界）。
> 关联任务：治理队列 S-SEC-B09-002（FaceDatabase 加密）。前置先例：`WinJsonConfig` 敏感字段
> AES-256-GCM + DPAPI（`src/win/src/WinJsonConfig.cpp`）。

## 1. 现状与威胁

- `src/win/src/FaceDatabase.cpp` 使用 `cv::FileStorage` **YAML 明文**读写识别数据库
  （特征向量 + 身份标签），路径默认 `storage/win_face_db.yml`（`WinJsonConfig.cpp` 默认值）。
- 攻击面：本地磁盘明文暴露身份标签与生物特征向量；复制文件即可获得完整识别库。
- 无任何加密/密钥概念（修复前）。

## 2. 密钥边界（冻结）

### 2.1 密钥来源
- **DPAPI（Windows 当前用户，无口令）**，与 `WinJsonConfig` 一致：密钥为随机 **32 字节
  AES-256 主密钥**，用 DPAPI 保护后 **base64 落盘**，文件路径 = `<数据库路径>.key.dpapi`。
- FaceDatabase **独立管理自己的密钥文件**，不复用 `config.key.dpapi`：
  - `WinJsonConfigStore::ensureOrLoadKey()` 为私有方法且绑定 `config.key.dpapi` 生命周期
    （配置轮换/热重载会读写它）；提取到共享处会越出允许路径（`WinJsonConfig.*` 禁止修改）。
  - 决策：**复用 `WinCrypto` 公开原语**（`base64Encode/Decode`、`dpapiProtect/Unprotect`、
    `aes256gcmEncrypt/Decrypt`、`randomBytes`——均为 `rk_win` 命名空间自由函数，可直接调用），
    **不重复实现加解密原语**；仅在本文件内实现极小的“密钥文件加载或创建”辅助（约 30 行，
    布局与 `config.key.dpapi` 完全一致：base64(DPAPI(32B key))）。
- **密钥边界红线**：任何路径不得把 AES 主密钥或派生密钥明文落盘/入日志；错误消息不得包含
  密钥材料。
- **密钥丢失/损坏 = 数据库不可恢复**（与 `config.key.dpapi` 同类风险，产品层面需备份策略）。

### 2.2 存储格式（加密容器）
```
offset 0 : magic   8B  "RKFDBCRY"      ← 明文/加密判别
offset 8 : version 1B  1
offset 9 : nonceLen 1B  12
offset 10: tagLen   1B  16
offset 11: nonce   12B
offset 23: tag     16B
offset 39: ciphertext   （AES-256-GCM，AAD = 前 11 字节头部）
```
- 明文载荷**仅存在于内存**（序列化→加密，解密→反序列化），绝不落盘：
  - 行首签名 `RKFDBSER1`；每行一条人员：`<hexId> <count> <dim> <hexFloat0> ... <hexFloat(dim-1)>`
  - id 按 UTF-8 字节 hex 编码（任意字符安全）；mean 浮点按 IEEE-754 bit 模式 hex 编码（无损往返）。
- **判别**：文件头 8 字节 == magic → 加密容器；否则 → 旧明文 YAML。

## 3. 版本化迁移与回滚（冻结）

### 3.1 迁移（load 时自动）
1. `load(path)` 检测到旧明文 YAML → 先用既有 `cv::FileStorage` 逻辑读明文；
2. 序列化→加密→写临时文件 `<path>.enc.tmp`；
3. 备份旧明文到 `<path>.pre-encrypt`（回滚点）；
4. 原子替换临时文件 → `<path>`（`MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`）；
5. 迁移后 `save()` 一律写加密容器。

### 3.2 回滚保证
- 迁移**任一步失败即中止**，旧明文文件保持原样（`.pre-encrypt` 与替换前原路径双保留）；
- `load` 仍返回内存数据成功（只读兼容），不因迁移失败而拒绝读取；
- 人工回滚 = 用 `<path>.pre-encrypt` 覆盖 `<path>` 并删除加密文件/密钥文件。
- **明文备份留存（P3-1 S3 CONFIRMED）**：`.pre-encrypt` 是完整明文库副本（身份+特征向量），
  迁移成功后**永久留存磁盘**（回滚点），即威胁模型 §1 所述的明文残留。属有意保留（回滚保证），
  须由运维人工删除，或稳定运行期后增加"验证后删除"选项；不做默认自动删除（会破坏回滚契约）。

### 3.3 错误密钥/损坏密文
- 加密容器解密时 GCM tag 校验失败 → **受控失败**：`load` 返回 false、`persons()` 为空、
  不崩溃、不写任何明文。
- 加密文件存在但密钥文件缺失/损坏 → 同上受控失败（不得新建密钥强行解密/覆盖）。

## 4. 存储层改动边界

- **公共 API 不变**：`load/save/clear/hasPerson/listPersonIds/updateMean/persons` 语义与调用方不变。
- 仅改存储层；新增 `static keyPathFor(dbPath)` 供工具/测试计算密钥文件路径（纯加法，不影响既有方法）。
- 加密容器写入走临时文件 + 原子替换；密钥文件同样临时文件 + 原子替换（防半写）。

## 5. 验证与验收

- 新库**无明文特征**（文件头为 magic、文件内容不含明文身份标签/特征串；迁移备份 `.pre-encrypt` 明文留存除外，见 §3.2）；
- 旧明文库**可迁移且数据完整**；迁移失败可回滚不损坏旧库；
- 错误密钥**受控失败**（不崩溃、明确错误、数据不可读）。

## 6. 越界记录（不在此任务内）

- `win_face_database_perf`（`cmake/windows.cmake`）仅编译 `FaceDatabase.cpp`，**不含
  `WinCrypto.cpp`/bcrypt/crypt32**；加密接入后该 target 将**链接失败**。该 target 为既有
  挂起缺陷（SEC-02：>20s 无输出，与本次无关），且 `windows.cmake` 不在允许路径内 → 记录为
  **“待共享/待接线”后续任务**：向 `win_face_database_perf` 源表追加 `WinCrypto.cpp` 并链接
  `bcrypt`/`crypt32`。本次验证以 `-E win_face_database_perf` 排除 ctest。
- 密钥文件管理辅助（`ensureOrLoadKey`/`loadKey`）与 `WinJsonConfig` 内同名逻辑结构相似但
  参数化为任意路径；如需统一，后续可提取共享 `DpapiKeyFile` 工具（越出允许路径，另立任务）。
