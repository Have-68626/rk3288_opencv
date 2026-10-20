<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-19
-->

# 构建与测试

以下命令从仓库根目录执行。只开发Android与Windows；CI中的Linux编译信号保留为参考。依赖版本与来源见[CREDITS](../../CREDITS.md)，本页统一维护操作前提和命令。

新建独立CMake树放在 `cache/builds/`；已有树含绝对路径，不直接搬移。示例中的OpenCV路径须替换为本机源码目录，不能指向安装目录。

本地CMake预设名称保持default/windows/android，新树分别进入cache/builds/core-ninja、windows-vs2022-x64、android-ninja-armeabi-v7a。批处理CLI独立树另以用途/ABI命名；已有树不直接移动。Android真实测试源目录见[模块入口](../../app/README.md)。

## C++核心快通道（PowerShell）

```powershell
cmake -S . -B cache/builds/core-tests -G Ninja -DRK_SKIP_OPENCV=ON
cmake --build cache/builds/core-tests --target core_unit_tests
ctest --test-dir cache/builds/core-tests -R '^core_unit_tests$' --output-on-failure
```

此处只验收已构建的核心测试，不表示完整推理或全部CI通过。

## Windows服务与选定测试（PowerShell）

前提：VS2022 x64工具链、OpenCV源码及opencv_contrib源码。ncnn默认关闭；需要它时另指定 `RK_ENABLE_NCNN=ON` 和匹配静态CRT的安装，见[构建怪异项](../../AGENTS.md#essential-build-quirks)。

```powershell
cmake -S . -B cache/builds/windows-tests -G 'Visual Studio 17 2022' -A x64 `
  -DOPENCV_ROOT='path/to/opencv' -DOPENCV_CONTRIB_ROOT='path/to/opencv_contrib'
cmake --build cache/builds/windows-tests --config Release --target win_local_service
```

上述仅构建服务。运行测试前须构建对应可执行文件，不能随即执行未筛选的全体CTest：

```powershell
cmake --build cache/builds/windows-tests --config Release --target core_unit_tests win_unit_tests face_infer_unit_tests win_face_database_perf
ctest --test-dir cache/builds/windows-tests -C Release `
  -R '^(core_unit_tests|win_unit_tests|face_infer_unit_tests|win_face_database_perf)$' --output-on-failure
```

这是选定四个测试目标，非全量CI。其他目标或独立ncnn精度测试须先构建，再用相应CTest名称筛选；精度缺输入的跳过不等于精度通过。框架与跳过说明见[DEVELOP](../../DEVELOP.md#43-测试框架)。

本机原生程序若在系统临时目录写入时返回Windows错误码5，应先核验目录访问权限。可在独立测试进程内将TEMP/TMP指向本次cache/reports下的可写临时目录，并给pytest显式传入--basetemp；不改系统环境或目录ACL。CTest与Python回归分别使用独立临时目录，保留原失败记录后完整复跑，不能跳过工具行为用例。

## Android

PowerShell：

```powershell
.\gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

POSIX宿主（仅作为Android构建宿主）：

```bash
./gradlew --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

真实后端需要正确的OpenCV/ncnn安装及双ABI配置。显式mock构建 `-PRK_SKIP_OPENCV=true` 只用于对应桩测试，不证明真实识别可用。Java、NDK及ABI约束见[助手指南](../../AGENTS.md#essential-build-quirks)。

跨平台共享NCNN_DIR时，Android构建须在当前进程将其指向含armeabi-v7a和arm64-v8a的Android SDK根，而非Windows的x64 SDK。Gradle依据两个ABI目录是否存在选择RK_ENABLE_NCNN；路径不匹配会关闭ncnn，assembleDebug成功不能证明启用。验收须核对两ABI的CMakeCache及native-lib编译定义，并将APK的.text摘要绑定到对应构建产物。不覆写系统环境变量。

## Web单测与构建

```powershell
pnpm -C web install --frozen-lockfile
pnpm -C web lint
pnpm -C web test
pnpm -C web build
```

产物在 `src/win/app/webroot/`，受Git跟踪。依赖安装不等于Cypress二进制可用；先确认本机已有与锁文件匹配的Cypress缓存，不擅自清理旧缓存。

## Cypress E2E与覆盖率

Windows完整驱动会准备带插桩的Vite、等待就绪、运行E2E及覆盖率报告，并清理自己的作业：

```powershell
pwsh -NoProfile -File scripts/run-web-e2e.ps1
```

手动运行须分两个终端；普通E2E也需要预先启动服务器。以下PowerShell示例提供插桩代码，API由现有Cypress用例隔离，不是真实后端验收。

终端一，从仓库根进入Web目录：

```powershell
Set-Location web
$env:CYPRESS_COVERAGE = '1'
node node_modules/vite/bin/vite.js --host 127.0.0.1 --port 5173 --strictPort
```

终端二，从仓库根执行：

```powershell
$env:CYPRESS_BASE_URL = 'http://127.0.0.1:5173'
pnpm -C web e2e:run:coverage
if ($LASTEXITCODE -ne 0) { throw 'E2E失败，停止覆盖率验收' }
pnpm -C web coverage:report
if ($LASTEXITCODE -ne 0) { throw '覆盖率未达到现有门槛' }
```

`e2e:run` 只调用Cypress；`e2e:run:coverage` 负责子进程环境隔离、旧计数归档及执行，不启动Vite，也不代替单独的覆盖率门槛检查。服务器端须在启动前设置插桩变量；仅给Cypress设置变量无效。当前插件默认只插桩serve；`vite build`/`pnpm preview` 不能替代上述覆盖率服务器，preview默认端口也不同。

手动终止服务器后，在各自终端清除本次设置的变量。不要停止用户原有服务或覆写系统环境变量。一次E2E通过不代替Windows真实API或设备验收。

## 本地门禁与验收边界

文档修改运行 `node scripts/docs-sync-audit.js --skip-links` 与 `git diff --check`。完整CI的job与非门禁边界见[CI说明](../../AGENTS.md#ci-pipeline)；本页的选定命令不能冒充全量CI。
