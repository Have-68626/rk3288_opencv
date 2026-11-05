<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-11-05
-->

# 构建与测试

以下命令从仓库根目录执行。Android、Windows 与 Linux 均为开发目标；Linux 用 WSL 构建，见本页「Linux（WSL）」一节，CI 中由 `linux` job 覆盖全量 OpenCV 构建。依赖版本与来源见[CREDITS](../../CREDITS.md)，本页统一维护操作前提和命令。

新建独立CMake树放在 `cache/builds/`；已有树含绝对路径，不直接搬移。示例中的OpenCV路径须替换为本机源码目录，不能指向安装目录。

本地CMake预设名称保持default/windows/linux/android，新树分别进入cache/builds/core-ninja、windows-vs2022-x64、linux-gcc13、android-ninja-armeabi-v7a。批处理CLI独立树另以用途/ABI命名；已有树不直接移动。Android真实测试源目录见[模块入口](../../app/README.md)。

需要固定配置、隔离构建树及Web→Windows独占产品装配时，使用[本地构建统筹](build-coordination.md)。构建与verify分别出具报告，保留本页原生入口及现有验收门槛。

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

<a id="linux-wsl"></a>
## Linux（WSL）

前提：WSL2 Ubuntu 24.04（GCC 13、CMake ≥3.28、Ninja），以及 OpenCV 与 opencv_contrib 源码目录（版本与 CI 一致，4.10.0）。构建树放仓库内 `cache/builds/linux-gcc13`。

```bash
# WSL 默认继承 Windows PATH；不净化时 CMake 会把 Anaconda 的 Windows 版 OpenJPEG/tiff 当成依赖
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export OPENCV_ROOT=/mnt/d/ProgramData/OpenCV/opencv-4.10.0
export OPENCV_CONTRIB_ROOT=/mnt/d/ProgramData/OpenCV/opencv_contrib-4.10.0
cmake --preset linux
cmake --build --preset linux -j "$(nproc)" --target \
  core_unit_tests core_gtest_tests face_infer_unit_tests \
  rk3288_cli opencv_verify_cli inference_bench_cli
ctest --preset linux -R 'unit_tests|core_gtest_tests|engine_run_reentry_behavior' --output-on-failure
```

只跑核心快通道（不带 OpenCV，约两分钟）：

```bash
cmake -S . -B cache/builds/linux-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DRK_SKIP_OPENCV=ON -DRK_BUILD_CORE_UNIT_TESTS=ON
cmake --build cache/builds/linux-core --target core_unit_tests -j "$(nproc)"
ctest --test-dir cache/builds/linux-core -R '^core_unit_tests$' --output-on-failure
```

注意：

- Ninja 单配置必须显式 `CMAKE_BUILD_TYPE`（`linux` 预设已设 Release）；缺省时 OpenCV 自带的 libjpeg-turbo 在 configure 阶段报 `string no output variable specified`。
- GitHub 网络抖动时 FetchContent 拉 googletest 会连续失败；可先克隆 `release-1.12.1` 到 `cache/deps/googletest`，再加 `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<该目录>`。
- 若仍解析到 Windows 侧包，先删除 `CMakeCache.txt` 重配；需要确定性时可加 `-DWITH_TIFF=OFF -DWITH_OPENJPEG=OFF`（本用途不需要这两个编码）。
- `scripts/run-builds.py` 的各配置以 Windows 宿主为前提，不覆盖 Linux；Linux 以本节命令为准。

ARM 交叉编译（只编不跑，工具链文件在 `cmake/toolchains/`；本机需先安装 `g++-arm-linux-gnueabihf` 与 `g++-aarch64-linux-gnu`）：

```bash
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export OPENCV_ROOT=/mnt/d/ProgramData/OpenCV/opencv-4.10.0
export OPENCV_CONTRIB_ROOT=/mnt/d/ProgramData/OpenCV/opencv_contrib-4.10.0
for a in armhf aarch64; do
  cmake -S . -B "cache/builds/linux-$a-cross" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/linux-$a.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DOPENCV_ROOT="$OPENCV_ROOT" \
    -DOPENCV_CONTRIB_ROOT="$OPENCV_CONTRIB_ROOT"
  cmake --build "cache/builds/linux-$a-cross" -j "$(nproc)" --target \
    core_unit_tests core_gtest_tests face_infer_unit_tests \
    rk3288_cli opencv_verify_cli inference_bench_cli
  file "cache/builds/linux-$a-cross/bin/rk3288_cli"   # 确认确为目标架构
done
```

交叉编译通过只证明可在该架构编译，不证明运行、性能或 RK3288 真机可用。

Linux 运行时服务（阶段 4 初版；需要 `libssl-dev` 与 `libcurl4-openssl-dev`）：

```bash
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export OPENCV_ROOT=/mnt/d/ProgramData/OpenCV/opencv-4.10.0
export OPENCV_CONTRIB_ROOT=/mnt/d/ProgramData/OpenCV/opencv_contrib-4.10.0
cmake --preset linux
cmake --build --preset linux -j "$(nproc)" --target linux_local_service
# 冒烟：默认绑定 127.0.0.1:8080，配置走 XDG（$XDG_CONFIG_HOME/rk_wcfr 或 ~/.config/rk_wcfr）
RK_WCFR_CONFIG=/tmp/rk-wcfr/config.json ./cache/builds/linux-gcc13/bin/linux_local_service &
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/api/v1/health   # 期望 200
kill -TERM %1
```

缺模型资产时健康接口返回 `degraded` 属预期；密钥文件在 Linux 上以 0600 权限保存（无 DPAPI 等价物），密文格式与 Windows 使用相同 AAD 字节即可互通（跨平台向量测试待补）。

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


需要串行协调多个套件并汇总新鲜证据时，使用[本地测试统筹](test-suite-coordination.md)。先list/plan确认前置，再明确run范围；统筹结果不替代远端CI或真机验收。
