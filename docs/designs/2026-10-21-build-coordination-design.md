<!-- DOCSYNC_DOC
type=design
status=living
as_of=2026-10-21
-->

# 构建统筹B01/B02设计

初始B01/B02边界及设计已获用户确认：固定构建配置，补齐Web、Windows服务和发布资源的依赖与核验；保留三个CMake预设及七类测试套件。后续用户另行授权B03产物证据补充，细节见下文。B04—B07、远端CI、签名发布、设备、私有媒体和推流不在本补充范围；不提交或推送。

## 方案选择

| 方案 | 收益与代价 | 选择 |
|---|---|---|
| 固定配置入口，复用现有执行器 | 沿用Windows Job、仓库互斥、日志脱敏和失败传播；保留原生构建工具，需要增加配置身份和产物校验 | 推荐 |
| 只追加批处理脚本 | 初始代码少，但参数、配置身份和失败证据仍分散，不能满足B01 | 不采用 |
| 引入新的跨平台构建系统 | 可重新描述所有目标，但增加依赖、迁移与重复规则，超出当前问题 | 不采用 |

## 入口和边界

入口为scripts/run-builds.py，动作固定为list、plan、configure、build、verify（B05 补充 `incremental`，
见下文）；通过`--profile`选择core、windows、android-mock、android-real、android-cli、web、windows-product。windows-product是Web与Windows的产品依赖链，其余是独立构建配置。动作不接受用户提供的任意命令；操作见[构建统筹](../guides/build-coordination.md)。

输入使用显式`--inputs <本地JSON>`，schema_version=1。只接受已知工具、JDK、SDK/NDK、OpenCV/contrib、ncnn、GoogleTest源码和Web依赖目录；未定义字段、凭据字段、相互矛盾输入和链接重定向拒绝。相对路径以输入文件所在目录为基准，本机绝对路径不写入公共默认配置。工具不安装依赖、不下载模型，不修改全局环境。

职责分离：scripts/building/profiles.py描述固定目标和前置；coordinator.py组合阶段、执行和报告；artifacts.py校验配置身份与装配文件。入口只解析参数。复用scripts/testing的Step、execute_step、RepoLock及脱敏工具，不重写进程树或新增测试套件。

## 固定配置

| 配置 | 构建契约 |
|---|---|
| core | Windows宿主VS2022/x64，RK_SKIP_OPENCV=ON，核心测试目标；保留原default Ninja预设不变 |
| windows | VS2022/x64、Release、静态CRT，OpenCV/contrib源码、静态OpenCV；显式ncnn静态CRT安装；服务与现有四个原生测试可执行目标 |
| android-mock | Gradle Wrapper、JDK21、双ABI、c++_shared、显式跳过OpenCV；APK及模型禁入 |
| android-real | Gradle与双ABI真实OpenCV/ncnn装配；构建输出与APK内主库按`.text`及全部可加载节摘要绑定 |
| android-cli | CMake/Ninja、ARMv7、c++_static；独立opencv_verify_cli，不能当作APK |
| web | 已存在且源码一致的pnpm10.33.0依赖；TypeScript和Vite；显式写入本轮独占Web输出，保留正式Vite默认产物位置 |

固定项不能由本地JSON任意改写；更换ABI、STL、CRT或后端须进入不同配置身份。Windows采用现有Host CLI已有的静态OpenCV策略，首次可能需要全量重建，不能复用当前动态OpenCV树冒充同一配置。不得为减小编译量裁掉实际链接的15个模块。

## B03产物证据补充设计

用户后续确认补齐Android快照摘要与双通道验证。不同profile继续使用不同配置身份构建树，每次运行仍把APK复制到独占run-id目录；不改Gradle输出路径、Android ABI配置或Windows模型复制策略。Android快照记录profile、run-id、APK SHA-256和每个`lib/<ABI>/*.so`的SHA-256/大小，verify重新读取APK并核对完整清单，阻止旧快照或清单漂移。

真实通道的APK主库可能因AGP去掉调试节而与CMake输出文件不完全相同。核验分别记录两份`.so`的完整SHA-256，再比较ELF的`.text`和所有`SHF_ALLOC`节（含节名、类型、地址、大小、对齐、元素大小及内容）的摘要；因此运行代码/数据变更会阻断，非加载调试数据移除可通过。该摘要绑定只证明本次装配对应关系，不宣称设备加载、模型精度或真机行为通过。

## B05增量测量补充设计

用户后续确认推进 B05（先评估原生增量，再决定是否引入编译缓存）。新增 `incremental` 动作，只复用编译阶段
与该配置既有的 verify 步骤，不新增依赖、不改变既有动作的步骤序列。

- `scripts/building/incremental.py`：对象快照与差异、产物摘要、从生成工程文件（`*.vcxproj` / `build.ninja`）
  反推源文件与 include 根、只改 mtime 的扰动与还原。扰动必须用「当前时刻」而不是「原 mtime + 常量」：
  源文件通常远早于对象文件，加常数仍会旧于产物，构建系统不会重编，测量就会把「没触发」误读成「无增量」。
- `coordinator.py` 的 `_run_incremental`：同一构建树内按 repeat/source/header 顺序，每轮记录前后对象快照、
  在 `finally` 里还原 mtime，再用该配置的 verify 步骤验证产物；判据失败以 `INCREMENTAL_*` 原因码中止。
- 配置身份不同（ABI/CRT/STL/后端/工具链）即不同构建树，不参与同一组比较；本轮不为此交叉编译。
- `build`（允许增量）与 `verify`（必须重跑检查，Android 用 `--rerun`）的区分保持不变；`incremental` 只是测量。
- 退出前若状态仍为 `running` 一律判 `REPORT_STATUS_UNSET`，避免「报告运行中却返回 0」的自相矛盾成功。

## 配置、执行和证据

配置树放cache/builds/build-coordination/<配置身份>/，报告放cache/reports/build-coordination/<运行ID>/。身份包含源码根、工具链、ABI、CRT/STL、后端及有效依赖配置；ncnn的导入元数据和对应静态库也纳入摘要，库文件变化会拒绝复用旧树。普通项目源文件改动不创建一套新树，交给原生增量机制处理。

Android verify逐ABI核对实际CMakeCache中的OpenCV/contrib根、ncnn包路径及禁用libyuv选项；只检查Gradle任务成功不足以证明配置契约成立。

plan只读前置、输出计划与errors，不签署构建成功。configure在持有仓库互斥后执行配置；CMake源根、生成器和契约与已有树不匹配时拒绝，不删除或静默重配不兼容树。Android按明确ABI运行配置任务，不以Gradle任务干跑替代真实native配置。build先满足configure前提再编译；verify核对本次身份、阶段退出码和产物摘要，并运行该配置明确指定的现有检查，不能只检查文件存在。

全部实际子进程经现有Windows Job执行，失败、超时、取消后停止下游并收回自身进程；报告保留原始退出码、未运行阶段与清理失败。默认串行、预算保守，不引入新的并行调度。测试验证需要调用现有七套件时，不在已持有同名锁的子进程中重复申请；使用同一协调层的固定步骤适配。

## Windows产品链

windows-product顺序固定：前置及配置身份核验→独占Web构建→生产win_local_service构建→独占发布目录装配→摘要与资源核验。每次装配都执行，不依赖C++是否重链。

发布目录为本轮cache/reports/build-coordination/<运行ID>/product/windows/，必须原先不存在。只装配本次服务程序、Web完整文件集、项目公共级联文件和已审查样例配置；不复制整个models、用户配置、图库或私有媒体。静态OpenCV与ncnn须按契约核验，若发现未覆盖的非系统运行依赖，阻断发布验收，不签署可运行产品。

Web源输出与发布件逐文件摘要和集合一致；index引用的本地资源必须存在且留在Web根内。拒绝链接、越界、空页面、缺文件、额外旧哈希资源及拷贝期间输入变化。不删除或覆写原发布目录，失败目录保留且状态不为passed；verify不能通过重新计算摘要使被改写产物恢复为有效证据。

为防实际服务构建的现有POST_BUILD复制整个models，已按用户批准应用核心补丁：开发模型复制开关RK_COPY_DEV_MODELS默认ON，统筹配置显式关闭。默认、ON、OFF的服务/旧UI六组隔离行为检查通过；正式产品验收仍需独立构建。现有Web POST_BUILD兼容路径不直接删除；本轮产品装配不依赖它作为正确性保障。

## 验收

1. 先写失败回归：未知配置、缺输入、源根/生成器/ABI/CRT不匹配、依赖路径漂移、输出链接、已有发布目录、失败后仍装配均应拒绝。
2. 真实独立目录配置和选定目标编译；保留旧树、依赖、模型、AAR和原Web依赖，不降低CI/覆盖率门槛。
3. 用最小CMake/合成Web夹具核验仅改Web且C++不重链的实际行为。本机VS2022仍执行POST_BUILD复制，先前“资源必然不更新”的假设不成立；继续复现copy_directory不清除旧哈希资源，新产品链必须装配新页面且文件集合精确一致。
4. 两次正式产品构建分别装配独占目录，验证服务和Web集合/摘要及来源，运行现有原生、真实HTTP回环及Web相关回归；不启动生产相机服务。
5. 植入关键变异：跳过配置身份校验、忽略失败退出码、复用旧Web、允许额外旧资源，相关行为回归必须失败。报告区分配置、编译、测试、装配和真实设备验收。

实施在当前工作区的新codex分支进行，保留既有已提交测试统筹分支；自行逐任务TDD，结束时做一次独立审查。提交、推送、核心配置应用和批量删除仍遵循[项目权限](../../AGENTS.md)。操作说明继续引用[构建指南](../guides/build-and-test.md)及[存储规范](../guides/repository-storage-layout.md)，不在入口文档复制整套命令。
