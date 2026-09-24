<!-- DOCSYNC_DOC
type=security
status=living
as_of=2026-09-23
-->

# FaceDatabase 加密安全规格（S-SEC-B09-002）

> 规格状态：**已锁定**（密钥边界与迁移方案在实现前书面锁定，未经批准不越界）；
> 正文跟随实现更新（元数据 `status=living`）。
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

## 7. 关联登记：日志中的明文 `person_id`（FACE-LOG-002，2026-09-23 追加）

> **本节为新增登记，不改变 §1–§6 的任何条款**（密钥边界、存储格式、版本化迁移与回滚、
> 公共 API 边界、验收判据均不变）。本节的写入链与 §1–§6 讨论的 `FaceDatabase` 文件加密
> **互不相干**——`FaceDatabase` 的落盘加密已生效，本节讨论的是**另一条**未加密的持久化路径。
> **决策状态：待拍板。当前为如实登记（未实施任何掩码，未改动任何日志写入代码）。**

### 7.1 事实（已逐行核实）

- `StructuredLogger::append`（`src/win/src/StructuredLogger.cpp`）把 `f.personId` 以**明文**写进
  `faces_json` 片段（仅经 `escapeJson` 转义 JSON，**不做掩码**）。同一片段落在两处：
  - CSV 的 `faces_json` 列（表头在该函数的 `headerWritten_` 分支，落列在其后的 `csv_ <<` 语句）
    → `<logDir>/recognition.csv`；
  - JSONL 的 `faces` 数组（同函数的 `jsonl_ <<` 语句）→ `<logDir>/recognition.jsonl`。
  - 两个文件路径由 `StructuredLogger::open` 拼出；`logDir` 默认 = exe 同级 `storage/win_logs`
    （`src/win/src/WinJsonConfig.cpp` 的 `defaultAppConfig()` 中 `log.logDir` 默认值），
    可由配置项 `log.logDir` 覆盖。
  ⚠️ 本条与下方「写方」清单中的 `StructuredLogger.cpp` 引用**已由行号锚点改为符号级**：本批给该文件
    加了轮转逻辑（冷启动轮转、代数收缩清理、`headerWritten_` 复位），行号整体后移，原文「这几处行号
    已于本批逐条复核、当前准确」的自证因此失真。
  ⚠️ **本节的其余行号锚点已逐条复核，结论如下**（原文曾笼统写「其余锚点指向的文件本批未改动，
    仍准确」——**那句是错的**，本批确实改过其中一部分文件，已按下述分类订正）：
    · **文件本批被改过、但所引行号仍准确**：`src/win/include/rk_win/StructuredLogger.h:31-39`
      （本批的新增落在其**之后**的私有段，`struct FaceLogEntry` 位置未变，已核）；
    · **文件本批被改过、但本节只按文件名引用（无行号）**：`src/win/include/rk_win/WinConfig.h`、
      `src/win/src/WinJsonConfig.cpp`、`tests/win/test_win_json_config.cpp` —— 不受行号漂移影响；
    · **文件本批未改动、行号仍准确**：`src/win/src/SideEffectSink.cpp:37`、
      `src/cpp/src/NativeLog.cpp` 的三处、`.gitignore:46-47`。
- 每条人脸记录含 `x` / `y` / `w` / `h` / `person_id` / `distance` / `confidence`
  （结构见 `src/win/include/rk_win/StructuredLogger.h:31-39`）。其中 `distance` 由
  `src/win/src/SideEffectSink.cpp:37` **恒写 0.0**（不携带任何信息），只有 `confidence` 是真实值。
- **结构性缺口（不是「漏了 personId 这一个键」）**：脱敏工具 `maskSensitiveData`
  （`src/cpp/src/NativeLog.cpp:228-270`）的敏感键预检清单（`:193-197`）与 KV 正则
  （`:249-254`）确实不含 `personId` / `person_id`；但更本质的是——`StructuredLogger`
  **完全不调用任何脱敏函数**（它只有 `escapeJson`，仅转义、不掩码），`maskSensitiveData`
  只服务于 `rklog::`（`NativeLog.cpp`）那条链（⚠️ 该链在 Windows 服务下**不落盘**——`src/win/`
  未接入 rklog，见 §7.2 末条）。⇒ 现状是
  「**有一条脱敏工具、有一条明文写入，两者互不相干**」；把 `personId` 加进键清单**不会**
  改变 `StructuredLogger` 的写入（它根本不经过该函数）。
- **仓内消费方：零**。全仓检索 `recognition.csv` / `recognition.jsonl` / `faces_json`，命中的
  只有**写方**与**注释/文案**两类，没有任何一处**读取**：
  - 写方：`StructuredLogger::open`（拼两个路径）与 `StructuredLogger::append`（CSV 表头、
    `faces_json` 落列、JSONL 的 `faces` 数组）——引用已改为**符号级**（理由见上一条的 ⚠️）；
  - 忽略规则：`.gitignore:46-47`；
  - 注释/文案（**均不读写该文件**）：`src/win/include/rk_win/WinConfig.h`（落盘上限说明）、
    `src/win/src/WinJsonConfig.cpp`（日志容量分摊告警文案）、
    `tests/win/test_win_json_config.cpp`（用例注释），以及本文件与
    `docs/guides/windows-service/deploy_and_rollback.md`。
  - 另有一份**仓根残留的未跟踪产物** `recognition.csv`（测试副作用、被 `.gitignore` 忽略）。
  ⚠️ 本节此前写「**只**命中写方与 `.gitignore`」——那是**穷举式断言**，已被本批自身推翻
  （上面列出的 `WinConfig.h` / `WinJsonConfig.cpp` 文案即反例），故改为分类列举。
  **实质结论不变且仍成立：无端点、无脚本、无测试读取该文件**。下游是人（Excel / pandas / jq）。
- **同一身份的实际暴露面（如实核对，勿据单点推断）**：
  - HTTP 的 JSON 接口**不返回** `personId`：`/api/faces`、`/api/v1/faces` 与 SSE 流
    （`/api/faces/stream`、`/api/v1/faces/stream`）均经 `buildFacesJson`
    （`src/win/src/FacesJson.cpp`）序列化，每张脸只有 `bbox` / `display_bbox` / `confidence`
    ——**没有** personId 字段。
  - `personId` **确实**出现在另外两处：(a) 本节的日志文件；(b) **预览画面像素**——
    `SideEffectSink::drawFacesOverlay`（`src/win/src/SideEffectSink.cpp`，标签拼装见 `:125`、
    `cv::putText` 见 `:133`）把 `personId + 置信度` 文本绘制到渲染帧上，而该帧正是
    `/api/v1/preview.jpg` 与 `/api/v1/preview.mjpeg` 返回的内容。
  - 识别库自身（`FaceDatabase`）以 AES-256-GCM 加密落盘（§1–§3），身份标签不在磁盘明文。
- 即：**personId 的明文入口是「日志文件」与「预览像素」两处**，性质不同——前者是持久化、
  可离线复制/外发的结构化 PII；后者是需认证（token 配置后）的实时画面。

### 7.2 未做掩码的决策依据（客观陈述，不替用户下结论）

- 掩码**会**减少一处持久化的明文身份面（日志文件长期留存，且随文件被复制/外发带走）；
  但同一身份仍会经预览画面像素暴露，故掩码**不消除全部**暴露路径。
- 掩码**会**损失「日志直接点名」的排障便利：排障者将无法从 `recognition.csv` 直接读出
  「谁被识别 / 未被识别」。掩码后仍保留的定位信息是**人脸框坐标 + `confidence`**
  （`distance` 恒 0，不提供信息）；若排障需要跨表关联身份，需另行引入可控关联手段。
- 可选掩码策略至少三种，**尚未拍板**：
  1. 全掩码（`person_id` 恒写为占位符）；
  2. 保留 SHA-256 前 8 字节（可跨表关联、不可反查身份）；
  3. 维持现状（明文），改以「日志目录按敏感数据保护」的运维约束承接。
- 相关但独立的既有事实（⚠️ **本批更正**，原文称「`logDir` 同时承载 `rklog::` 日志与上述结构化日志」）：
  **`logDir` 下没有、也不会有 `rklog::` 日志**。`src/win/` **根本未接入 rklog**（全仓 `src/win/` 对
  `rklog` 只命中 `WinJsonConfig.cpp` 里那句「本服务未接入 rklog」的注释，无任何 `rklog::logXxx` 调用），
  而 rklog 的文件落盘需先调 `rklog::setLogDirs`（`src/cpp/src/NativeLog.cpp`），其调用方只有 Android
  JNI（`src/cpp/jni/config.cpp`）与两个测试 ⇒ `win_local_service` 下 `logDir` **只承载三份文件集**
  （`recognition.csv` / `recognition.jsonl` / `event_log.jsonl`，另有各自 `.1`…`.N` 滚动档位）。
  ⚠️ **本批订正**：原文在此写「落盘面 = 3 份」「加 rklog 才是 4 份」——**该记账口径已不成立**。
  第 4 份是 **`render_metrics.jsonl`**（`RenderMetricsLogger`），由**旧版 UI 目标**
  `win_camera_face_recognition` 写进**同一个 `logDir`**（该目标同时写上述三份 ⇒ 它落 4 份），
  与 rklog **无关**（rklog 在 `src/win/` 下仍完全不落盘，这一结论未变）。
  `WinConfig.h` 的 `kLogFileSetCount` 已相应由 3 改为 **4**（预算按「写得多」的那个可执行体分配）。
  ⇒ 「按敏感数据保护 `logDir`」仍**不**与 §7.1 那条 `rklog::` 掩码链重叠：它是一条只针对这些
  **明文结构性日志**的运维约束；本条只订正份数，不改该结论。

### 7.3 拍板前即已成立的约束

- `storage/win_logs/`（或 `log.logDir` 指向的目录）**须按敏感数据保护**：其中含明文 PII
  （`person_id`），敏感级别与 `config.key.dpapi`、`FaceDatabase` 密钥文件同级。
- 部署与运维侧的落地要求见 `docs/guides/windows-service/deploy_and_rollback.md`
  「运行期所需文件清单」。
- 本节**未实施任何掩码**，也未改动任何日志的**写入内容/字段**；掩码策略一旦拍板，须先更新本节再同步
  实现与测试。（注：`StructuredLogger.cpp` 后续**已因轮转功能**被其他批次改动——冷启动轮转、代数收缩
  清理、`headerWritten_` 复位——那些改动**不触及**本节讨论的 `person_id` 写入内容与掩码决策；
  `NativeLog.cpp` 至今未改。）
