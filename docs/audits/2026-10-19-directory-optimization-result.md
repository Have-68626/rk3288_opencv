<!-- DOCSYNC_DOC
type=audit
status=snapshot
as_of=2026-10-19
-->

# 目录优化与规范构建结果

## 结论与边界

已按[目录审计](2026-10-19-directory-organization-audit.md)的R01—R06及[实施计划](../plans/2026-10-19-directory-optimization.md)完成本轮目录改造。两份核心补丁和三个旧件退出均获用户确认；未提交、推送、合并或操作实机。构建来自当前含未提交改动的工作区，不是纯净HEAD的发行包。

Windows完整重建14个目标，最终CTest筛选5/5通过；Android真实双ABI与独立mock通道均完成构建、单测、lint及APK无内置模型检查；Web全量检查和现有覆盖率门槛通过。完整Python工具回归187项通过。**全量本地CI尚未全部通过**：文档与Node被其他会话基准文档阻断，详见下表；不能用前轮通过记录替代。

原始识别日志、私有媒体、模型二进制、旧构建树、根CivetWeb历史快照及其他会话版权材料保留。本轮只补模型用途映射，没有移动或重新量化模型，也没有解除许可、真实精度或实机验收限制。

## R01—R06落地

| 项目 | 本轮结果 | 直接验证 |
|---|---|---|
| R01新构建入口 | 三个preset及三个Windows包装入口的新树进入cache/builds；显式锚定源码根与binary根，旧树不搬 | 实际preset路径探针、15项入口/模块契约回归、真实Windows与Android重建 |
| R02输出边界 | Host报告进入cache/reports/opencv-host，测试集进入test-set01，模型校验默认进入face-models | 外部cwd行为回归、真实默认模型校验、真实CLI合成输出 |
| R03测试入口 | app/README明确tests/unit/java；退出不参与收集的DummyTest，保留sourceSets | Gradle实际只打印tests/unit/java；真实及mock各33份JUnit、401项测试，均零失败/零跳过 |
| R04历史身份 | CivetWeb账本登记根副本到规范快照的SHA映射，根副本和历史审计正文保留 | 原始字节摘要相等；账本/文档回归 |
| R05模型用途 | 新增[模型存储与消费者](../guides/model-storage-and-consumers.md)，区分父图、中间件、校准集与消费件 | Android两profile共8个源件身份校验通过；源表/manifest相关回归 |
| R06工具归属 | 校准CPP迁入src/cpp/tools，8组公共参数迁入tests/fixtures/opencv；补scripts、app及OpenCV桩说明 | 新CPP实际编译并通过letterbox/合成量化回归；新CSV真实grid执行8组，每组eval=1 |

批处理同时修复了路径迁移暴露的NDK块内标签解析、配置失败继续执行、SUMMARY掩盖失败和manifest空格拆参。Android preset与三个入口去掉不完整的5模块覆盖，复用项目15模块链接契约。没有全局改名、整体移动Java源码或改动CI临时目录契约。

## 已退出文件与恢复

仅删除以下三个已冻结文件，共2,849字节；未递归删除目录：

- scripts/prepare_ncnn_calibration.cpp → src/cpp/tools/ncnn_calibration_images.cpp。
- scripts/face_param_list_test_set01.csv → tests/fixtures/opencv/face-parameters.csv。
- app/src/test/java/com/example/rk3288_opencv/DummyTest.java：失活样例，不迁入测试入口。

前两项迁移件与原件逐字节相同。删除前复核源SHA、mtime、大小、ZIP CRC及归档SHA、绝对路径边界和祖先无重解析点；构建结束、新消费者验证后执行。删除后重新构建校准目标、运行真实合成grid、完整Python及CTest，均通过。现行消费者无旧路径，审计快照保留当时文件名。

本机恢复包为.superpowers/sdd/2026-10-19-directory-optimization/original-files.zip，冻结表及批准补丁同目录。需要回滚时核验当前改动归属，按原路径逐项恢复三个文件，再按逆序反向应用两份核心补丁；不要重置整个工作区或移动含绝对路径的CMake树。其他脚本原件也在恢复包中，不能覆盖后续工作。

## 本轮验收与未通过记录

| 通道 | 实际结果 |
|---|---|
| Windows新树 | VS2022 x64、真实OpenCV及静态CRT ncnn；CI目标加服务/CLI共14目标完成；最终配置使用规范位置的固定GTest源码 |
| Windows CTest | 原workflow正则选择5/5通过；GTest仍有41及7条前提跳过、1条disabled，另有1处缺模型标记；不据此声明模型精度通过 |
| Python工具 | 187/187通过，无跳过；12项精度工具行为真实执行，包含合成校准、INT8转换、检测指标及性能样本计数 |
| Android真实 | 两ABI均RK_ENABLE_NCNN=ON、RK_SKIP_OPENCV=OFF，native-lib实际定义RK_HAVE_NCNN=1；401项单测通过；lint无Error/Fatal、173条Warning未纳入本轮整改 |
| APK静态绑定 | 签名、双ABI、依赖名称闭包、AAR代码及编译件.text绑定全部通过；runtime_verified仍为false |
| Android mock | 独立build/native staging；CI同款RK_SKIP_OPENCV=true，401项单测及lint、APK无模型检查通过；不当作真实识别包 |
| Web | 锁文件安装、现有官方npm审计门禁、lint、build、bundle检查通过；Vitest 162项、Cypress 34项通过 |
| Web覆盖率 | Statements90.17%、Branches84.88%、Functions88.14%、Lines93.95%；未降低现有门槛，沿用已有审计高危例外 |
| 行为回归及变异 | 15项目录/模块回归；5组旧逻辑变异全部检出，另有4项模块契约先失败后通过；独立复核所见manifest拆参问题修复后复核通过 |
| 仓库卫生 | scan --ci退出0，5个历史/工具构建候选估计5.79GB，按保留边界未删除；退出0不表示零候选 |
| Node末轮 | 205/206，唯一失败是文档门禁自检；阻断来自其他会话新增基准文档的非法status和未索引，两项high，未跳过或放行 |
| 文档末轮 | H2/M0/L10，退出2；两项high均为docs/plans/2026-10-19-cyber-cricket-arena-benchmark-spec.md，所属会话尚未修正，本轮文档无新增high/medium |

失败记录全部保留，未改成通过：

1. 首次Android构建继承Windows的NCNN_DIR，虽编译成功却关闭ncnn，APK绑定校验失败。仅为后续Gradle进程指定正确Android SDK后重建，按两ABI实际定义及APK代码验证通过；没有修改系统环境。
2. 首轮Node作业清理回归与本会话E2E争用5173端口，205/206；E2E结束后串行复跑206/206。后续独立复跑又被新基准文档阻断，用户指定由所属会话修订，不弱化门禁。
3. 初次完整Python为183通过/4失败，CTest为1通过/4失败。原生最小探针对外部临时目录的CreateFileW返回错误码5，std文件打开errno13；同一探针在仓内可写。仅为测试进程设置仓内TEMP/TMP及pytest --basetemp后，完整Python187/187、CTest5/5通过。不修改系统ACL、生产工具或断言。

[构建与测试](../guides/build-and-test.md)已补NCNN_DIR平台边界及本机可写临时目录前提。包装脚本行为使用真实cmd与原生命令替身；真实CLI另用纯合成空白图执行8组参数，不能把这些结果当作真人精度或性能验收。完整数据库性能基准、私有数据量化、实机及Linux参考编译未执行。

## 可复用内容与落点

| 路径 | 用途与限制 |
|---|---|
| cache/builds/windows-vs2022-x64-ncnn/bin/Release | 本轮服务、测试和CLI，含OpenCV DLL及复制的webroot；复用整个必要运行目录，不只取单一exe |
| cache/builds/android-gradle-real/app/outputs/apk/debug/app-debug.apk | 本轮真实双ABI调试APK；未安装、未做相机/推流或真实精度验收 |
| cache/builds/android-gradle-mock/app/outputs/apk/debug/app-debug.apk | CI桩通道APK，仅作开发验证，勿代替真实包 |
| cache/builds/android-native-real、android-native-mock | 分离原生树；real中的1o35ad6g为最终启用ncnn树，1gh2c4h1为关闭ncnn的首次诊断树，后者保留但不算验收产物 |
| cache/dependency-sources/googletest/58d77fa8 | release-1.12.1固定提交58d77fa8070e8cec2dc1ed015d66b454c8d78850；无硬链接本地克隆、官方origin，最终Windows配置不再绑定旧构建源 |
| src/win/app/webroot | Web正式受控分发产物，保持既有契约 |
| cache/reports/directory-optimization-2026-10-19 | 本轮脱敏命令日志、JUnit汇总、产物摘要、失败/变异及恢复证据；本地证据，不作为正式文档的稳定引用 |

Android新输出位置由本轮本地Gradle init脚本形成，未改生产Gradle默认app/build及.cxx。真实与mock输出不互相覆盖。工具规定名称、ABI、模型节点与历史身份保持。可复用构建含工作区修改和构建时间，不能承诺跨次字节相同。

主要产物身份：

| 产物 | 字节数 | SHA256 |
|---|---|---|
| 真实APK | 65417140 | `d5ee5478c3e391315cb7ded2ae81dbc7cbec861e76e21da43895694d87ca6403` |
| mock APK | 35496905 | `7477aa041897f98cfc6ee4eda15510e99d4a038aaf4f27876dc4d8756f0863e2` |
| Windows服务exe | 7267840 | `3882b8d474efa79118d5d0b11f8ae7134a6361af07611adeef0c110593d842f5` |

## 后续边界

本轮不提交或合入master；需单独审阅当前累积工作区并按提交规则确认。原始日志、旧树、模型与依赖输入的进一步清理须另立冻结清单及审批，不能按卫生扫描候选或gitignore直接删除。

AST图谱更新退出0，有6处语法解析警告，部分文件仅提取部分节点；未调用LLM刷新语义或社区标签。真实编译结果独立验收，图谱更新不代表文档语义或其他会话设计已复核。Git差异检查通过，本轮范围外36项既有文件摘要一致，HEAD未变、暂存区为空。
