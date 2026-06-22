# 人员注册与权限系统顶层设计（Win + Android 双端）

- 日期：2026-06-22
- 状态：已确认设计，待实现规划
- 适用范围：`rk3288_opencv` 项目 Windows 本地服务、Web SPA、Android App、共享 C++ 识别核心
- 设计目标：在不引入企业级重型平台的前提下，建设一套具备生产实践能力、支持特殊场景扩展、覆盖 Win/Android 双端的人员注册与权限系统

## 1. 背景与目标

当前项目已经具备以下基础能力：

- Windows 侧已经形成 `本地服务 + Web SPA` 主线，提供本地 REST API、配置热重载、识别结果展示、模型管理等能力。
- Android 侧已经具备摄像头权限状态机、安全模式、加密模板存储等能力，可承担现场采集和轻量值守任务。
- C++ 侧已有共享的人脸检测、识别、跟踪与注册原型，但当前人员数据模型仍过于简单，只有 `personId + 均值模板 + 样本数`，无法支撑完整人员生命周期和权限治理。

本设计的目标不是建设企业级统一身份平台，而是在现有架构内补齐“可上线、可审计、可维护、可扩展”的双端人员系统，满足以下业务约束：

- 实际落地场景操作员数量在 10 人以内。
- 当前以本地部署为主，但后续需要具备向多站点、特殊设备、离线场景扩展的能力。
- 合规要求以“基础本地合规”为基线，即最小化采集、本地优先、模板加密、可删除、可审计。

## 2. 设计原则

### 2.1 设计边界

- 第一阶段只解决本地闭环，不做企业级 SSO、LDAP、OAuth、统一组织中心。
- 第一阶段不做多站点实时同步，但所有核心对象预留 `orgId`、`siteId`、`policyVersion` 字段。
- 第一阶段不做复杂审批流引擎，但保留“待确认”状态以支撑未来双人审核或主管审批。

### 2.2 设计原则

- 贴合现有技术栈：继续使用 Windows 本地服务、Web SPA、Android App、共享 C++ 核心，不引入额外后端平台。
- 域模型先行：先把人员、模板、角色、策略、审计对象建清楚，再扩展界面和接口。
- 前后端双重约束：前端做可见性和交互限制，后端做最终权限判定，避免接口被本机脚本绕过。
- 双端能力分层：Windows 负责完整管理闭环，Android 负责现场采集与受限操作，避免移动端承担高风险管理职责。
- 最小必要原则：只采集和保存必要信息，不引入当前场景用不到的复杂字段。

## 3. 实际基础信息与核心需求拆解

### 3.1 用户规模与部署假设

- 操作员规模：10 人以内。
- 部署形态：本地部署为主，Windows 设备是管理主控端，Android 设备可作为现场采集端和值守端。
- 人员规模：第一期按轻量级人员库设计，但数据模型需要可扩展到后续更大库容。
- 网络条件：默认支持弱联网或离线工作，不依赖持续在线的中心服务。

### 3.2 业务角色类型

建议采用 5 类操作角色，满足当前场景即可：

- 系统管理员：管理角色、策略、关键配置、密钥轮换和高风险动作。
- 安全管理员：负责冻结/删除、导入导出审批、异常核查和审计查看。
- 注册员：负责建档、采集、注册提交、补录和重复人处理。
- 值守员：负责识别结果查看、现场异常处理、基础告警确认。
- 审计员：只读查看关键日志和变更记录，不参与业务修改。

### 3.3 数据合规要求

- 人脸模板与人员主数据分离存储。
- 模板默认加密落盘，禁止长期明文保存。
- 删除、冻结、导出、角色变更必须有审计记录。
- 仅保存必要的人员字段，不默认采集超范围个人敏感信息。
- Android 端只保留必要的本地缓存或加密模板，不保留完整策略主库。

### 3.4 核心需求拆解

系统需求拆为三大核心模块：

1. 人员全生命周期注册流程
2. 多维度权限管控模型
3. 与现有业务系统的集成对接

#### A. 人员全生命周期注册流程

- 支持建档、采集、质检、模板生成、重复人检测、确认入库、生效、冻结、删除。
- 支持 Win 与 Android 任一端发起采集会话，但模板主库由 Windows 本地服务统一管理。
- 支持离线采集缓存、失败重试和异常回退。

#### B. 多维度权限管控模型

- 基于 `RBAC + ABAC` 混合模型进行权限控制。
- 区分端类型、设备可信度、对象范围、人员状态、操作风险级别。
- 高风险操作默认只允许 Windows 端执行。

#### C. 集成要求

- 与现有 Windows 本地 REST API 集成。
- 与 Web SPA 管理页面集成。
- 与 Android 现有权限状态机、识别链路和模板加密能力集成。
- 与共享 C++ 人脸识别能力集成，保证 Win/Android 双端状态语义一致。

## 4. 总体架构设计

### 4.1 总体结构

```text
[Web SPA / Android UI]
  |- Windows: 人员中心 / 注册工作台 / 权限与审计 / 系统设置
  |- Android: 登录 / 待采集列表 / 注册采集 / 识别值守 / 受限操作

                |
                v

[Local API / Platform Adapter]
  |- Windows: HttpFacesServer 扩展人员、权限、审计接口
  |- Android: Activity/ViewModel + JNI + 本地会话代理

                |
                v

[Domain Services]
  |- PersonnelService
  |- EnrollmentService
  |- AccessControlService
  |- AuditService
  |- SessionService

                |
        +-------+-------+
        |               |
        v               v

[Recognition Core]   [Policy / Session Core]
  |- FramePipeline     |- 本地账户
  |- IRecognizer       |- 角色绑定
  |- BioAuth/Engine    |- ABAC 条件规则
  |- 质量评估组件       |- 会话校验

                |
                v

[Local Stores]
  |- personnel_store
  |- biometric_store
  |- authz_store
  |- audit_store
  |- config.json（现有）
```

### 4.2 前后端分层

#### Windows 端

- UI 层：Web SPA，负责完整管理操作和审计查看。
- API 层：继续使用本地 `HttpFacesServer` 作为统一入口。
- 服务层：新增人员、注册、权限、审计、会话模块。
- 数据层：继续本地存储，配置与模板分离。

#### Android 端

- UI 层：Activity/Fragment 或现有页面体系，负责登录、采集、值守、受限确认。
- 平台层：处理摄像头权限、前后台状态、JNI 调用、安全模式。
- 服务层：轻量本地会话代理、采集会话执行器、离线缓存管理。
- 数据层：仅保存必要缓存和加密模板，不保留完整管理主数据。

### 4.3 核心服务模块划分

#### PersonnelService

- 管理人员主档：创建、更新、冻结、删除、状态查询。
- 维护人员与模板、人员与组织、人员与分组之间的关系。

#### EnrollmentService

- 发起并跟踪采集会话。
- 执行人脸样本质量检测。
- 触发模板生成、重复人检测和注册回写。

#### AccessControlService

- 维护角色、权限码、ABAC 条件规则。
- 负责接口授权判定。
- 负责 Win/Android 双端能力裁剪。

#### AuditService

- 记录关键操作日志。
- 支持按人员、操作者、设备端、动作类型检索。
- 为误操作追踪、导出审批、删除溯源提供依据。

#### SessionService

- 提供轻量登录、会话续期、会话失效。
- 提供当前账号、角色、策略版本快照。

## 5. 领域对象设计

### 5.1 人员主数据对象

`PersonProfile`

- `personUuid`
- `personId`
- `displayName`
- `alias`
- `groupId`
- `department`
- `status`
- `createdAt`
- `updatedAt`
- `createdBy`
- `orgId`
- `siteId`
- `remark`

说明：

- `personUuid` 是系统内部不可变主键，用于支持后续改名、编号调整、重复人合并、跨端同步和审计追溯。
- `personId` 是业务可读标识，默认保持唯一，但允许在受控条件下修改；所有内部关联和模板绑定均以 `personUuid` 为准。

### 5.2 生物模板对象

`BiometricProfile`

- `personUuid`
- `personId`
- `templateId`
- `embeddingDigest`
- `sampleCount`
- `qualityScore`
- `modelId`
- `modelVersion`
- `templateSchemaVersion`
- `lastEnrolledAt`
- `enrollmentDevice`
- `isPrimary`

### 5.3 操作员对象

`OperatorAccount`

- `operatorId`
- `loginName`
- `status`
- `roles[]`
- `allowedSites[]`
- `allowedDevices[]`
- `lastLoginAt`
- `passwordHash` 或本地口令绑定摘要

### 5.4 策略对象

`PolicySnapshot`

- `policyVersion`
- `roleBindings`
- `permissionMap`
- `attributeRules`
- `riskActions`
- `androidRestrictedActions`

### 5.5 审计对象

`AuditRecord`

- `recordId`
- `operatorId`
- `action`
- `targetType`
- `targetId`
- `result`
- `reasonCode`
- `deviceType`
- `deviceId`
- `createdAt`
- `details`

## 6. 人员注册全流程状态流转设计

### 6.1 状态定义

- `Draft`：人员档案刚创建，尚未具备注册条件。
- `PendingEnrollment`：等待发起采集。
- `Enrolling`：已进入采集会话，系统正在收集样本。
- `DuplicateSuspected`：检测到高相似历史人员，等待人工判断是否重复人或合并。
- `SyncPending`：Android 离线采集完成，等待回传 Windows 主控端。
- `SyncFailed`：离线包回传失败，需要重试或人工处理。
- `PendingReview`：命中高风险规则或策略要求人工确认，等待确认。
- `ReviewRejected`：人工审核驳回，需要重新采集或修正档案。
- `Active`：注册完成，可参与识别。
- `Suspended`：暂时冻结，不参与正常通行或授权识别。
- `PendingDeletion`：进入删除保护期。
- `Deleted`：模板和档案已删除，仅保留最小审计信息。

### 6.2 状态流转图

```text
Draft
  -> PendingEnrollment
  -> Enrolling
  -> DuplicateSuspected
  -> SyncPending
  -> SyncFailed
  -> PendingReview
  -> ReviewRejected
  -> Active
  -> Suspended
  -> Active
  -> PendingDeletion
  -> Deleted
```

### 6.3 关键流转规则

- `Draft -> PendingEnrollment`：必须具备最小建档字段。
- `PendingEnrollment -> Enrolling`：操作员发起采集会话，系统分配 `sessionId`。
- `Enrolling -> Active`：默认策略下，样本数量达标、质量检查通过、未命中重复人风险，允许自动入库并直接生效。
- `Enrolling -> DuplicateSuspected`：重复人检测命中高相似目标，需要人工判定是合并、覆盖还是重新注册。
- `Enrolling -> PendingReview`：命中高风险规则、策略要求人工确认、或现场端回传需要最终复核时进入待审。
- `Enrolling -> SyncPending`：Android 离线采集完成后，生成加密回传包等待主控端导入。
- `SyncPending -> PendingReview`：离线包成功回传到 Windows 后，由主控端完成最终校验。
- `SyncPending -> SyncFailed`：离线包导入失败、签名校验失败、版本不兼容时进入失败状态。
- `SyncFailed -> PendingEnrollment`：问题处理后重新发起采集或重新回传。
- `DuplicateSuspected -> PendingReview`：人工确认需要继续审核后进入待审。
- `DuplicateSuspected -> Active`：人工确认不是重复人，允许入库生效。
- `DuplicateSuspected -> Draft`：人工确认档案错误或需要合并，回退建档阶段处理。
- `Enrolling -> PendingEnrollment`：采集超时、多脸、遮挡、质量不足时回退。
- `PendingReview -> Active`：确认入库成功。
- `PendingReview -> ReviewRejected`：人工审核驳回。
- `ReviewRejected -> PendingEnrollment`：修正档案或重新采集后再次进入注册流程。
- `Active -> Suspended`：人员临时停用或风险控制触发。
- `Suspended -> Active`：管理员恢复。
- `Active/Suspended -> PendingDeletion -> Deleted`：删除采用两阶段处理，降低误删风险。

### 6.4 双端注册流程

#### Windows 主控端流程

1. 注册员创建或选择人员档案。
2. 发起注册采集会话。
3. 预览流内进行质量检测与连续稳定帧筛选。
4. 调用现有识别器生成模板。
5. 执行重复人检测。
6. 若质量合格且未命中重复人风险，则默认允许自动入库，直接写入模板主库并转为 `Active`。
7. 若命中重复人风险或策略要求人工确认，则转入 `DuplicateSuspected` 或 `PendingReview`。
8. 全流程写入审计日志。

#### Android 现场端流程

1. 操作员登录并获取待采集任务或创建现场采集任务。
2. 通过 Android 摄像头权限和安全模式校验。
3. 启动本地采集会话并通过 JNI 复用共享 C++ 识别能力。
4. 本地完成质量检测与模板暂存。
5. 若在线，回传 Windows 主控端入库；若离线，则生成加密缓存包等待回传。
6. 在线模式下，若未命中重复人和高风险规则，允许主控端自动入库；否则转待审。
7. 离线模式下，回传成功后由 Windows 主控端完成最终校验，可按策略自动入库或转人工审核。
8. 全流程记录 Android 端和 Windows 端双侧审计事件。

### 6.5 注册质量门槛

当前项目尚未实现注册前质量门槛，第一期必须补齐以下检查：

- 单人脸约束：同一帧只允许一个主要人脸。
- 清晰度阈值：模糊图像直接拒绝。
- 光照阈值：过暗、过亮或强背光拒绝。
- 姿态阈值：偏转角过大拒绝。
- 连续稳定帧：避免抓拍单帧误入库。
- 重复人检测：与现有人员库做相似度比对，命中时转人工确认。

## 7. RBAC + ABAC 混合权限模型设计

### 7.1 RBAC 角色定义

- `system_admin`
- `security_admin`
- `registrar`
- `operator`
- `auditor`

### 7.2 权限码建议

- `personnel:create`
- `personnel:update`
- `personnel:enroll`
- `personnel:activate`
- `personnel:suspend`
- `personnel:delete`
- `personnel:export`
- `authz:manage`
- `audit:read`
- `faces:read`
- `system:rotate_key`
- `system:clear_db`
- `system:config_write`

### 7.3 ABAC 条件维度

- `deviceType`：`win` / `android`
- `deviceTrustLevel`：可信设备 / 普通设备 / 临时设备
- `siteId`
- `groupScope`
- `objectStatus`
- `riskLevel`
- `isOffline`
- `timeWindow`
- `isCreator`

### 7.4 典型判定规则

- `registrar` 可在 Win/Android 端发起采集，但无权执行 `system:clear_db`。
- `operator` 只能查看识别与处理低风险现场事件。
- `security_admin` 可冻结和删除人员，但密钥轮换仍只允许 `system_admin`。
- Android 端默认禁止高风险动作：批量导出、角色变更、清库、密钥轮换。
- 离线 Android 端仅允许采集缓存，不允许执行删除、导出、策略修改。

### 7.5 鉴权执行方式

- 前端根据当前会话的权限快照控制页面入口可见性。
- 后端对每个接口做最终权限校验。
- 高风险接口需要记录更细粒度审计信息。
- 后续若启用特殊场景模式，可通过策略开关局部放宽 Android 端能力，但必须显式配置。

### 7.6 账号初始化与离线鉴权闭环

#### 首个管理员初始化

- 系统首次启动时，由 Windows 主控端进入启动向导。
- 启动向导负责创建首个 `system_admin` 账号，并初始化本地鉴权配置、口令摘要、设备信任标记和基础策略。
- 在首个管理员创建完成前，高风险接口默认不可用，避免系统处于“无主但可操作”状态。
- 首个管理员创建完成后，生成初始化审计记录，并写入本地引导完成标记。

#### 本地登录模型

- Windows 端采用本地账号 + 口令摘要校验的轻量登录模型。
- Android 端采用本地登录令牌 + 设备绑定 + 权限快照的受限登录模型。
- 所有会话都必须带 `sessionId`、`operatorId`、`policyVersion`、`expiresAt`。

#### Android 离线鉴权规则

- Android 在线登录后，从 Windows 主控端获取签名的权限快照，快照中包含：
  - `operatorId`
  - `roles`
  - `allowedActions`
  - `deviceId`
  - `issuedAt`
  - `expiresAt`
  - `policyVersion`
- 离线状态下，Android 只允许在快照有效期内执行受限动作。
- 一旦快照过期、设备不匹配、策略版本不匹配或签名校验失败，Android 自动降级为只读或安全模式。
- 离线模式下，Android 仅允许登录后继续执行低风险动作和采集缓存，不允许执行角色变更、删除、导出、清库、密钥轮换等高风险操作。

#### 特殊场景说明

- 若现场确有需要在 Android 端放宽能力，必须通过 Windows 主控端显式下发策略，并记录审计。
- Android 永远不作为首个管理员创建端，也不作为策略主编辑端。

## 8. 数据安全与合规保障方案

### 8.1 存储分层

第一期建议将存储拆为 4 类：

- `personnel_store`：人员主档和状态
- `biometric_store`：模板与样本元数据
- `authz_store`：账号、角色、策略、权限快照
- `audit_store`：审计和风险记录

这样做的原因是避免继续把全部语义堆叠到现有 `FaceDatabase` 单文件结构中，降低后续扩展成本。

### 8.2 加密与保护

- Windows 端：继续沿用现有 DPAPI 绑定当前 Windows 用户的方式保护敏感密钥，并对模板文件做 AES-GCM 加密。
- Android 端：继续沿用 AndroidKeyStore + AES-GCM，对本地缓存模板或离线包进行加密。
- 前端页面不保存模板明文，不缓存敏感字段。

### 8.3 最小化采集

- 默认不采集身份证号、家庭住址等当前场景无关字段。
- 只保存注册必要信息：标识、名称、分组、模板、必要审计。
- 临时采集帧仅用于当前注册会话，超时后删除。

### 8.4 删除与回滚

- 删除采用两阶段：`PendingDeletion` 保护期 + `Deleted` 物理删除。
- 审计记录保留最小必要信息，不随业务删除一起清空。
- 模板误删回滚仅限保护期内恢复，超过保护期则视为正式清除。

### 8.5 审计要求

以下操作必须记录审计：

- 登录、登出
- 建档、注册、激活
- 冻结、恢复、删除
- 角色变更、策略变更
- 导入、导出
- 密钥轮换、清库
- Android 离线采集包生成与回传

### 8.6 数据迁移与兼容策略

#### 唯一事实来源

- 人员主数据的唯一事实来源是 `personnel_store`。
- 模板主数据的唯一事实来源是 `biometric_store`。
- 现有 `FaceDatabase` 不再继续扩展为人员系统主库，而是降级为：
  - 旧版本导入来源
  - 兼容阶段的运行时缓存或派生索引

#### 主键与兼容规则

- 新系统内部统一使用 `personUuid` 作为不可变主键。
- 旧 `personId` 作为业务标识保留，并在迁移时为每个历史条目分配新的 `personUuid`。
- 历史识别结果、审计记录、模板记录都需要支持 `personUuid + personId` 双字段兼容，以保证老数据可追溯。

#### 迁移阶段划分

1. 阶段一：导入兼容
- 系统首次发现只有旧 `FaceDatabase` 而无新存储时，执行一次性导入。
- 导入规则：每个旧 `PersonEntry` 生成一个 `personUuid`，写入 `personnel_store` 和 `biometric_store`。
- 导入完成后保留原始库备份和迁移清单。

2. 阶段二：新库主写
- 所有人员注册、冻结、删除、合并、改名都只写入新存储。
- 识别运行期如仍依赖旧型索引结构，则由新存储派生生成兼容运行时缓存，不再反向以旧库存储为准。
- 禁止长期双写两套主库，避免数据分叉。

3. 阶段三：兼容退出
- 当识别链路完全切换到新存储适配层后，`FaceDatabase` 仅保留导入工具角色。

#### 回滚策略

- 导入前备份原始 `FaceDatabase`。
- 导入后生成迁移清单，记录 `personUuid <-> personId` 映射。
- 若新存储加载失败或迁移中断，系统回退到只读兼容模式，继续使用旧库提供识别能力，并阻止新注册写入，直到迁移修复完成。
- 回滚期间不得让 Android 端继续回传自动入库，避免新旧数据继续分叉。

## 9. 与现有系统的集成对接方案

### 9.1 与 Windows 本地服务集成

- 保持 `HttpFacesServer` 为统一 API 入口。
- 在现有 `/api/v1/settings`、`/api/v1/models`、`/api/v1/faces` 之外新增：
  - `/api/v1/auth/*`
  - `/api/v1/personnel/*`
  - `/api/v1/enrollment/*`
  - `/api/v1/roles/*`
  - `/api/v1/audit/*`
- 新增启动引导接口：
  - `/api/v1/setup/bootstrap`
- 高风险接口统一接入授权检查。

### 9.2 与 Web SPA 集成

新增页面建议：

- 人员中心：列表、查询、状态维护。
- 注册工作台：建档、采集、质检反馈、重复人确认。
- 权限与审计：角色查看、审计检索、风险事件查看。
- 保持现有设置页、模型页和预览页能力不回归。

### 9.3 与 Android App 集成

Android 端新增或补齐以下能力：

- 本地登录与会话管理。
- 待采集任务列表。
- 现场采集流程。
- 离线缓存包管理。
- 轻量识别和值守能力。
- 签名权限快照校验与过期处理。

Android 端不作为主配置和主策略维护端，仅作为受限执行端。

### 9.4 与共享 C++ 核心集成

- Win/Android 双端尽量复用同一套识别和质量评估能力。
- 人员状态枚举、错误码、审计事件码采用统一定义，避免双端语义分裂。
- 对现有 `BioAuth`、`Engine`、`FramePipeline`、`IRecognizer` 做接口层补充，而不是复制两套识别逻辑。

## 10. 里程碑拆分与阶段交付计划

### 里程碑 M1：领域建模与接口骨架

交付内容：

- 人员、模板、账号、策略、审计对象模型
- Win/Android 共享状态码、错误码、事件码
- Windows 端 API 骨架
- `personUuid` 引入与旧库迁移清单设计
- 启动向导与首个管理员初始化流程

验收标准：

- 可以完成人员建档和列表查询
- 可以通过启动向导创建首个管理员
- 可以识别并导入旧 `FaceDatabase` 为新模型
- 现有识别、预览、配置功能不回归

### 里程碑 M2：Windows 主控端闭环

交付内容：

- 人员中心
- 注册工作台
- 角色管理
- 审计查询
- 接口鉴权

验收标准：

- Windows 端能完成 `建档 -> 采集 -> 确认 -> 生效 -> 冻结/删除` 闭环
- 高风险接口已受控

### 里程碑 M3：Android 现场端闭环

交付内容：

- 登录与会话
- 待采集任务
- 现场注册采集
- 轻量识别与值守
- 离线回传
- 离线权限快照校验

验收标准：

- Android 端可在受限权限下发起采集并回写结果
- 离线状态下仅能在快照有效期内执行受限动作
- 权限不足时能够明确拦截并给出原因

### 里程碑 M4：特殊场景增强

交付内容：

- 加密导入导出
- 重复人检测增强
- 特殊设备模式开关
- 风险动作双确认扩展点

验收标准：

- 支持离线采集后回传
- 支持特殊场景受控扩展而不破坏主流程

## 11. 核心功能验收标准

### 11.1 功能验收

- 人员可在 Win 或 Android 端发起注册流程。
- 默认策略下，质量合格且未命中重复人风险时允许自动入库。
- 注册成功后状态正确变为 `Active`，失败时有明确错误码和回退状态。
- 未授权账号不能执行高风险接口。
- Android 端默认不能执行清库、密钥轮换、角色变更等高风险动作。
- 删除、导出、角色变更均可追溯到操作者和设备端。
- 系统首次启动时可通过启动向导创建首个管理员。
- 历史 `personId` 数据迁移后可稳定映射到新的 `personUuid`，且不影响现有识别能力。

### 11.2 非功能验收

- 10 人以内操作员并发使用时，权限判定不成为明显瓶颈。
- 模板不以长期明文方式落盘。
- 弱联网或离线情况下，Android 端能够进入安全模式或离线采集模式，不导致崩溃。
- 引入人员系统后，不破坏现有模型管理、预览、识别与设置流程。

## 12. 风险与应对预案

### 风险 1：双端状态语义不一致

问题：

- Windows 与 Android 各自定义状态、错误码和事件码，后续难以联调和排查。

应对：

- 第一阶段就定义共享枚举和错误码契约。
- 统一审计事件编码。

### 风险 2：注册质量不稳定

问题：

- 现场采集光照、姿态、多人脸等条件不稳定，容易造成误入库。

应对：

- 强制启用质量门槛。
- 引入连续稳定帧与重复人检测。

### 风险 3：权限只做前端控制

问题：

- 本机脚本可直接调用接口绕过前端限制。

应对：

- 所有敏感接口以后端鉴权为准。
- 高风险动作写强审计。

### 风险 4：Android 端权限过大

问题：

- 现场端一旦可执行高风险动作，误操作成本高。

应对：

- Android 默认做受限端。
- 高风险动作只在 Win 端开放，特殊场景需显式策略开关。

### 风险 5：继续沿用单一模板文件模型导致难扩展

问题：

- 当前 `FaceDatabase` 模型太薄，不适合继续承载人员、策略、审计等完整语义。

应对：

- 将人员主档、模板、权限、审计拆仓存储。
- 对旧结构仅做兼容迁移，不继续扩展其职责。

### 风险 6：离线 Android 权限快照失效后仍继续执行

问题：

- Android 端若没有有效期、签名和设备绑定校验，离线状态下可能长期持有过期权限。

应对：

- 对权限快照加入签名、设备绑定和过期时间。
- 快照失效后自动降级为只读或安全模式。

### 风险 7：自动入库放大误注册风险

问题：

- 默认允许自动入库后，如果质量门槛或重复人检测不稳，误入库风险会提高。

应对：

- 自动入库仅在质量门槛通过且未命中重复人规则时启用。
- 命中高相似度、离线回传异常、高风险策略时强制转人工审核。

## 13. 结论

本设计采用“本地域模型 + 可扩展策略层”的方案，以 Windows 本地服务为主控端、Android 为受限现场端，围绕人员全生命周期、RBAC + ABAC 混合权限、模板加密和审计留痕构建一套轻量但具备生产实践能力的双端系统。该方案能够在不引入企业级重型平台的前提下，补齐当前项目从“识别原型”走向“可落地人员系统”的关键缺口，并为后续特殊场景和多站点扩展预留结构化边界。
