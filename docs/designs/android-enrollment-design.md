<!-- DOCSYNC_DOC
type=design
status=living
as_of=2026-09-12
-->

# Android 端人员注册设计（目标 vs 已落地）


**状态**: 部分落地。核心数据层与注册会话已实现并有单测覆盖；**注册 UI 与冷启动恢复已接线（任务 18）**，但**注册闭环因缺 native 逐帧特征源仍不可用（Blocker，待补）**。

**范围**: Android App（`src/java/com/example/rk3288_opencv/` 与 JNI `src/cpp/jni/native_face_bridge.cpp`）。

**与顶层设计的关系**: 本文只描述 **Android 现场端**的实现边界，不重述人员域、RBAC/ABAC、审计、双端同步等目标模型；那些内容以 `docs/designs/personnel-enrollment-design.md` 为准。顶层设计 §1.1 中“Android 加密模板存储工具尚未证明接入注册 UI”一行，本文给出截至当前的准确状态。

## 1. 目标 vs 已落地（总表）

| 能力 | 目标 | 状态 | 本地证据 / 边界 |
|---|---|---|---|
| 加密模板仓库 | 模板向量加密落盘、人员主数据与模板分离 | **已落地** | `AndroidEnrollmentRepository` + `FeatureTemplateEncryptedStore`（AndroidKeyStore + AES-GCM）；注册表 `face_registry_v1.dat` 只存元数据，不含向量 |
| 注册表落盘崩溃安全 | 中断不丢索引、可自愈 | **已落地** | `persistRegistry` 阶段替换 + `.bak` 恢复；单行损坏跳过隔离，成功写时自愈 |
| 八样本注册会话 | 采样门槛 + 姿态配额 + 均值模板 | **已落地（任务 17）** | `EnrollmentSession`：8 样本、正4/左2/右2，L2 归一均值模板 |
| 重复人保护 | 拒绝重复注册 | **已落地** | `EnrollmentSession` 与现有模板 cosine ≥ 0.55 判 `DUPLICATE_PERSON`；仓库侧 `DUPLICATE_USER` |
| 模板加解密落盘 | 由仓库闭环完成 | **已落地** | `AndroidEnrollmentRepository.add/remove/readTemplate/list/get` 接线加密存储 |
| JNI 管线桥 | Android 端能真实检测/识别 | **已落地（P1-2 修复）** | `NativeFaceBridge` 8 个 native 方法；`loadProfile` 仅校验/暂存，`activateRuntime` 决定 `effective` |
| 模型管理器 | 用户导入 ZIP、校验、原子启用 | **已落地** | `FaceModelManager.importZip`，详见 `docs/designs/model-acquisition-import.md` |
| 注册 UI 接线 | 把会话接到预览页/注册按钮 | **已接线（任务 18）** | `MainActivity.bindModelManagementUi` 绑定注册按钮 → `showFaceRegisterDialog`/`startEnrollment`；会话构造于 `:3809`，完成时经仓库 `add` 落盘于 `:3932` |
| 注册闭环（逐帧特征源） | 预览帧喂入 8 样本会话并完成采集 | **未落地（Blocker，待补）** | `MainActivity.feedEnrollmentSample`（`:3903`）无生产调用方；JNI 未暴露逐帧 embedding，采集恒停在 0/8，详见 §3 |
| 冷启动恢复 | 重启后恢复已注册库/激活态 | **已落地（任务 18）** | `MainActivity.restoreActiveProfileOnStartup`（`:3553`，`onCreate` `:1886` 调用）：先重建图库 → `replaceGallery`，再读 `active_profile` → `activate` 回放激活 |
| 低版本（API < 23）只读降级 | 只读预览 + 注册入口禁用 | **已落地** | 写操作返回 `UNSUPPORTED_PLATFORM`；`list/get` 读取放行 |
| 双端模板互通 | Android/Win 模板直通 | **未实现（研究项）** | 顶层设计 §8.7；当前不假设直通 |

> 口径提醒：上表“已落地”指代码与单测层面成立，**不等于**已在真机完成注册/重启/识别/删除闭环；真机验收见 `docs/guides/rk3288-device-acceptance.md`。

## 2. 已落地设计

### 2.1 加密模板仓库 `AndroidEnrollmentRepository`（任务 16）

存储布局（默认作用域 `files/face_templates/`，与 `FeatureTemplateEncryptedStore.defaultDir` 一致）：

- `face_registry_v1.dat` —— gallery 级单表注册表，记录**元数据**：`hex(userId) mv sv ciphertextSize sha256hex createdAtMs`，**不含任何生物特征向量**。
- `u_<sha256(userId)>.bin` —— 每人加密模板，由 `FeatureTemplateEncryptedStore` 以 AndroidKeyStore + AES-GCM 落盘。

关键设计取舍：

- **模板加密、注册表明文**：敏感的是向量；模板文件名是 userId 的 SHA-256，没有注册表无法反推，因此索引必须依赖一张表；低版本无 Keystore AES-GCM 时，明文注册表仍能支撑“只读预览”。
- **userId 用 hex 可逆编码而非哈希（明文档化取舍）**：为在低版本展示人员标识；若后续要求不可逆，需放弃 `list` 展示或改为单向派生，不能只改编码一处。**该取舍仍然成立**——见下一条的保护边界。
- **显示名加密（任务 B，2026-09-16）**：显示名**不再**以明文存于 SharedPreferences，改由 `PersonDisplayNameStore` 加密存于 `files/face_names/n_<sha256(personId)>.enc`（KeyStore AES-GCM，**独立密钥** `person_name_aes_gcm_v1` + **独立 AAD 域**，与模板文件互不可换用）。**三者各自的边界**：注册表 `face_registry_v1.dat` 只存 userId 等元数据、**不含显示名**；显示名加密存于上述目录；模板向量加密存于 `files/face_templates/`。⚠️ `userId` 仍是可逆编码，故**不得**宣称「人员 PII 全部加密」。
- **显示名生命周期（任务 B）**：采集期**仅存内存**；**注册提交（`add` 落盘）成功后即加密写入**，与图库替换结果**解耦**——图库替换失败**不回滚**显示名，否则「模板已落盘但图库替换失败」的人会永久丢失显示名（A1 审计 P3-3 修正）。因此取消/失败**不会留下残留**（此前是先写 SharedPreferences 再采集，取消/失败都不清理）。旧明文键经一次性迁移清除，顺序为「加密 → **验证密文可读** → 删原值」，失败保留原值待重试。API<23 或密钥不可用时返回 `UNSUPPORTED_PLATFORM` / `KEYSTORE_ERROR`，**绝不回落明文**，UI 降级显示 personId。密钥刻意**不设** `setUserAuthenticationRequired(true)`：设了则只能在「已设置安全锁屏」的设备上生成，且锁屏变更会令其不可逆失效。
- **显示名文件的摘要暴露（如实记录，不做安全表演）**：显示名文件的**文件名与 AAD 都明文含 `sha256(personId)`**，理论上可对低熵 personId（姓名/工号）做字典反查。但**真正的短板是注册表本身**——`face_registry_v1.dat` 用**可逆 hex** 存 userId，能读到显示名文件的攻击者必然也能读到注册表，从中直接还原 personId。故给文件名加盐**不构成实质改进**（盐只能与数据同处一地）。**真正的修复路径**是让注册表不再存可逆 userId，改存不可逆随机 id——而任务 B 引入的加密显示名存储恰好使这条路可行（人可读标签已不再依赖 userId）。属**独立设计变更**，未在本批实施。
- **降级与旧数据（API<23）**：AndroidKeyStore 的 **AES/HMAC 支持自 API 23 起**（更早版本只提供 RSA/EC），故本项目在 API 21–22 上**无法直接复用现有 AES-GCM 方案**。
  ⚠️ **但不得据此断言「这些设备没有密钥保护、且没有替代方案」**：Android Keystore provider **自 API 18 即存在**，硬件保护程度取决于设备与算法；`androidx.security:security-crypto` 的 `MasterKey` 被弃用**本身**也不能证明所有替代路径都不安全——例如「用 Keystore 中的 RSA 密钥包裹随机 AES 密钥」在 API 18+ 即具备可行性。
  **准确表述（本项目的立场）**：**本项目尚未实现并验证**适用于 API 21–22 的替代加密方案，因此**不启用**该 API 级别的显示名加密持久化，也**不回落到新增明文**；UI 使用 `personId`。
- **历史明文键：默认保留 + 用户确认后清除（已决策）**：**不在启动、升级或迁移失败时自动删除**。检测到历史键时提示「本设备仍保留未加密的历史显示名，当前不用于展示，可手动清除」；清除需**二次确认**（明确「显示名不可恢复，人员 ID 和模板不受影响」）；**仅删除显示名对应键，禁止清空整个 SharedPreferences**；`commit()` 落盘成功后才提示完成。
  理由：保留是**避免未经同意丢失数据**，**不是**低估历史 PII 风险——而是把它当作**兼容旧数据的明确例外**。**只要历史键存在，就不得宣称「设备上已无明文 PII」**；「注册功能在 API<23 不可用」也不能替代对其他写入路径的检查（已核查：全仓除 `PersonDisplayNameStore.save`（加密）外，**无其他显示名写入路径**）。
- **并发**：按规范化 `scopeDir` 路径共享静态锁，串行化同目录多实例的 `add/remove/readTemplate/list/get`。
- **容错**：单行损坏跳过并计数（`skippedLineCount`），仅文件头（魔数 `FREG1`/版本）损坏才判 `CORRUPTED`；后续成功写入自愈。
- **崩溃恢复（P1-3）**：`persistRegistry` 采「tmp 落盘+fsync → dst→bak → tmp→dst → 清理 bak」；中间退出导致主文件缺失时，`loadRegistry` 先用 `.bak` 恢复为主文件再解析，避免把唯一索引伪装成空库。
- **API < 23**：`add/remove/readTemplate` 返回 `UNSUPPORTED_PLATFORM`；`list/get` 放行用于只读预览。

### 2.2 八样本注册会话 `EnrollmentSession`（任务 17）

状态机：`IDLE --start()--> COLLECTING --第 8 个样本被接受--> COMPLETED`，任意态可 `cancel()/reset()`；`reset()` 回 `IDLE`。

- **内存与隐私**：不接收、不保存任何图像像素；样本只含调用方测好的数值指标与 512 维 embedding。集满即求均值、L2 归一，只保留一个均值模板；取消/复位清零；本类不调用日志接口。
- **采样门槛（按检查顺序）**：单确认脸（`faceCount==1`）→ 检测置信度 ≥ 0.5 → 面积占比 ≥ 10% → Laplacian 方差 ≥ 80 → 灰度均值 ∈ [50, 205] → 灰度标准差 ≥ 25 → 姿态可判定 → 姿态配额（正4/左2/右2）→ 与已收样本 cosine ≥ 0.75（否则视为不同人/异常帧）→ 现有模板快照必须可读（读取失败 `REPO_UNAVAILABLE` 直接拒绝，**绝不按“库中无重复”放行**）→ 与现有模板 cosine ≥ 0.55 判重复人员。
- **常量**：`EMBEDDING_DIM=512`、`TARGET_SAMPLES=8`、`QUOTA_FRONT/LEFT/RIGHT=4/2/2`、`MIN_DET_SCORE=0.5`、`MIN_AREA_RATIO=0.10`、`MIN_LAPLACIAN_VARIANCE=80`、`MIN_GRAY_MEAN=50`、`MAX_GRAY_MEAN=205`、`MIN_GRAY_STD=25`、`MIN_SAMPLE_COSINE=0.75`、`DUPLICATE_TEMPLATE_COSINE=0.55`、`POSE_YAW_THRESHOLD=0.15`。
- **阈值来源**：Laplacian/灰度/面积占比/cosine/L2 归一复用 `ImageMetrics` 静态实现；姿态默认由关键点偏航代理分类（`ImageMetrics.classifyPose`），调用方可直接给 `Pose`。

### 2.3 JNI 管线桥 `NativeFaceBridge`

运行时动态注册（`registerFaceBridgeMethods`），类 `com/example/rk3288_opencv/NativeFaceBridge`，共 8 个方法：

| 方法 | 作用 | 生产调用方 |
|---|---|---|
| `loadProfile(manifestJson, currentAbi)` | 仅校验 manifest 格式/ABI 并暂存（`validated:true`，**不代表生效**） | `FaceModelManager` |
| `activateRuntime(manifestJson, baseDir, currentAbi, galleryDir)` | 绑定实时管线；Engine 未创建时暂存为 pending，创建后补激活 | `FaceModelManager` |
| `replaceGallery(personIds, embeddings)` | 替换内存图库 | `MainActivity.refreshGalleryFromRepository`（`:4027`） |
| `beginEnrollment(personId, embedding)` | 登记注册意图 | 暂无 |
| `cancelEnrollment()` | 取消注册 | 暂无 |
| `getEnrollmentStatus()` | 查询注册状态 | 暂无 |
| `takeEnrollmentTemplate()` | 取走模板字节（取走即清零） | 暂无 |
| `getRuntimeStatus()` | 运行时状态 JSON：`profileValidated`/`profileLoaded`/`profileId`/`embeddingDim`/`galleryLoaded`/`galleryCount`/`galleryDim`/`enrollmentState`/`effective`/`reason` | `FaceModelManager`（`:136`）、`MainActivity`（`:621`） |

> 注册类 JNI 方法现状：`beginEnrollment`/`cancelEnrollment`/`getEnrollmentStatus`/`takeEnrollmentTemplate` **仍无生产调用方**。闭环缺逐帧特征源（见 §3）；Java 侧完成注册走 `EnrollmentSession.meanTemplateLe()` + 仓库 `add`，不经过这 4 个 native 方法。`MainActivity.cancelEnrollmentInternal` 是会话侧取消，非 `cancelEnrollment` JNI 调用。

语义要点：

- `loadProfile`/`activateRuntime` 复用同一套严格 `FaceRuntimeManifest::parseV1` 校验（schemaVersion==1、ABI、路径、SHA-256 格式）。
- 是否“已就绪”只看 `effective` 与 `reason`；`profileLoaded` 仅为旧消费方保留的同值别名，语义等于 `profileValidated`。
- 尚无模型/未跑帧时 `reason` 缺省为 `missing_model`，UI 不再误报就绪。

### 2.4 模型管理器 `FaceModelManager`

APK 不内置模型，用户经 SAF 导入 ZIP；校验链、ZIP 契约与原子启用细节见 `docs/designs/model-acquisition-import.md`。

## 3. 未落地（任务 18 及以后）

- **注册闭环的逐帧特征源（Blocker，待补）**：注册 UI 已接线——`EnrollmentSession` 由 `MainActivity.startEnrollment`（`:3809`）构造，采样进度/拒绝原因由对话框轮询展示，完成时经仓库 `add`（`:3932`）加密落盘并 `replaceGallery` 刷新运行中图库；但**当前 JNI 契约未暴露逐帧 (embedding, box, landmarks, 质量指标)**，`feedEnrollmentSample`（`:3903`）无生产调用方，会话恒停在 0/8，闭环不可用。`NativeFaceBridge.beginEnrollment`/`cancelEnrollment`/`getEnrollmentStatus`/`takeEnrollmentTemplate` 4 个注册类原生方法**仍无生产调用方**。
- **冷启动恢复**：已落地——`restoreActiveProfileOnStartup`（`:3553`，`onCreate` `:1886` 调用）先由加密仓库重建内存图库并 `replaceGallery`，再读取 `active_profile` 回放 `activate`；后台执行、失败不阻塞启动，reason 如实展示。
- **身份证件/人员域扩展**：人员域对象、权限、审计、双端同步均未实现（顶层设计目标项）。

Android 端的注册能力当前应表述为“**数据层与会话可用、注册 UI 已接线，但逐帧特征源缺失导致注册闭环不可用（Blocker，待补）**”，不得宣称注册闭环已可用。

## 4. 安全与隐私约束（已落地部分）

- 不存照片、不存原始帧；`EnrollmentSession` 不接触像素，仓库只落加密模板与元数据。
- 模板密文与注册表元数据分离；注册表不含向量。
- Keystore 密钥不可用/损坏时受控失败，不新建密钥强行解密。
- 错误消息只含状态码语义与相对文件名，不含模型/模板内容。
- API < 23 只读降级，写操作显式 `UNSUPPORTED_PLATFORM`。

## 5. 待确认 / 风险

- 注册表 userId 可逆编码（hex）是刻意取舍，若合规升级要求标识不可逆，需同步改造 `list` 展示策略。
- 模板双端互通仍为研究项，Android 与 Windows 模板不假设可直认。
- 低版本只读模式下注册入口的门禁已在 `MainActivity.startEnrollment`（`:3804`，`AndroidEnrollmentRepository.isEnrollmentEnabled`）实现：点击后返回 `unsupportedReason()` 的受控失败；如要求进入前即置灰按钮，仍待 UI 层补充。
