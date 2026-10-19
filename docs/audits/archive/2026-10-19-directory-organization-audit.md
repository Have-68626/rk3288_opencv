<!-- DOCSYNC_DOC
type=audit
status=snapshot
as_of=2026-10-19
-->

# 目录结构与组织审计

## 结论

**主体分层合理，建议渐进整理，无需整体搬家或改成另一种仓库模板。** 当前主要偏差是本地构建入口与存储规范不一致、Android默认测试目录留有失活样例、模型目录缺少跨消费者的统一身份视图。脚本分类、重复第三方快照和本地产物保留规则可随后完善。

本轮只审查并提出建议，新增本报告及索引登记；没有执行以下整改，没有移动、删除、重命名源码、依赖、模型或缓存，没有提交、推送或合入master。

## 范围、方法与基线

用户确认范围为“正式目录逐项审查＋忽略目录分类盘点”，交付问题证据、目标布局、实施顺序和验收办法。规范以[仓库存储布局](../../guides/repository-storage-layout.md)、[文档规范](../../DOC_STANDARD.md)和实际构建消费者为准；通用习惯只作为可选建议，不将工具默认名或历史证据路径判为错误。

- 当前分支为 `codex/pr495-review-fixes`，HEAD为 `265d3fc575d649f2ff908bfdfc20a4f086e596fc`。
- 根目录实际有34个目录、27个文件；Git索引685项，分属15个顶层目录及24个根文件。索引仍含上轮已获批删除的两份文档，当前文件系统不存在；不能误报为本轮丢失文件。
- 工作区含上轮文档整改及其他会话的未跟踪版权材料，均按实际状态识别。本轮不修改其正文或代为提交。
- 使用graphify定位关系，再核对Git跟踪/忽略规则、构建配置、脚本、源码路径及必要文件摘要。图谱不是实时事实源。
- 忽略目录只做顶层及重点目录直接子项的分类，不扫描全部二进制内容、依赖内部或其他工作树；未重新统计磁盘总占用，不能据本报告推算可回收空间。
- 未读取原始识别日志、私有媒体或个人记忆正文；版权材料仅列元数据，并为保护现有工作计算字节摘要，未解析或摘录正文。本地证据位于 `cache/reports/directory-organization-audit-2026-10-19/`，仅保存目录元数据、消费者行号、摘要及门禁结果，不作为正式文档的稳定链接。

## 正式目录逐项判断

| 目录或入口 | 当前职责与判断 | 建议 |
|---|---|---|
| 根目录 | README、助手指南、Gradle/CMake配置、Loop固定入口；另有第三方源与本地日志 | 固定入口保留；本地输出和重复源分别按后述R02/R04处理，不按根文件数量机械搬迁 |
| `.github/` | 两份workflow，工具规定路径 | 保留；CI临时目录不必等同本机长期构建目录 |
| `.claude/` | 已跟踪角色、工作流和项目入口；其他本机内容忽略 | 保留加载路径；与其内部其他分支工作树区分，不能整体移动或删除 |
| `app/` | Android模块、manifest/res、Gradle装配与可选AAR入口 | 保留；Java不在默认目录是显式设计，需处理失活测试样例R03 |
| `src/cpp/` | 共享引擎的include/src、JNI和编译工具 | 分层成立；新增独立编译工具优先进入tools，不做全量拆模块 |
| `src/java/` | Android生产Java，Gradle显式指定 | 保留；无需为了Android惯例迁入app/src/main/java |
| `src/win/` | Windows头文件/实现、应用/工具、第三方快照及webroot | 分层成立；webroot是受控分发产物，不按生成文件一概删除 |
| `web/` | SPA源码、Cypress、前端配置、构建/E2E脚本 | 保留；node_modules和覆盖率目录属于工具产物；与根scripts分工要写清 |
| `cmake/` | 构建规则按核心、Android、Windows、OpenCV、推理及契约拆分 | 保留；路径改动须同步模块与测试，而非只改文件系统 |
| `tests/` | cpp、win、unit/java、scripts、fixtures；报告目录忽略 | 布局可用；测试按平台/宿主混合分类不构成缺陷，维持现有运行器入口 |
| `scripts/` | 仓库门禁、构建/验证入口；android、audit、ffmpegkit已分域 | 基本合理；根部23份文件可分批归类，保持被CI/工具调用的入口兼容 |
| `config/` | 旧版Windows INI迁移样例 | 保留；不能代替用户运行态JSON，更不能将运行态配置复制入库 |
| `deps/` | 上游父模型/数据、SDK接入说明及OpenCV测试桩 | 保留职责；deps/opencv是桩，不是完整OpenCV安装，建议增加薄说明避免误用 |
| `models/` | 正式消费模型与Android profile文本；二进制大部忽略 | 保留；补R05身份/消费者映射，不靠文件夹统一推断模型可互换 |
| `docs/` | 按职责组织设计、指南、计划、审计、安全及研究 | 保留现有分类；以元数据和接续链接区分活跃/历史，不批量搬历史快照 |
| `gradle/`及wrapper | 构建引导与固定发行身份 | 保留标准路径及校验值 |
| `patterns/` | 根级registry供Loop使用；docs/patterns是说明 | 两者职责不同，保留，不能按同名合并 |
| `cache/` | 唯一入库文件为README，其余含构建、依赖输入、工具及唯一证据 | 继续分类治理；忽略不等于可删，正式工具应进入scripts而非只留cache |

## 六项具体整改建议

### R01：本地新构建入口尚未遵守统一落点（优先）

证据：[CMakePresets.json](../../../CMakePresets.json)第8/18/28行仍输出到根级build_ci、build_win、build_android_rk3288；[Android包装脚本](../../../scripts/build_android.bat)第65行和[Host包装脚本](../../../scripts/verify_opencv_host.bat)第8行也创建根级构建树，而存储规范约定新独立树进入cache/builds。本机仍有build_fp15_gtest等历史目录，名称无法单独说明工具链、用途和有效期。

最短整改：保留preset名称和已有树，先让新的本地构建统一输出到 `cache/builds/<用途-工具链>/`。包装脚本显式使用仓库根的 `-S` 和目标 `-B`；不能只改BUILD_DIR却保留依赖父目录层数的 `..`。VS、Ninja、不同ABI及mock/真实后端使用不同树。CI runner中的build_ci/build_ci_win属于独立短期契约，可保留，不为目录美观改workflow。

风险与回滚：新目录首次构建会增加时间和临时空间；不要移动含绝对路径的旧CMake树。恢复原preset/脚本即可回退新入口，旧树持续保留至消费者退出。涉及核心配置修改须另行报批。

验收：用独立新目录核对source/binary根、生成器、ABI、OpenCV及ncnn身份；选定目标构建后仅运行对应CTest。确认入口不再新建根级本地树，旧树仍可按原命令使用。

### R02：运行及验证输出的归属仍分散（优先明确规则，迁移另批）

本机根级recognition.csv、recognition.jsonl合计5,733,365逻辑字节，均已忽略；这里只核验元数据，未读内容。当前生产Windows默认日志由[WinJsonConfig.cpp](../../../src/win/src/WinJsonConfig.cpp)第561行解析为exe旁storage/win_logs，**不能据根级同名文件断言当前服务仍错误写根目录**。

另有build/reports、tests/reports、web/tests/reports及cache/reports等产物位置。[Host验证脚本](../../../scripts/verify_opencv_host.bat)第107行仍在仓库根输出opencv_verify_results.csv及verify.log；说明输出布局不完全由一个规则覆盖。

最短整改：生产运行态维持应用既有storage规则；工具默认机器报告留在各自约定目录，跨会话需保留的脱敏证据集中到 `cache/reports/<任务-日期>/`，结论入docs/audits。只改后续工具的输出参数或运行目录，不搬正在写入的日志，不让正式指南稳定链接忽略的原始报告。先核对根日志的作者、保留需求和写入句柄，再决定受控归档或另批删除。

验收：在独立合成输入上检查输出均进入声明目录，仓库根不新增运行/验证文件；真实日志只做访问控制和保留确认，不作为公开fixture或提交内容。该建议不授权读取或迁移原始日志。

### R03：Android默认测试目录保留不参与测试的样例（优先）

[app/build.gradle](../../../app/build.gradle)第119/123行将生产Java指定为src/java、测试指定为tests/unit/java，但 `app/src/test/java/com/example/rk3288_opencv/DummyTest.java` 仍被跟踪，仅含assertTrue(true)。它不在当前test源集合中，容易让后续人员在惯常目录添加测试却未执行。

最短整改：维持当前源集合，在Android入口明确真实路径；确认该样例无外部消费者后移除，不将39份现有Java测试整体迁移，也不为保留空样例扩大sourceSets。

验收：由Gradle核对测试源集合，确认tests/unit/java的真实测试被收集；在专门临时副本验证放错目录的用例不会被冒充执行。移除样例后跑Android单测及相关入口契约回归。

### R04：根级第三方源与账本副本重复，但绑定历史证据（分批）

`original_civetweb.c` 与 `src/win/third_party/civetweb/src/civetweb.c` 均为616,093字节，原始字节SHA256完全相同，完整摘要见本地证据。但[历史审计](AUDIT_REPORT.md)第317—320行仍按根文件名记录观察；不是无消费者的垃圾。

建议先以[第三方账本](../../../src/win/third_party/civetweb/LEDGER.md)中的快照为唯一当前来源，在当前说明中登记根路径到规范路径、版本和摘要的映射。原历史正文不改写。只有确认历史定位可以经映射恢复、无外部工具固定该根路径后，才单独申请退出根副本；现阶段保留更安全。

验收：原始和规范化摘要均可复算，账本回归通过，构建/检索不依赖根副本；恢复方案保留原路径身份及历史引用解释。不得连带删除第三方许可证或把CivetWeb写成当前HTTP实现。

### R05：模型路径缺少统一的用途与消费者视图（优先补映射，代码迁移另立任务）

[Android Gradle](../../../app/build.gradle)第165—172行消费scrfd_face_detect_ncnn；[CI](../../../.github/workflows/ci.yml)第811行量化输入为scrfd_ncnn；[ModelRegistry.cpp](../../../src/cpp/src/ModelRegistry.cpp)第130行仍有yolo_face_int8_ncnn的独立历史入口。本机还并存models/int8、各族FP32/INT8和Android profile。

这证明布局和消费者多路并存，**不证明所有目录内容重复，也不证明YOLO与SCRFD可互换**。检测器注册ID、图结构、节点及解码语义不能靠改文件夹名统一。

最短整改：扩充现有[资产映射](../assets/2026-10-18-storage/asset-paths.json)和供应链说明，分别登记上游父图、转换中间件、校准集、生产消费件及消费者。Android消费入口以既有profile/manifest为准，其他注册入口列为明确兼容项。新产物先采用一致命名；历史路径只在真实消费者、摘要、预处理及templateSpaceId同步验证后分批切换。

验收：每个保留路径有明确角色及消费者，切换前后四文件摘要、预处理、节点、ABI和模板空间一致；按模型族分别运行路径/manifest/合成契约回归。真实精度缺测和结构跳过继续单列，不能用目录整理解除验收阻塞。

### R06：辅助工具、编译源和测试参数尚可进一步分域（可选，中期）

scripts共43份跟踪文件，其中23份在根部，另20份已归入android/audit/ffmpegkit。多种语言本身不是问题；需要改善的是工具入口、编译源和测试参数的发现方式。

[CMakeLists.txt](../../../CMakeLists.txt)第284行直接编译scripts/prepare_ncnn_calibration.cpp，而共享编译工具已有src/cpp/tools；[验证脚本](../../../scripts/verify_faces_test_set01.bat)第122行又从scripts读取CSV参数清单。现状能工作，属于维护建议，不是编译缺陷。

建议仅在对应工具下次维护时，将独立编译源放src/cpp/tools，已确认不含私有路径/身份数据的公用参数夹具放tests/fixtures；Android验证归scripts/android，仓库通用门禁仍可保留scripts根入口。目录归属按职责，不为统一连字符/下划线同时改所有脚本。根入口有CI、文档及用户消费者时先保留兼容入口。

验收：CMake、CI、导入路径、测试和文档消费者同步；从仓库根及其他工作目录调用均按声明行为工作；不新增依赖，不擅自调整执行、失败传播或脱敏语义。迁移所需配置改动须单独确认。

## 需要先确认用途的新增材料

本轮发现未跟踪的docs/copyright目录含一份Markdown、一份源程序TXT和两份DOCX；未读取材料正文，不判断内容正确性、隐私或申报完整性，也不将它们当垃圾。

建议由材料所属会话决定：正式可公开指南遵守DOC_STANDARD并登记索引；申报交付件可在该目录作为明确批准的二进制/生成文档例外，或放受控交付位置；中间导出稿进入对应cache/reports任务目录。文件名采用稳定的用途/版本标识，是否保留中文交付名以收件要求为准。未经内容核验和用途确认，不纳入通用提交或清理清单。本轮只提出归类边界，不替其他会话迁移或重命名。

## 忽略目录与工具固定位置

| 类别 | 当前范围 | 处置建议 |
|---|---|---|
| Git内部 | .git及其他工作树引用 | 不纳入整理，不删索引/pack或自动gc |
| 工具管理缓存 | .gradle、.gradle-local、app/.cxx、app/build、web/node_modules、coverage、__pycache__、.pytest_cache | 保留工具规定名称；淘汰按配置身份和消费者确认，不强制迁入cache |
| 历史独立构建 | build_fp15_gtest、build_android_rk3288、build_decision_core及cache/builds | 原位登记用途与工具链；只将新建树导向统一位置，旧树不直接移动 |
| 混合历史cache | audit-fixes、review-495、ffmpeg-kit-build、交接卡等 | 按来源映射保留旧名；新输入/工具/构建/证据分别分类，不因已有正式脚本就自动删原件 |
| 模型与数据 | deps/insightface、deps/WIDER_train、models下二进制及校准集 | 来源、许可、摘要、用途决定保留，不凭mtime或“未被跟踪”判废 |
| 知识图谱 | graphify-out | 固定发现入口保留；AST更新不能冒充文档语义已刷新 |
| 本机协作与历史 | .agents、.codex、.codegraph、.Jules、.remember、.superpowers、IDE目录 | 区分加载配置、工具实现、临时补丁和历史证据；不递归检查其他工作树，不批量改名 |
| 诊断和识别输出 | ErrorLog及根级识别文件 | 未读原文；确认保留/隐私和写入者后，按单独批准批次处理 |

cache/README以“可复用内容的唯一根目录”为标题，正文已说明正式工具进入scripts。建议后续将标题限定为“本地可复用产物”，避免误解为所有可复用源代码和依赖都应迁进cache；这属于说明精度调整，不推翻现有存储规则。

## 建议目标布局

以下为新内容的落点；不表示立即迁移所有现有目录：

```text
仓库根/
├─ app/                     Android模块、资源、可选AAR固定入口
├─ src/
│  ├─ cpp/{include,src,jni,tools}/
│  ├─ java/                 Android生产Java，保持既有sourceSets
│  └─ win/{include,src,app,tools,third_party}/
├─ web/                     前端源码、Cypress、前端专属脚本
├─ tests/{cpp,win,unit/java,scripts,fixtures}/
├─ scripts/{android,audit,ffmpegkit}/ + 兼容/通用入口
├─ cmake/                   目标装配及构建契约
├─ config/                  可公开的静态样例，非运行态配置
├─ deps/                    上游输入、测试桩及可选SDK接入说明
├─ models/                  消费模型及profile文本，二进制不自动入库
├─ docs/                    权威结论、说明、设计和时点报告
├─ cache/
│  ├─ builds/<用途-工具链>/
│  ├─ dependency-sources/<依赖>/<版本或提交>/
│  ├─ model-conversion/<族>/<本次转换>/
│  ├─ tools/<工具>/<版本>/
│  └─ reports/<任务-日期>/
└─ 标准工具与固定入口        .github、gradle、graphify-out、助手/Loop等
```

生产Windows运行态storage保留在既有配置解析的位置，不新增一个可能误导为仓内事实源的根级storage。Android模型stage资产仅由任务生成并保持忽略；主APK不内置模型约束不变。webroot继续作为当前Windows分发契约的一部分保留。

命名只约束新建项目自有目录：小写英文、简短职责名、必要时kebab-case；构建用途标明工具链/ABI/后端，时点报告按YYYY-MM-DD-slug。Java包/类、CMake/Gradle标准文件、ABI、模型节点/文件及第三方原包保留规定名称，不做全局替换。

## 实施顺序与质量验收

| 顺序 | 工作 | 完成标准 |
|---|---|---|
| 1 | 固化本轮状态与保留例外 | 区分本轮报告、上轮未提交整改、其他会话材料；建立消费者和恢复清单 |
| 2 | R01本地新构建入口、R03真实测试入口、R05模型映射 | 配置补丁先报批；新树独立验证，真实测试可发现，模型每条路径有身份和消费者 |
| 3 | R02后续输出规则及必要薄说明 | 合成运行不污染根目录；deps/opencv和cache职责明确；不搬原始日志或唯一证据 |
| 4 | R06按域分批迁移辅助工具 | 每批同步CMake/CI/脚本/测试/文档，保持兼容入口；按涉及平台实际验证 |
| 5 | 再评估R04及历史产物退出 | 摘要与历史映射可恢复、使用者停止、明确批准后才移动或删除；保留许可证及审计身份 |

所有批次至少检查Git差异、路径完整性、适用门禁和相关回归；文档门禁要求H0/M0。涉及C++只按Windows与Android目标验证，不把Linux参考信号提升为验收。若后续要求创建PR，另按项目规则运行全量本地CI并取得推送/PR授权。目录调整不授予提交、推送或合并权限。

本轮检查结果：初始与加入报告后的文档门禁均退出0，H0/M0/L10，未新增门禁问题；16处本地引用目标存在，Git差异检查通过，原有未提交文件摘要保持不变。未重新执行产品构建或测试。

本报告的验收仅为证据可复核、建议有消费者/风险/回滚与分批标准、报告登记索引及文档门禁/差异检查；没有实际迁移，不能宣称构建、设备、精度、隐私内容或目录整改已经通过验收。
