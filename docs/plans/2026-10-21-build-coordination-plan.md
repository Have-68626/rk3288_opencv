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

## B05 增量行为评估（2026-10-22）

- [x] 新增 `scripts/building/incremental.py` 与 `incremental` 动作：同一构建树内 repeat/source/header 三对受控比较，只扰动 mtime 并在 `finally` 还原；重建后用该配置既有 verify 步骤证明产物正确。
- [x] 拒绝不支持的配置（Gradle 构建树）、非 passed 报告与缺失构建树；退出前残留 `running` 判 `REPORT_STATUS_UNSET`。
- [x] 回归 `tests/scripts/test_build_incremental.py`（7 条，含真实 VS2022 增量测量）；新增 `incremental-diff` 行为变异，全部 9 条变异可检出。
- [x] core 实测（身份 `f296bb65f3f4`）：repeat **0**、source **1**（`EventManager.cpp`）、header **4**（`FaceSearch.h`），三组 verify 均 passed；`artifact_changed` 在 source/header 为真。
- [x] 结论：原生增量可解释且未失效，**不引入编译缓存**；配置身份不同不参与同组比较，未做 ABI/CRT 交叉编译。

## B07 本地/CI 目标映射（2026-10-22）

- [x] 新增 `docs/guides/ci-local-mapping.md`：七个 CI job（`changes`/`repo-hygiene`/`unit-tests`/`docs-audit`/`web`/`android`/`windows`）与七个本地套件、七个构建配置的对应表，逐条登记门禁边界与**未运行/非阻断/缺资产/跳过**的判读规则。
- [x] 覆盖回归 `tests/scripts/test_ci_local_mapping.py`：解析 `ci.yml`+`docs-audit.yml` 的 job 列表与 `SUITES`、`PROFILES`，缺登记即失败。
- [x] 明确未覆盖项：CI 只跑 `android-mock`（真实后端仅本地 `android-real`）；本地 `core` 是 `RK_SKIP_OPENCV` 快通道，不等价于 `windows` job；`unit-tests` 为 Linux 非门禁参考。
- [x] 不修改任何 CI 配置；CI 变更仍需另行报批。

## B06 构建时间与可复现（2026-10-22）

- [x] 统筹侧已落地：`--build-time` 显式输入、`validate_build_time` 校验、`BUILD_TIME_INVALID` / `BUILD_TIME_ANDROID_APP_ONLY` / `BUILD_TIME_MISMATCH` / `PRIOR_BUILD_TIME_UNAVAILABLE` 四个原因码；verify 复用原报告时间；NDK/CMake 有效版本由 `record_android_toolchain` 逐 ABI 登记。
- [x] 回归：`test_android_plan_accepts_and_reports_a_fixed_build_time`、`test_android_build_time_is_validated_without_forking_the_build_tree`（不同时间共用同一构建树）、`test_android_verify_reuses_the_original_build_time`、新增 `test_android_verify_never_reuses_stale_test_results`（verify 必须 `--rerun`，不得跳过测试）。
- [x] **已按批准应用 `app/build.gradle` 补丁**：`BuildConfig.BUILD_TIME` 优先取 `-Prk.buildTime`，未给出才回退当前分钟；
  形状不符抛 `GradleException`。
- [x] 复现性实测（android-mock）：`20261022-2000` 连续两次构建 **APK 摘要相同**（`037f1cad…`）；`20261022-2001`
  **摘要不同**（`8b873640…`）；APK 内 `classes*.dex` 分别含 `20261022-2000` / `20261022-2001`，三次 `status=passed`。
- [x] 新增静态 tripwire `tests/scripts/test_build_time_contract.py`：锁住「Gradle 读 `rk.buildTime`」与
  「`buildStamp` 进 `BuildConfig.BUILD_TIME`」这条接线，防止再次退化成「只把参数写在命令行里」。
- [x] 新增 `test_android_verify_never_reuses_stale_test_results`：verify 必须 `--rerun`，不得跳过测试。
- 边界：Gradle 侧形状校验未单独用 `gradlew -Prk.buildTime=<非法>` 实测（统筹侧会先拒绝），属**防御性**第二道闸。


- [x] 为Android快照补入profile、run-id和APK内所有原生库的SHA-256/大小清单；verify校验ABI、模型禁入、清单摘要和真实通道构建绑定。
- [x] 增加ELF可加载节摘要；真实通道同时记录构建文件与APK内文件的SHA-256，区分AGP剥离调试节与运行内容漂移。
- [x] RED/GREEN回归覆盖非`.text`运行数据漂移、合法非加载调试尾节、摘要篡改及真/桩快照并存；新增`android-loadable`行为变异。
- [x] 完成当前源码绑定的Android real/mock build与verify、全量脚本回归、文档门禁和差异检查；各次运行ID与详细结果保留在 `cache/reports/build-coordination/`，最终交付时列明。
