<!-- DOCSYNC_DOC
type=plan
status=living
as_of=2026-10-16
-->

# PR #495 全量审计整改

依据 cache/audit-pr495-2026-10-13/audit-report.md 的 62 项记录逐项处理。审计固定基线为 40121ce；整改起点为本地 06118af，已有九项审查修复保留并重新核验。允许自行 commit，禁止 push。

启动时已 fetch origin：工作区干净，本地比功能分支多 1 个提交；master 已 squash 合并该功能分支，当前与 master 的源码差异仅为既有修复。保持本地分支，不重写历史。

已确认缺陷先复现再修复；候选先核验，不将未知数据当作已验证缺陷。用户已确认“1+2”：同时补齐报告列出的未实现功能，包括 templateSpaceId 路由和单图 JNI 消费者。已接受取舍保留；真机、私有身份数据和远程写操作仍待专项授权。核心配置改动先形成可审阅补丁，再按用户规则确认。

每项记录落地、回归与变异证据；恢复变异后才做统一检查和独立复核。缓存内账本记录原始条目、裁定与证据，原始审计报告不覆盖。构建只验证 Windows/MSVC 和 Android；Linux 保留参考 CI。

2026-10-16 检查点：用户已明确接回工作区并批准样例上报地址清空、MobileFaceNet指定父模型及预处理统一。Engine空间竞态和Windows跨运行时登记/清库/旧身份发布已修；F32已通过可复现转换及六项精准变异，不能再列为等待规范。同空间重载的数据库快照登记丢失已实证并由本轮修复：reloadRuntime 持 databaseMutationMu_ 覆盖候选整库 load 与提交，processLoop 仅对会写库的命令取同锁并在 run() 后立即释放（锁序 lifecycleMu_ → databaseMutationMu_ → publicationMu_）；两条真实 FramePipeline.reload 门控回归先 RED（137 通过 / 2 失败）后 GREEN（140/140），精准变异 M1/M2 各致 3 条确定红。统一检查：全目标构建 rc=0、ctest 6/6、node --test 202/202、Web 156 条、docs H0/M0/L10、仓库卫生 rc=0 均通过；图谱已刷新。已知边界：移除"取锁后运行时复核"无判别锚（已登记）；Cypress 二进制启动仍受阻，未作真机/真人精度/外网推流验收。本轮尚未 commit，不 push。

## 逐项账本

| 编号 | 审计类别 | 优先级 | 问题 | 当前裁定 |
|---|---|---|---|---|
| F-0001 | CONFIRMED_DEFECT | P1 | CSRF/反 DNS-rebinding 防御不完整：从不校验 Host，且非 cross-site 的 Sec-Fetch-Site 一律放行 | 已修复；实际HTTP与Host变异通过，本轮202脚本和正式core/Windows回归通过 |
| F-0002 | CONFIRMED_DEFECT | P2 | parseRegistryV2 的 declared==null 分支漏置 authoritative=false，破坏性对账在不完整集合上运行 | 已复核当前非法COUNT分支authoritative=false；既有精准变异及Gradle401条覆盖保留 |
| F-0003 | CONFIRMED_DEFECT | P2 | 检测「真实精度」程序跨架构比较且不计算任何检测指标 | 已实现真实同父模型检测一致性；正式工具+配对21条及6个变异通过；不代表真实权重精度 |
| F-0004 | CONFIRMED_DEFECT | P2 | 检测精度程序未被 CI「Run INT8 precision tests」步骤执行；add_test 缺 WORKING_DIRECTORY 致静默 SKIP=PASS | 构建/CI补丁已获批应用；工具缺资产exit77，正式工具回归通过；待远程CI（不push） |
| F-0005 | CONFIRMED_DEFECT | P2 | PerfReporter 的性能指标只测到解码耗时（端到端 P95/内存峰值通道失效） | 真实阶段/RSS与私有目录已修；Engine六字段变异及Reporter九变异均检出，正式推理回归通过 |
| F-0006 | CONFIRMED_DEFECT | P2 | 加速自检 effective 由编译宏+env 推导，忽略运行期初始化结果（可产生假 ok） | 真实运行证据接线已修；4行为probe/9变异、正式Engine及双ABI22对象编译通过；非真机验收 |
| F-0007 | CONFIRMED_DEFECT | P2 | DNN「粗检」输出被整体丢弃：识别成本不由 stride/背压控制，perf.infer_ms 度量被丢弃的一趟 DNN 前向 | 已移除丢弃SSD前向并实测识别耗时；当前邻近15条与精准变异通过 |
| F-0008 | CONFIRMED_DEFECT | P2 | 文档化的 /openapi.json 别名路径不可达，实际 404（契约漂移） | 已修复；别名内容与认证一致，3条回归及路由/认证变异通过 |
| F-0009 | CONFIRMED_DEFECT | P2 | 前端把后端 status="disabled" 显示为「缺失」（disabled→missing 映射） | 已修复；真实组件与变异覆盖，本轮Web156条/lint/build通过 |
| F-0010 | CONFIRMED_DEFECT | P2 | CI 依赖审计门禁只枚举 high 严重度，Critical 漏洞不阻断 | 用户已批准并落地；实际门禁 11 条与旧缺陷变异通过 |
| F-0011 | CONFIRMED_DEFECT | P2 | 依赖审计门禁在 pnpm audit 输出 schema 变化时静默报 Audit OK | 用户已批准并落地；schema 漂移明确阻断，实际门禁 11 条通过 |
| F-0012 | CONFIRMED_DEFECT | P2 | int8Enabled 仅为 manifest 声明布尔，无量化层/运行时/编译支持校验；头注释声称的一致性判据不存在 | 结构/编译/实际后端一致性已修并抽共享guard；本轮正式推理/精度契约通过，既有变异保留 |
| F-0013 | CONFIRMED_DEFECT | P2 | templateSpaceId 全无生产消费（空间路由未接线）；两处 C++ 注释引用不存在的 replaceGalleryForSpace | 空间路由、Engine竞态及Windows命令/输出屏障已修；同空间重载的候选整库 load 与在途登记/清库帧的 saveDb 竞争（登记丢失/清库复活）由独立复核实证并已修复：reloadRuntime 持 databaseMutationMu_ 覆盖候选装配与提交，processLoop 仅对会写库的命令取同锁、run() 后立即释放（锁序 lifecycleMu_ → databaseMutationMu_ → publicationMu_）；两条真实 FramePipeline.reload 门控回归 RED(137/2)→GREEN，精准变异 M1/M2 各致 3 条确定红。见 enrollment-database-lock/ |
| F-0014 | CONFIRMED_DEFECT | P3 | .pre-encrypt 留存口径：文档与实现（A11）相反 | 已同步回读验证后删除本次副本及历史副本保护文档 |
| F-0015 | CONFIRMED_DEFECT | P3 | HttpFacesPoster 把完整 postUrl 明文写入 event_log.jsonl | 生命周期日志全脱敏；真实Poster回归和恢复明文日志精准变异均通过 |
| F-0016 | CONFIRMED_DEFECT | P3 | Android 侧日志身份掩码缺口：MainActivity 明文记录 personId | 已修复；日志链路回归与变异通过，Gradle 编译修正后通过 |
| F-0017 | CONFIRMED_DEFECT | P3 | 孤儿旁置门禁用 complete（可为恢复副本）而非权威/主表判据 | 已修复；恢复与孤儿旁置回归及变异通过 |
| F-0018 | CONFIRMED_DEFECT | P3 | README 治理进度与架构契约实证自相矛盾 | 已修正文档治理进度；其余4条契约仍待修复 |
| F-0019 | CONFIRMED_DEFECT | P3 | CI 量化产出的 scrfd_int8_ncnn 无任何程序读取校验 | CI已实际调用SCRFD精度模式，合成量化→推理→指标已验证；真实资产未验收 |
| F-0020 | CONFIRMED_DEFECT | P3 | 注释失真：test_face_infer_pipeline.cpp 描述已删除的 INT8 回落链 | 已更正旧INT8回落链注释，与当前显式装配一致 |
| F-0021 | CONFIRMED_DEFECT | P3 | Cypress E2E 断言已失效且从不连真实后端，属「空心端到端」 | mock 冒烟已修复；真实 Chrome 验证通过，Cypress 二进制启动受阻 |
| F-0022 | REVIEW_CANDIDATE | P2 | Windows reloadRuntime 失败不保留旧运行态（与「load 失败旧状态保持」相悖） | 失败保旧运行态、首次失败和HTTP本次结果已修；正式Windows及精准变异通过 |
| F-0023 | REVIEW_CANDIDATE | P2 | profile/模型切换不隔离内存图库空间 ⇒ 跨嵌入空间复用模板 | 空间路由、Engine竞态及Windows命令/输出屏障已修；同空间重载的候选整库 load 与在途登记/清库帧的 saveDb 竞争（登记丢失/清库复活）由独立复核实证并已修复：reloadRuntime 持 databaseMutationMu_ 覆盖候选装配与提交，processLoop 仅对会写库的命令取同锁、run() 后立即释放（锁序 lifecycleMu_ → databaseMutationMu_ → publicationMu_）；两条真实 FramePipeline.reload 门控回归 RED(137/2)→GREEN，精准变异 M1/M2 各致 3 条确定红。见 enrollment-database-lock/ |
| F-0024 | REVIEW_CANDIDATE | P3 | 检索只挡 NaN 不挡 Inf：Inf 分量可致分数为 NaN，排序退化为索引序 | 已修复；实际检索回归与旧缺陷变异通过 |
| F-0025 | REVIEW_CANDIDATE | P3 | 身份哨兵大小写不一致：LBPH 的 "unknown" 小写不被 TrackCoordinator 视作未认证 | 已修复；小写未知身份回归与变异通过 |
| F-0026 | REVIEW_CANDIDATE | P3 | 已确认的框在身份累计中（1-2/3）时被上报为 AUTH_FAIL Unknown | pending/隐藏轨迹已修；两个旧缺陷变异及后续正式Engine/推理回归通过 |
| F-0027 | REVIEW_CANDIDATE | P3 | Engine 侧不校验图库模板空间与当前 embedder 是否兼容（keepIds 仅按字符串） | 严格换库校验、检测前代次与同对象新快照撤销已修；正式Engine44条、两竞态变异及独立复核通过 |
| F-0028 | REVIEW_CANDIDATE | P3 | 单图路径的 consecutivePassesToTrigger（--consecutive N>1）实际不生效 | 已修复；单图 N>1 明确拒绝，行为回归与变异通过 |
| F-0029 | REVIEW_CANDIDATE | P3 | SCRFD 装配缓存键未含 decodeMode/anchorsPerPos/outputLayout ⇒ 同 profileId+同 sha256 下改这些字段会误复用旧实例 | 已复核SCRFD完整解码缓存键；当前正式推理回归与既有精准变异覆盖 |
| F-0030 | REVIEW_CANDIDATE | P3 | 校准几何与生产 letterbox 不一致（ncnn2table 无 letterbox 表达手段） | 候选确认并修复：生产letterbox离线校准；工具回归与几何变异通过 |
| F-0031 | REVIEW_CANDIDATE | P3 | ArcFace 预处理在精度程序用 1/128、生产契约与另一测试用 1/127.5（口径分裂） | 已修复；真实预处理与合成 ncnn forward 契约通过，未声明真实权重精度 |
| F-0032 | REVIEW_CANDIDATE | P3 | 校准脚本 mobilefacenet 预设与生产 MobileFaceNetAdapter 预处理不一致 | 用户已认可指定w600k_mbf父图及RGB/(x-127.5)/127.5；生产/校准已统一，六变异检出，fp16转换双文件逐字节复现。旧模板须重登记、旧INT8须重校准，真人精度未验收 |
| F-0033 | REVIEW_CANDIDATE | P3 | profile 的 identitySimilarity 未透传到单次管线 acceptThreshold | 已复核manifest阈值与显式CLI覆盖标记；当前正式推理回归与既有精准变异覆盖 |
| F-0034 | REVIEW_CANDIDATE | P3 | A11 迁移结果（含删失败/校验失败）无生产消费者，静默 | 已修复；生产日志输出固定结果码，回归与精准变异通过 |
| F-0035 | REVIEW_CANDIDATE | P3 | 崩溃在「备份后、替换前」自造的 .pre-encrypt 会永久阻断后续迁移 | 新pending事务支持一致性重试；历史备份不自动改动，回归与变异通过 |
| F-0036 | REVIEW_CANDIDATE | P3 | FileProvider 路径范围过宽（三条 path="."） | FileProvider仅授权captured_images，两拍照入口同步；声明回归/独立变异与Android构建通过 |
| F-0037 | REVIEW_CANDIDATE | P3 | AndroidManifest 无 INTERNET 权限 ⇒ RTMP 推流实际不可用 | INTERNET已声明；权限回归与独立变异通过；真实推流未做 |
| F-0038 | REVIEW_CANDIDATE | P3 | Windows cfg_ 无锁写、他处持 mu_ 读 ⇒ 数据竞争 | 配置读写同锁；当前正式Windows、邻近15回归及精准变异通过 |
| F-0039 | REVIEW_CANDIDATE | P3 | Windows captureThread_/processThread_ 无互斥保护 | 生命周期串行与重复初始化保护已修；三方锁序/回调拒重载回归及对应变异检出 |
| F-0040 | REVIEW_CANDIDATE | P3 | Windows 回调持 renderMu_ 执行（再入隐患，当前无生产使用者） | 回调在render锁外且受发布屏障保护；只读重入与旧身份拒绝已独立复核，正式Windows通过 |
| F-0041 | REVIEW_CANDIDATE | P3 | VideoManager::getLatestFrame 不复位 hasNewFrame ⇒ 引擎线程可能忙循环 | 同锁消费一次/关闭清残帧已修；正式face推理回归及独立变异通过 |
| F-0042 | REVIEW_CANDIDATE | P3 | JNI GetStringUTFChars 挂起异常未清（两处，与 nativeInit 不一致） | 两JNI域异常处理已修；行为/精准变异、Gradle401条及最新双ABI22对象编译通过 |
| F-0043 | REVIEW_CANDIDATE | P3 | MppDecoder 输入缓冲可能在 decode_get_frame 消费前释放 | 原生命周期候选经SDK实际同步复制反证；关联API/pos/NV12缺陷已修并验证；不代表真机MPP通过 |
| F-0044 | REVIEW_CANDIDATE | P3 | GET /api/v1/models 可返回 status="disabled"，但 openapi.yaml 枚举只列 loaded/failed/missing | 已修复；前端类型与 OpenAPI 契约及变异通过 |
| F-0045 | REVIEW_CANDIDATE | P3 | GET /api/faces 无帧时以 pipeline_unavailable 报错，与同类端点 frame_unavailable 不一致 | 无帧与空pipeline错误码已区分；真实HTTP及恢复旧错误码精准变异检出 |
| F-0046 | REVIEW_CANDIDATE | P3 | poster 默认 postUrl 指向 GET 端点；INI/env 来源的 postUrl 不经 scheme 校验 | 结构体/INI缺键/样例默认已清空，样例单行获批；控制字节/DEL/零端口/IPv6边界已修，真实Poster及三精准变异通过 |
| F-0047 | REVIEW_CANDIDATE | P3 | AppStore.refreshServerSettings 过期分支返回 true，布尔语义与「本次是否落地」不一致 | 已修复；真实并发请求回归与旧缺陷变异通过 |
| F-0048 | REVIEW_CANDIDATE | P3 | PerfReporter「Peak RSS」名不副实 + 有锁入队/静默丢样本/p95 口径分裂 | 采样峰值/最近秩/保新1024/drop可见/退出排空已修；6回归9变异并独立复核通过 |
| F-0049 | REVIEW_CANDIDATE | P3 | ncnn FetchContent 默认引用可移动 ref master | 已固定官方release完整SHA，用户已批准并应用CMake/CI接线 |
| F-0050 | REVIEW_CANDIDATE | P3 | RTMP 推流：凭据未脱敏 Toast、回调语义失真、相机源未实现 | 回调/隐私/相机流/JNI独立seq已实现；2回归15变异/双ABI语法/Android桩通过；可选AAR缺失与设备实推未验收 |
| F-0051 | REVIEW_CANDIDATE | P3 | CHANGELOG/CREDITS 的 DOCSYNC as_of 元数据陈旧 | 元数据已按实际变更更新；本轮文档门禁H0/M0/L10 |
| F-0052 | REVIEW_CANDIDATE | P3 | CLAUDE.md 对 unit-tests 跨平台编译检查的范围描述偏大 | 已将Linux参考编译检查范围订正为实际3个文件 |
| F-0053 | REVIEW_CANDIDATE | P3 | CREDITS/ledger 对 SCRFD「代码许可」的归属待取证 | 原Apache-2.0归属经SCRFD子目录LICENSE反证为正确；已补直接上游来源，不改权重许可 |
| F-0054 | REVIEW_CANDIDATE | P3 | win_face_bench_cli / test_face_database_perf 无稳态/暖机口径 | 实际基准暖机/稳态样本/分位已实现；正式工具21条组合回归与暖机变异通过 |
| F-0055 | REVIEW_CANDIDATE | P3 | inference_bench_cli 的 backend 四元组存在写死/错文案 | 后端原因/生效按实际迭代成功数，SNPE未接线诚实报告；正式回归与变异通过 |
| F-0056 | REVIEW_CANDIDATE | P3 | PerfReporter 头注释与实现不符（自称无锁，实为加锁） | 头注释已改短锁有界队列，与实现一致 |
| F-0057 | ACCEPTED_TRADEOFF | P3 | 识别器装配成功时 DNN(SSD) 缺失/失败告警不经 health 顶层暴露 | 保留已接受取舍 |
| F-0058 | CONFIRMED_DEFECT | P3 | DeviceProfile.collect() 的 hasGms 硬编码 false ⇒ GLOBAL_GMS 分支与 isLikelyMainlandDevice 生产恒不可达 | 真实collect按定向可见包查询；存在/缺失/拒绝查询回归与旧恒false变异、Gradle401条通过 |
| F-0059 | CONFIRMED_DEFECT | P3 | 单图路径 nativeInferFaceFromImage / nativeSetInferenceThrottle 在 Android 侧无任何 Java 调用者（死 JNI 面） | 单图选择/拍照后台JNI消费及throttle已接；边界行为/空间变异与Gradle401条通过；真实设备未验收 |
| F-0060 | CONFIRMED_DEFECT | P3 | 桩 native-lib-stub.cpp 的 nativeInit 第三参类型写成 jint（应 jstring），类型漂移无人拦 | 已将JNI桩第三参订正为jstring；跨语言契约RED→GREEN，Android构建通过 |
| F-0061 | REVIEW_CANDIDATE | P3 | checkApkFaceModels 守门未接入任何 CI job 或脚本 | 用户批准CI调用现有APK门禁；实际APK通过，注入合成条目失败，原APK逐字节恢复 |
| F-0062 | CONFIRMED_DEFECT | P3 | win_unit_tests 的 4 条用例对构建目录布局敏感：build dir 在仓库之外时探测不到 cascade 资产而假红 | 两源码根锚已修；仓库外cwd正向4/4，删除两处锚分别触发对应断言失败 |

## 补充：审计 62 项之外的本轮新发现

| 编号 | 来源 | 问题 | 裁定 |
|---|---|---|---|
| N-01 | 独立复核（审计外） | 同 templateSpaceId 重载时候选整库 load 与在途登记/清库帧的 saveDb 无互斥 ⇒ 登记丢失 / 清库复活 | **已修复**（`reloadRuntime` 持 `databaseMutationMu_` 覆盖候选装配与提交；`processLoop` 仅对会写库的命令取同锁、`run()` 后立即释放；锁序 `lifecycleMu_ → databaseMutationMu_ → publicationMu_`）。两条真实 `FramePipeline.reload` 门控回归 RED→GREEN，M1/M2 变异各致 3 条确定红。证据 `cache/audit-fixes-2026-10-13/enrollment-database-lock/` |
| N-02 | 独立复核（审计外） | `test_pipeline_initialize_rejects_live_threads` 只定义未注册 ⇒ 死测试、零覆盖（自 enrollment-runtime 轮次起） | **已修复**：补注册进 `win_unit_tests_main.cpp`，转为真执行 |
| N-03 | 独立复核（审计外） | `processLoop` 取锁后的运行时复核（`rt != runtimeSnapshot()`）当前无任何用例可确定性判别 | **如实登记为无判别锚**（可达性论证见同目录报告 §5），本轮未新增生产接缝 |

## 交付标准

- 所有条目有基于当前代码的裁定，不以旧基线结果直接宣布修复完成。
- 实际缺陷完成最短修复、相应回归及精准变异；候选被推翻时记录反证。
- 运行适用构建与测试，区分跳过、环境阻塞和实际通过；更新已跟踪 Web 产物与图谱。
- 独立复核整改差异，新增相关问题继续修复，直至无新增问题或确实需要用户介入。
- 暂存区敏感信息检查通过后本地提交，最终报告提交、验证与未完成项，不推送。
