<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-21
-->

# 本地测试套件统筹

统一入口为 `scripts/run-test-suites.py`，在Windows宿主使用Python 3.10+。本工具复用现有命令，不代替远端CI，不自动访问设备、私有媒体或推流；完整范围见[设计](../specs/2026-10-20-test-suite-coordination-design.md)。

## 先查看计划

从仓库根执行；也可用脚本绝对路径从其他目录调用。默认源码/报告位置相对脚本所在仓库；显式传入的相对路径相对调用终端。

```powershell
python scripts/run-test-suites.py list
python scripts/run-test-suites.py plan --suite docs --suite tooling
python scripts/run-test-suites.py run --suite docs --suite tooling
```

plan只检查前置并展示命令，不启动测试或创建运行报告；其中errors非空表示相关套件不能执行，不能把计划退出0当测试通过。run只执行明确选择的套件，按固定顺序去重，首个失败即停止，后续列为not_run。

| 套件 | 前置与结果范围 |
|---|---|
| docs | Node；审计器回归及H0/M0门禁，外部链接探测未运行 |
| tooling | Node、Python、pytest；卫生、JNI、action固定、全部Node/Python辅助回归 |
| windows | 显式已配置的CMake目录，且源码根匹配；构建四个测试可执行目标，运行含独立重入用例的五个CTest目标 |
| android-mock | 明确JDK及SDK；桩APK、模型禁入、Java单测、lint，不证明真实后端 |
| android-real | 明确OpenCV/contrib及双ABI ncnn；真实APK、单测、lint，并绑定APK的.text到启用ncnn的构建记录；不证明设备或精度 |
| web | pnpm10.33.0和匹配的已有依赖；lint、单测、build及现有包体门禁 |
| web-e2e | 同一Web源码、Cypress二进制与空闲5173；带插桩Vite、Cypress和原覆盖率门槛，API由用例mock |

工具不安装依赖或下载模型。若node_modules由其他pnpm版本产生，选择已有且源码一致的独立Web目录，或由操作者另行准备；不擅自移除原依赖。普通跳过如实报告，缺精度工具不等于真人精度通过。

## 明确输入

Windows例子使用已配置构建树，不自动重新配置或猜测OpenCV/CRT/ncnn：

```powershell
python scripts/run-test-suites.py plan --suite windows `
  --windows-build-dir cache/builds/windows-vs2022-x64-ncnn
```

Android可显式传入以下参数；具体安装位置由操作者替换，示例不是本仓默认路径：

```powershell
python scripts/run-test-suites.py plan --suite android-mock `
  --java-home 'C:/toolchains/jdk21' --android-sdk 'C:/toolchains/android-sdk'
```

真实通道额外指定 `--opencv-root`、`--opencv-contrib-root`、`--android-ncnn-dir`。现有环境变量可作为这些前提的来源；仅调整子进程环境，不改系统环境。真实通道核对两ABI配置、编译定义和APK代码绑定，找不到绑定时标为unverified。

合成精度回归可另传 `--precision-tools-dir`、`--ncnn2table`、`--ncnn2int8`、`--public-cascade`；它们指向本机工具或公共级联文件，不是人脸媒体。Web可指定 `--web-dir`，工具入口可指定 `--node`、`--pnpm`、`--cmake`、`--ctest`、`--pwsh`。E2E不会启动真实后端。

每个步骤有用途对应超时，可用 `--timeout` 覆盖为正的有限秒数；0、负数和NaN/Infinity会被拒绝。心跳每30秒显示当前步骤/PID/时长，只代表存活，不代表用例已经开始。

子进程使用本轮 `cache/reports/.tmp/<短标识>` 作为TEMP/TMP，保留现场，不修改父进程或系统环境。显式Node入口也加入子进程PATH，供pnpm和E2E驱动调用。

## 资源与取消

同一仓库使用Windows全局命名互斥，第二个统筹进程快速失败。所有套件串行，避免Gradle、CMake、Web覆盖率及报告竞争。该互斥不能阻止用户手工运行其他工具；执行前仍须确认没有手工任务并发写同一构建树。

E2E会再次检查5173端口，已有服务不属于本次所有权，不会被停止。实际子进程先进入本次Windows Job再放行；成功、失败、超时及Ctrl+C均收回自身后代，不按进程名称批量清理。清理或日志捕获失败导致非零结果。

## 报告判读

结果放在 `cache/reports/test-suites/<唯一标识>/summary.json` 与summary.md，各步骤保留脱敏log.txt和结构化证据。旧报告不覆盖；固定输出的Gradle报告必须由本次重跑产生，内容不变或无法证明新鲜则拒绝签署。仅touch旧文件不会使其被接受。

每步将校验后的JUnit独占归档到自身evidence目录；XML按节点脱敏，保留原路径、原摘要及归档摘要。后续Gradle覆盖共享报告不影响已归档证据；校验与归档之间内容变化也会被拒绝。

每次run独占保存plan.json，包含实际命令、cwd、超时、报告位置/单位和明确输入身份；summary记录其摘要。只保留选定配置文件及公共工具的摘要，不遍历整个SDK、模型或媒体目录；目录身份明确限定为这些配置文件。PATH等环境值不落盘，只保留环境键名，Java/SDK等有效路径由白名单输入记录。plan的终端输出仍是套件数组，不创建运行报告。

Python版本直接记录当前解释器；Node/pnpm/CMake/CTest/PowerShell/Java按所选套件通过受控Job采集版本，原始退出码与脱敏日志保留在versions目录。Gradle使用wrapper配置身份，不为取版本额外下载发行包。版本缺失或无效阻塞执行；探测禁用Corepack联网。取消仍返回130，边界异常将未完成步骤收束为cancelled或blocked，不遗留running；已完成结果保留。取消期间同时发现源码变化会记录source_changed，不覆盖取消退出码。

JSON包含schema版本、运行ID、源码HEAD/工作区摘要、选择/未选范围、步骤原始退出码、时长、状态、计数单位、证据路径及摘要。运行前后源码绑定变化会使整体非零；受跟踪webroot是构建输出，与源码绑定分开处理。汇总无全局唯一用例总数：不同套件可能重复覆盖相同用例，不跨套件相加。

工作区摘要覆盖受跟踪差异，以及scripts/tests/src/app/web/docs/cmake/config中约定扩展名的未跟踪源码文本；排除依赖、构建、报告、private目录、媒体二进制和链接目标，不作为全磁盘完整性签名。

CI对应关系：docs对应文档审计；tooling对应卫生和Windows工具回归；windows对应选定原生目标；android-mock对应Android桩流程，android-real为额外本地通道；web对应Web检查的部分步骤，未包含安装和在线漏洞审计；web-e2e在远端仍非阻断，在本工具中失败会阻断。完整CI前提仍见[构建指南](build-and-test.md)。

- passed：该步骤满足自己的退出码与证据判据，不表示全部平台或真实模型通过。
- failed：进程或原有门禁失败，后续不运行。
- blocked：缺前置、互斥、启动、捕获或清理问题。
- unverified：报告缺失/过时/无效，或实际Android构建绑定不足。
- timed_out / cancelled / not_run：分别为超时、取消及未执行。

JUnit读取真实用例节点并核对声明计数，拒绝缺失、畸形、零用例及矛盾报告。Windows适配器逐目标采集CTest日志、GoogleTest XML和bool汇总，分别保留目标数、case_execution及unique_cases；混合框架分开读取，不把默认禁用用例算作执行。独立重入目标按实际可执行文件和用例名辨别身份。CTest日志上限为16MiB，截断或缺汇总导致未验证，不猜计数。缺资产目标、框架跳过和未运行禁用条目分别保留；TEST_UNVERIFIED和TEST_SKIP_ABSENT不构成精度通过。

整体退出码：0满足所选执行判据；1测试/门禁失败；2前置、资源、源码或证据问题；124超时；130取消。原始步骤码单独保留。异常或无法写入汇总时返回非零，不伪造报告。selected_execution_complete及has_skips_or_unverified须连同逐步状态阅读，不是全覆盖签名。

## 与原入口兼容

原 `scripts/run-web-e2e.ps1` 调用保留，可选WebDirectory、PnpmPath、SkipInstall和ReportDirectory用于统筹。默认安装使用冻结锁文件；统筹指定SkipInstall，只消费已准备的依赖。Cypress启动器透传本次CLI报告参数，仍保留旧覆盖率归档、环境隔离和退出码。

[现有构建指南](build-and-test.md)的手工命令继续有效；本工具不改CI的非阻断策略或覆盖率阈值。本地所选套件通过不等于远端CI、真实服务、设备加载或生物识别精度验收。

## 验证统筹器自身

### 扩展验证入口

真实前后端契约是Windows套件的显式选项，不改变七类套件，也不改变已有mock E2E：

```powershell
python scripts/run-test-suites.py run --suite windows `
  --windows-build-dir cache/builds/windows-vs2022-x64-ncnn `
  --contract-web-dir cache/builds/web-pnpm-10.33.0/web
```

该目录仅提供已安装的Vite，加载的API模块来自当前仓库。测试宿主复用真实HttpFacesServer和配置存储，以独占临时配置、动态回环端口、空pipeline运行；不启动生产服务、不读取用户配置、不访问相机。检查health诊断、settings JSON Schema、OpenAPI操作、前端ApiError及拒绝更新的原子性。独立入口为scripts/check-web-contract.py；原生独立入口scripts/run-native-tests.py本身不提供Windows Job，建议通过统筹器调用。

scripts/record-ci-result.py的start/finish用于CI运行前后快照，保存真实退出码、JUnit或LCOV身份及Step Summary；缺失、空白或未改变证据不算通过。workflow保留E2E非阻断策略，页面状态与最终job阻断性质分别解释。

增量工程检查使用`python scripts/check-raii-violations.py --changed --base HEAD`及`git diff --check`。不带--changed时扫描跟踪C++文件；旧sh入口转发同一Python实现。词法规则只覆盖ANativeWindow与AndroidBitmap锁族及指定封装类体，不展开宏、追踪别名或证明open/close配对；全仓扫描通过不等于全部RAII契约完成。规则尚未作为新CI静态门禁。

执行器在启动前对本次短TEMP进行UTF-8写读检查，只删除自身探针文件。环境和协调边界异常保留类型、errno、winerror及阶段，不写异常正文或环境全集。卫生目录缓存分别限于一次只读盘点或scan规则展开，不缓存文件stat；clean/restore及后续快照重新读盘，不共享过期缓存。

### 离线验证输入

`python scripts/check-validation-inputs.py --manifest <显式清单.json>`只校验声明的输入身份。合成清单scope为synthetic；本地模型清单scope为local-assets，须额外传--allow-local-assets。两者都不读取图像或视频，不调用设备。

清单schema_version=1，files至少含model_param和model_bin的path/sha256；可另列parent_model（onnx）和apk。source、license_reference、parent_sha256必须明确；preprocessing包含channel_order、三通道mean/norm、input_shape、input_node、output_node、embedding_dim；measurement为samples、warmup、steady_samples的计划值。相对文件路径以清单目录为准，链接与媒体后缀拒绝。来源/许可仅记录声明，不独立确认法律许可或转换父链。

输出在cache/reports/validation-inputs，缺项退出2/blocked；身份检查通过仍退出77，accuracy=unverified、device=not_run、performance=not_run。输入摘要通过、测量计划存在均不能替代真实精度或性能数据；真人、真机、显示方式与推流另行确认。

### 六组固定变异

```powershell
python scripts/check-test-suite-mutations.py --list
python scripts/check-test-suite-mutations.py
python scripts/check-test-suite-mutations.py --mutation leak-descendants
```

固定六组：吞退出码、接受旧报告、接受空报告、失败后继续、绕互斥、漏清后代。先在未修改隔离副本运行相同测试，基线不通过则不执行变异。每组仅修改独立副本，外层使用未变异的执行器收回后代，包括变异故意造成的泄漏。

结果位于cache/reports/test-suite-mutations/唯一标识/results.json。退出1且本轮JUnit包含断言失败、无收集/导入错误才算检出；原源码摘要须保持不变。存活、超时、基线失败或无效证据均不构成检出。可用--timeout调整每组正的有限秒数；工具退出0表示全部所选变异检出，非零须读取具体原因和原始退出码，不能以pytest失败本身当作成功。该工具独立运行，不增加第八套件，不自动接入CI。
