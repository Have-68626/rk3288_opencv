<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-21
-->

# 本地构建统筹

`scripts/run-builds.py`统一固定构建配置、前置、串行执行和产物核验；原有三个CMake preset与七类测试套件保留。首次配置可能全量编译，不复用其他工具链的旧树。当前实现的正式验收进度见[实施计划](../plans/2026-10-21-build-coordination-plan.md)，不能把入口存在当作各通道均已验收。

## 输入与前置

从仓库根运行，使用Python3.10+和Windows宿主。先准备已安装依赖，不自动安装、下载或重建原node_modules。本机输入JSON留在忽略的`cache/local-inputs/`；相对路径基于该JSON所在目录，不能加入命令、环境变量、凭据或自定义CMake选项。

以下为Web配置示意；将路径替换为实际已存在位置再运行：

```json
{
  "schema_version": 1,
  "paths": {
    "node": "C:/tools/node/node.exe",
    "pnpm": "C:/tools/node/pnpm.cmd",
    "web_dir": "../../builds/web-pnpm-10.33.0/web"
  }
}
```

| 配置 | 必填paths键 | 固定契约与边界 |
|---|---|---|
| core | cmake、ctest、googletest_source | VS2022/x64、静态CRT、跳过OpenCV，仅core_unit_tests |
| windows | 上行全部、opencv_root、opencv_contrib_root、windows_ncnn_dir | 静态OpenCV、静态CRT ncnn、服务及四个原生测试目标 |
| android-mock | java_home、android_sdk、android_ndk | JDK21、离线Gradle9.0、双ABI/shared STL、桩APK |
| android-real | 上行全部、opencv_root、opencv_contrib_root、android_ncnn_dir | 双ABI真实OpenCV/ncnn，APK及.text绑定 |
| android-cli | cmake、ninja、android_ndk、opencv_root、opencv_contrib_root | ARMv7/static STL、独立opencv_verify_cli，不能当作APK |
| web | node、pnpm、web_dir | pnpm10.33.0，现有依赖与活树Web源码一致 |
| windows-product | windows与web所需键的并集 | Web→服务→独占装配；不启动生产服务 |

工具键指向可执行文件，其余指向源码或安装根；ncnn的Windows与Android安装不能混用。Android SDK须包含android-34和CMake3.22.1；NDK显式指定，不修改系统环境。Gradle发行包须已在当前GRADLE_USER_HOME或用户默认缓存解包并完成，缺失则前置阻断，`--offline`不会替Wrapper下载发行包。

未知/重复字段、路径链接和不兼容构建树拒绝。pnpm托管的TypeScript/Vite链接仅允许落在当前Web的实体`node_modules/.pnpm`内。Android ncnn导入元数据及各ABI静态库纳入输入摘要；替换库会拒绝旧身份。Android verify逐ABI核对真实CMakeCache中的OpenCV/contrib路径、ncnn路径和`RK_ENABLE_LIBYUV=OFF`。依赖身份只覆盖已登记的二进制及元数据，不是整个SDK或OpenCV源码的供应链签名。

## 执行与核验

```powershell
python scripts/run-builds.py list
python scripts/run-builds.py plan --profile web --inputs cache/local-inputs/builds.json
python scripts/run-builds.py configure --profile web --inputs cache/local-inputs/builds.json
python scripts/run-builds.py build --profile web --inputs cache/local-inputs/builds.json
```

`plan`只读、列出实际命令与错误，不创建成功报告。`configure`执行该配置的真实配置任务；Web没有原生配置步骤，不能据此宣称Web编译通过。`build`先配置，再编译与归档；每次输出`REPORT=.../summary.md`，同目录JSON保存机器证据。只有build报告为passed，才能用其JSON执行独立verify：

```powershell
python scripts/run-builds.py verify --profile web --inputs cache/local-inputs/builds.json --report cache/reports/build-coordination/运行ID/summary.json
```

所有动作可用`--cpu-budget N`设置正整数CPU槽位，默认2；CMake和Gradle会使用同一值。`--memory-budget-mb N`可选，设置后按Windows Job进程树峰值监控并在超限时终止本步骤；省略时只采集峰值、不设上限。统筹过程保持串行，当前不开放阶段并行。报告同时记录预算、峰值活动进程数和峰值Job内存；Job不可用或未采样时明确显示“不可用”。例如：

```powershell
python scripts/run-builds.py build --profile core --inputs cache/local-inputs/builds.json `
  --cpu-budget 4 --memory-budget-mb 8192
```

退出码：0表示该动作通过，1表示步骤失败/超时，2表示前置、身份或证据不成立，130表示取消。失败后停止下游，保留未执行状态、原始退出码和失败目录。与测试统筹共用仓库互斥；不要同时运行其他不遵守此锁的构建。只清理自己创建的进程，不清理既有构建树。

构建树在`cache/builds/build-coordination/<配置>-<身份>/`，报告在`cache/reports/build-coordination/<运行ID>/`。普通源文件变化沿用同树增量构建；工具、CRT/STL、ABI、后端或依赖元数据变化使用不同身份。长构建期间修改源码会令结果变为unverified，修正后须重新运行，不能重写旧报告。

## 产品装配与验收边界

windows-product每次在本轮报告下新建`product/windows/`，仅包含服务exe、完整Web文件、公共cascade和样例INI。逐文件比较集合/摘要、检查index本地引用以及PE普通/延迟动态依赖；拒绝额外旧资源、动态OpenCV/ncnn/CRT和未覆盖的非系统DLL。不把复制成功称作签名发行或真机验收。

正式CMake的`RK_COPY_DEV_MODELS`默认ON，保留开发兼容；统筹Windows固定OFF。产品没有自动复制模型，需按[模型消费者](model-storage-and-consumers.md)显式配置外部模型和独立模板空间。

Android mock/real快照位于各自run-id下，显式记录通道、APK完整SHA-256及APK内每个ABI原生库的完整SHA-256和大小；模型目录仍禁止进入APK。real通道另比较构建输出与APK内主库的`.text`及全部`SHF_ALLOC`节摘要，并记录两边文件摘要、CMakeCache与Ninja摘要。AGP会剥离非可加载调试节，因此不要求打包前后`.so`文件字节完全相同；可加载节一致只证明本次装配对应关系，不证明设备加载或推理有效。

verify先检查原build报告的源码与配置身份、归档产物原摘要，再执行该配置的检查：核心CTest；Windows混合原生用例，产品另加隔离HTTP契约；Android Java单测/lint；Web lint/Vitest及原700KB包体门槛。Android CLI仅复核配置与产物，没有设备执行。完整Cypress与覆盖率仍使用[构建与测试](build-and-test.md#cypress-e2e与覆盖率)或[测试统筹](test-suite-coordination.md)独立验收，不包含在本入口的Web verify里。

报告绑定本地未提交源码和原产物，不防对抗者同时篡改证据与清单；本地通过、远端CI、模型精度和设备加载分别判读。八组构建及资源契约变异位于`tests/scripts/test_build_mutations.py`，先通过隔离基线，再要求当前JUnit真实检出构建身份、失败传播、发布资源白名单、Android加载节绑定、CPU预算、重复资源键及内存越限回收等回归。
