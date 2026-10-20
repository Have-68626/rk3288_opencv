<!-- DOCSYNC_DOC
type=plan
status=living
as_of=2026-10-19
-->

# 目录优化与规范内容重建计划

目标：按[目录审计](../audits/archive/2026-10-19-directory-organization-audit.md)实施R01—R06，并以Windows、真实双ABI Android APK、Web全量重建和本地CI验收。不新增运行依赖。

设计：沿用当前分支和用户工作区，用独立新构建树与补丁探针保护旧产物及未提交工作。规范目录承载后续可复用工具/夹具/报告；源模型、生产身份和历史证据不迁移。台账在本地.superpowers/sdd/2026-10-19-directory-optimization，脱敏日志在cache/reports/directory-optimization-2026-10-19。技能默认计划目录、自动提交/清理及逐任务分派由用户当前约定替代。

全局约束：保留原始日志、模型二进制、旧树、根第三方快照和版权材料；核心配置和批量退出先报批；不提交、推送、合并或操作实机。共享接口为构建目录→报告工具、校准源→CMake目标、sourceSets→真实Java测试；模型映射不得改变解码器或templateSpaceId。

## 任务1：构建入口和输出边界（R01/R02）

- [x] 记录基线及既有文件摘要，备份受影响配置/脚本原件ZIP。
- [x] 先运行tests/scripts/test_build_layout_behavior.py；排除测试驱动引号/行尾因素后，旧脚本7项失败。
- [x] 三个批处理入口显式锚定仓库根与CMake -S/-B；Host构建进入cache/builds/windows-opencv-vs2022-x64，Android CLI进入android-opencv-ninja-armeabi-v7a。
- [x] 后续工具报告进入cache/reports对应领域，保留显式参数；修复NDK块内标签、配置失败继续构建和SUMMARY掩盖失败，9项行为回归通过。
- [x] 已应用获批的core-layout.patch及opencv-module-followup.patch：preset输出、校准源、CRLF规则及完整模块契约；15项回归和实际新树验证通过。
- [x] 五组行为变异均检出，包括恢复旧路径/退出码语义及manifest空格拆参。

验证：`python -m pytest tests/scripts/test_build_layout_behavior.py -q`。实际15项通过；替身只取代命令边界，不连接设备或读取私有媒体。

## 任务2：测试入口与工具归属（R03/R06）

- [x] 原校准CPP和公用CSV逐字节复制到src/cpp/tools/ncnn_calibration_images.cpp与tests/fixtures/opencv/face-parameters.csv；旧件待批。
- [x] 核对CMake与所有现行脚本消费者，更新指南和最小发现入口；保持src/java与tests/unit/java的sourceSets不变。
- [x] 当前构建完成并切换新源后，按批准的冻结清单退出旧CPP、旧CSV和失活DummyTest，不递归删除目录。
- [x] Gradle打印真实测试源集合并运行单测，Windows实际重建校准工具并执行合成letterbox回归。

回滚：从original-files.zip核验恢复旧件，不覆盖新工作；反向应用对应核心补丁。目录切换与失败传播分别验证，不能用文件存在代替执行结果。

## 任务3：模型映射与历史身份（R04/R05）

- [x] 建立现行模型用途/消费者指南，引用既有profile、供应链和历史资产映射；区分父图、中间件、消费件及校准集。
- [x] 用既有校验器核对Android消费件SHA；不运行私有校准集、量化或真实精度验收。
- [x] CivetWeb账本登记根副本到规范副本的SHA与历史引用，根快照保留。
- [x] deps/opencv补桩说明，cache标题限定本地产物，长期指南与索引同步。

模型路径整理不改变模型ID、节点、预处理、许可或模板空间；历史移动记录不按当前布局重写。

## 任务4：全量重建和本地CI

- [x] Windows新树cache/builds/windows-vs2022-x64-ncnn：VS2022/x64、真实OpenCV与静态CRT ncnn；构建CI全部门禁目标、服务及CLI，运行workflow的CTest选择及Python工具回归。
- [x] Android用现有Java21，在独立Gradle build/native staging目录运行assembleDebug、testDebugUnitTest、lintDebug、checkApkFaceModels；另核验CI的mock通道，不把mock当真实识别。
- [x] Web锁文件安装、官方npm审计（沿用CI高危豁免）、lint、Vitest、build、bundle检查、Cypress E2E及现有覆盖率门槛。
- [x] 仓库卫生扫描退出0、Python工具全套187项通过；CTest在仓内可写临时目录5/5通过，原失败证据保留。
- [ ] 全量Node和文档门禁仍受其他会话基准文档阻断：Node205/206，文档H2/M0/L10；用户指定由所属会话修订，不接管该文件。

Windows构建目标和CTest选择按ci.yml第708/757行执行，另构建服务/验证CLI；不为了目录统一改workflow。Windows从空树编译OpenCV；ncnn固定安装和GTest已知源码复用。Android初始化脚本仅改变本轮验收输出，不清空app原build/.cxx；不替换FFmpegKit消费AAR、模型或设备应用。每条命令记录真实退出码、脱敏日志和30秒心跳。

Linux参考编译不作为开发目标。真实INT8精度/私有媒体不在授权范围；相关非门禁数据步骤记录未执行或UNVERIFIED，不能声称所有云端工作流逐字等价运行。

## 任务5：复核和交付

- [x] 核对R01—R06落地、失败/跳过及风险，修复本轮回归。
- [x] 记录新构建、产物摘要、重用命令和恢复方式，形成正式结果并登记索引。
- [x] 检查旧改动和版权材料保护，无意外二进制暂存，无提交推送。

涉及配置或退出仍待确认时，完成独立工作并保留该项未完成，不把部分成功写成整轮验收完成。

## 本轮结果与外部待办

详见[实施结果](../audits/2026-10-19-directory-optimization-result.md)。Windows14目标、Android真实及mock各401项、Web162项及34项E2E通过；实机和真实精度未验收。三旧件按批准退出，模型/旧树/历史材料保留，未提交、推送或合并。

全量本地CI尚不能宣称全部通过：docs/plans/2026-10-19-cyber-cricket-arena-benchmark-spec.md仍有非法status及未索引两项high，需所属会话修正后重跑Node与文档门禁。该项不降低门槛，也不以此前206项通过替代当前状态。
