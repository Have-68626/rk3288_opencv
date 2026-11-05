<!-- DOCSYNC_DOC
type=plan
status=snapshot
as_of=2026-11-05
-->

# Linux 子系统恢复（阶段 1–3）

> 本轮落地「恢复 Linux 为开发目标」的阶段 1–3：文档口径、`unit-tests` 转阻断、新增 `linux` 全量构建 job、`linux-arm-cross` 交叉编译 job，以及阶段 0 实测出的三处代码修复。阶段 4（Linux 运行时服务）与阶段 5（转必需检查）不在本计划内。

**背景：** 2026-09-17 起 Linux 被移出开发目标，`unit-tests` 降级为非阻断，Linux 侧只保留 `RK_SKIP_OPENCV=ON` 的核心快通道。2026-10-28 用户决定恢复，范围为核心与 CLI 全量验证、ARM 交叉编译检查、Linux 运行时服务（后者单独设计）。

**阶段 0 证据（2026-11-03，WSL2 Ubuntu 24.04 / GCC 13.3 / CMake 3.28.3）：**

| 模式 | 结果 |
|---|---|
| x86_64 核心（`RK_SKIP_OPENCV=ON`） | configure 24s / build 113s / `ctest` 2/2 |
| x86_64 全量（OpenCV 4.10，15 模块） | build 657/657；修复下述三项后 `ctest` 5/5 |
| armhf 全量交叉 | 1175/1175（修复后） |
| aarch64 全量交叉 | 1184/1184（修复后） |

## 代码修复

| 编号 | 位置 | 问题 | 处置 |
|---|---|---|---|
| L-01 | `src/cpp/src/FaceSearch.cpp` | NEON 段用 `std::uintptr_t` 未包含 `<cstdint>`，ARM 编译失败（x86_64 不编译该段） | 补 include |
| L-02 | `src/cpp/src/FaceInferencePipeline.cpp` | `std::atomic` 成员缺 `<atomic>`（MSVC 依赖传递包含） | 补 include |
| L-03 | `src/cpp/src/Engine.cpp` | RSS 采样只挂 `_WIN32` / `__ANDROID__`，桌面 Linux 恒 0，`EngineTest.PerformanceCsvMeasuresActualStagesAndResidentMemory` 断言失败 | 两处 `#elif` 放宽为 `__linux__`，复用 `/proc/self/statm`（Android 同样定义 `__linux__`，行为不变） |

## CI 与文档

- `unit-tests`：移除 job 级 `continue-on-error`，钉 `ubuntu-24.04`（与本地 WSL 同代），重写注释说明职责边界；job 名与 id 不变（仍是远端必需检查）。
- 新增 `linux` / `linux-build-test`：源码拉取并缓存 OpenCV 4.10 + contrib，`-DCMAKE_BUILD_TYPE=Release`，构建 `core_unit_tests`、`core_gtest_tests`、`face_infer_unit_tests`、`rk3288_cli`、`opencv_verify_cli`、`inference_bench_cli`，运行 `ctest -R 'unit_tests|core_gtest_tests|engine_run_reentry_behavior'`。**当前不是必需检查**；转门禁须改远端分支保护与 ruleset，另行授权。
- `CMakePresets.json` 新增 `linux` 预设（`cache/builds/linux-gcc13`，Release）。
- 新增 `linux-arm-cross`：armhf / aarch64 矩阵，用 `cmake/toolchains/linux-{armhf,aarch64}.cmake` 交叉构建同批目标，只编不跑，并用 `file` 断言产物架构；job 级非阻断、非必需检查。
- 同步 `AGENTS.md`（开发目标、入口表、构建约束、CI 表）、`README.md`、[构建与测试](../guides/build-and-test.md#linux-wsl)、[CI 本地映射](../guides/ci-local-mapping.md)、[构建统筹](../guides/build-coordination.md)（Linux 不在 `run-builds.py` 配置内）。

## 验证边界

- 本地 WSL 已按 `linux` job 的同构参数复跑（Ubuntu 24.04.5 / GCC 13.3 / CMake 3.28.3，默认 TIFF/OpenJPEG、`CMAKE_BUILD_TYPE=Release`、同一 `BUILD_LIST` 与目标集）：configure 546s，build 1366/1366 全绿，`ctest` 4/4 通过（`core_unit_tests`、`face_infer_unit_tests`、`core_gtest_tests`、`engine_run_reentry_behavior`）。
- 本地 WSL 已按 `linux-arm-cross` 同构参数复跑：armhf configure 576s / build 1205/1205，aarch64 configure 541s / build 1214/1214，均 0 错误；`file` 断言分别为 `ELF 32-bit … ARM, EABI5` 与 `ELF 64-bit … ARM aarch64`。只编不跑，不证明 ARM 运行或性能。
- 文档门禁 `docs-sync-audit --skip-links` high 0 / medium 0；`check-workflow-pins`、仓库卫生 scan、Node 回归与 `tests/scripts` pytest 通过（`test_device_script_behavior` 4 项在 Bash 工具的 PATH 顺序下把 `git` 解析到 `mingw64/bin` 而失败，cmd-first PATH 下 22/22 通过，属环境而非回归）。
- Windows / Android 侧未因本轮改动回归：L-01/L-02 为纯 include，L-03 只改非 Windows 分支且 `__linux__` 覆盖 Android 原有路径；两侧的真实构建留给 CI 验证。
- ncnn 在 Linux job 中未启用（`RK_HAVE_NCNN=0`），INT8 精度仍只在 Windows/本地执行。
- Linux 运行时服务、ARM 实机运行、MPP 硬解码不在本轮范围。

## 后续

- 阶段 4：Linux 运行时服务（HTTP 服务、相机、配置与加密）——设计见 [Linux 运行时服务设计](../designs/linux-runtime-service.md)（决策已定，未落地）。
- 阶段 5：`linux`、`linux-arm-cross` 稳定后转必需检查（需远端分支保护与 ruleset 授权）。
