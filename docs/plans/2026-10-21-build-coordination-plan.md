<!-- DOCSYNC_DOC
type=plan
status=snapshot
as_of=2026-10-21
-->

# 构建统筹B01/B02实施计划

> 实施方式：由当前助手按executing-plans逐任务执行，遵循TDD；结束后一次独立审查。不自动提交、推送或删除工作目录。

**目标：** 固定各平台构建配置，补齐Web到Windows产品的配置、编译、独占装配与可信核验。

**架构：** 新增building模块，复用testing执行器、仓库互斥、脱敏与证据；保留原生构建系统和现有入口。

**技术栈：** Python3.10+、Windows Job、CMake/VS2022/Ninja、Gradle、pnpm/Node，零新增依赖。

**设计：** [已批准设计](../designs/2026-10-21-build-coordination-design.md)。

## 全局约束

- 保留三个preset和七套件；公共配置不写本机绝对路径，显式输入JSON字段白名单。
- 源码/ABI/CRT/STL/后端与工具链身份不符即拒绝，不删除或覆写已有不兼容构建树。
- Windows产品静态OpenCV、静态CRT与显式ncnn；15个模块不裁剪。
- 复用RepoLock、Step、execute_step；默认串行，失败后下游not_run，取消保留130。
- 产物独占创建、比较原摘要；模型/图库/私有媒体不进入发布，核心补丁批准后才应用。
- 用户明确要求不提交；计划中的完成记录绑定未提交源码与运行报告，不能伪造提交范围。

## 审查重点

重复JSON字段、大小写路径/链接重定向、读取和拷贝之间漂移、配置身份与实际CMakeCache不一致、ncnn静态库漂移、取消边界与报告状态由相应回归覆盖。真实Windows静态依赖另由实际编译/发布闭包核验，不能仅靠静态配置宣称闭环。

### Task 1: 固定输入与配置计划

**文件：** 新建scripts/building/profiles.py及包入口；tests/scripts/test_build_profiles.py。

**接口：** load_inputs(path)返回schema1白名单路径；make_plan(root,profile,inputs,run_dir)返回配置身份、构建树、错误与固定阶段描述。profile固定为core/windows/android-mock/android-real/android-cli/web/windows-product。

- [x] 先写未知profile、缺字段、重复JSON键、任意命令字段、相对路径、链接、工具缺失与无副作用计划回归。
- [x] 运行`python -m pytest tests/scripts/test_build_profiles.py -q`，确认真实断言或接口缺失失败，再实现最少逻辑。
- [x] 核验各平台ABI/STL、Windows静态契约与固定目标，Web输入身份一致；同配置不同源文件改动不更换树。
- [x] 同命令通过后记录任务完成，暂不提交。

### Task 2: 配置身份与产物核验

**文件：** 新建scripts/building/artifacts.py；tests/scripts/test_build_artifacts.py。

**接口：** digest(path)、inventory(directory)、check_cache(plan)、assemble_product(plan,exe,web,destination)、verify_manifest(manifest)。配置与清单只包含可复核身份，不允许重算后覆盖旧签署。

- [x] 先写源根/生成器/CRT/ABI错误、依赖配置漂移、链接/越界、缺index、引用缺失、已有输出、额外旧资源、拷贝漂移回归。
- [x] 运行`python -m pytest tests/scripts/test_build_artifacts.py -q`，观察RED，再实现GREEN。
- [x] 通过最小CMake夹具核验不重链时POST_BUILD的真实行为；本机VS2022仍复制新页面，但旧哈希文件残留，独占装配须精确更新集合且旧目录不动。
- [x] 同命令通过后记录任务完成，暂不提交。

### Task 3: 受控构建协调与CLI

**文件：** 新建scripts/building/coordinator.py、scripts/run-builds.py；tests/scripts/test_build_coordinator.py。

**接口：** run(action,plan)执行固定阶段并原子保存summary；verify读取原报告，先核对产物和源身份，再执行相应现有检查，不能重签被改写文件。CLI固定list/plan/configure/build/verify及--inputs、--profile、--report。

- [x] 先写plan不写盘/不执行、失败后停止、错误报告、互斥、超时、取消及不能复用旧产物回归。
- [x] 运行`python -m pytest tests/scripts/test_build_coordinator.py -q`，RED后复用现有执行器实现。
- [x] 单独编译与验证分开签署；实际命令、工具版本、输入身份、源码绑定及产物摘要写入独占脱敏报告。
- [x] 同命令通过后记录任务完成，暂不提交。

### Task 4: 核心复制开关候选与批准

**文件：** 候选补丁留cache/reports/build-coordination-implementation-2026-10-21；拟修改cmake/windows.cmake；tests/scripts/test_build_cmake_contract.py。

- [x] 先用合成models标记文件证明现有POST_BUILD复制路径，准备默认保留旧行为的开发复制开关候选。
- [x] 隔离配置验证开关关闭不复制模型、默认值维持兼容，运行`python -m pytest tests/scripts/test_build_cmake_contract.py -q`。
- [x] 展示确切补丁、风险和反向回滚，获得用户批准后应用核心配置；批准前完成所有独立模块工作。
- [x] 正式配置复验并记录结果，不提交。

### Task 5: 真实构建、变异与文档

**文件：** 同步docs/guides/build-and-test.md、scripts/README.md及本计划；正式行为变异归属tests/scripts/。

- [x] 独立配置并真实构建已登记通道，保留命令、失败、心跳及原始退出码；Windows静态树首次重建如实报告成本。
- [x] 两次真实产品构建，Web输出与独占发布集合/摘要一致；核验非系统动态依赖，不启动相机服务。
- [x] 运行原生、HTTP回环、Web和全部相关工具回归，四组关键变异必须检出；不跨套件累加用例。
- [x] 文档H0/M0、diff检查及敏感扫描通过，更新graphify AST；一次独立审查，重要缺陷按TDD闭环。
- [x] 报告实际完成、缺测、核心配置批准状态及未提交状态；不删除历史或本轮证据。


## 验收结果（2026-10-21）

- 所有构建配置均完成 build 与 verify，源码摘要在每个运行内保持一致：Windows 产品 `20261021T123427-bca6c6` / `20261021T123524-f730b6`，Android real `20261021T122902-9ef98b` / `20261021T122936-fc4df8`，Android mock `20261021T123106-8130fe` / `20261021T123148-881fb5`，Android CLI `20261021T123310-5925ae` / `20261021T123320-949eb9`，core `20261021T123332-24a2b4` / `20261021T123337-860ba5`，Web `20261021T123354-cdf840` / `20261021T123404-63ab73`。报告在 `cache/reports/build-coordination/`。
- Windows 产品链完成两次独立构建和装配；最终产品资源清单、PE运行依赖、4项HTTP契约、163项Web测试通过。原生套件报告645次执行、597通过、48跳过、0失败；跳过项不视为精度通过。
- 最终套件报告 `cache/reports/test-suites/20261021T123830-2090a4/summary.json`：文档80、Node 214、Python 368、Cypress 34项通过，0失败、0跳过；文档审计H0/M0/L10。构建统筹专属回归65项通过，四组行为变异均检出。
- 独立审查的两个重要问题已按TDD修复：Android ncnn各ABI静态库和导入元数据进入输入指纹；Android真实CMakeCache逐ABI核对OpenCV/contrib/ncnn路径及 `RK_ENABLE_LIBYUV=OFF`。新增回归先复现失败，再通过。
- `graphify update .`成功更新AST；工具报告语法解析警告与社区重命名提示，未运行联网语义标签刷新。
- 未执行真机、摄像头、模型真实精度或网络推流验收。Android MPP/Qualcomm SDK缺失时回退CPU；不宣称硬件加速或真实识别精度通过。
- 工作区改动未提交、未推送；原始日志、模型、依赖与历史证据均保留。

## B03后续补充进度（2026-10-21）

- [x] 为Android快照补入profile、run-id和APK内所有原生库的SHA-256/大小清单；verify校验ABI、模型禁入、清单摘要和真实通道构建绑定。
- [x] 增加ELF可加载节摘要；真实通道同时记录构建文件与APK内文件的SHA-256，区分AGP剥离调试节与运行内容漂移。
- [x] RED/GREEN回归覆盖非`.text`运行数据漂移、合法非加载调试尾节、摘要篡改及真/桩快照并存；新增`android-loadable`行为变异。
- [x] 完成当前源码绑定的Android real/mock build与verify、全量脚本回归、文档门禁和差异检查；各次运行ID与详细结果保留在 `cache/reports/build-coordination/`，最终交付时列明。
