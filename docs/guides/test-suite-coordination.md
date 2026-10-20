<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-20
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

JSON包含schema版本、运行ID、源码HEAD/工作区摘要、选择/未选范围、步骤原始退出码、时长、状态、计数单位、证据路径及摘要。运行前后源码绑定变化会使整体非零；受跟踪webroot是构建输出，与源码绑定分开处理。汇总无全局唯一用例总数：不同套件可能重复覆盖相同用例，不跨套件相加。

工作区摘要覆盖受跟踪差异，以及scripts/tests/src/app/web/docs/cmake/config中约定扩展名的未跟踪源码文本；排除依赖、构建、报告、private目录、媒体二进制和链接目标，不作为全磁盘完整性签名。

CI对应关系：docs对应文档审计；tooling对应卫生和Windows工具回归；windows对应选定原生目标；android-mock对应Android桩流程，android-real为额外本地通道；web对应Web检查的部分步骤，未包含安装和在线漏洞审计；web-e2e在远端仍非阻断，在本工具中失败会阻断。完整CI前提仍见[构建指南](build-and-test.md)。

- passed：该步骤满足自己的退出码与证据判据，不表示全部平台或真实模型通过。
- failed：进程或原有门禁失败，后续不运行。
- blocked：缺前置、互斥、启动、捕获或清理问题。
- unverified：报告缺失/过时/无效，或实际Android构建绑定不足。
- timed_out / cancelled / not_run：分别为超时、取消及未执行。

JUnit读取真实用例节点并核对声明计数，拒绝缺失、畸形、零用例及矛盾报告。CTest计数单位为target，内部用例数明确未知；其system-out中的缺测/跳过标记也保留。框架允许的skip不会自动变成失败，但报告保留数量、原因所在证据和覆盖限制。TEST_UNVERIFIED和TEST_SKIP_ABSENT不构成精度通过。

整体退出码：0满足所选执行判据；1测试/门禁失败；2前置、资源、源码或证据问题；124超时；130取消。原始步骤码单独保留。异常或无法写入汇总时返回非零，不伪造报告。selected_execution_complete及has_skips_or_unverified须连同逐步状态阅读，不是全覆盖签名。

## 与原入口兼容

原 `scripts/run-web-e2e.ps1` 调用保留，可选WebDirectory、PnpmPath、SkipInstall和ReportDirectory用于统筹。默认安装使用冻结锁文件；统筹指定SkipInstall，只消费已准备的依赖。Cypress启动器透传本次CLI报告参数，仍保留旧覆盖率归档、环境隔离和退出码。

[现有构建指南](build-and-test.md)的手工命令继续有效；本工具不改CI的非阻断策略或覆盖率阈值。本地所选套件通过不等于远端CI、真实服务、设备加载或生物识别精度验收。
