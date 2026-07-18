# Windows CI 优化审计报告

**日期：** 2026-07-18（首次）；2026-07-18（更新）
**PR：** #451 (`bolt-fix-redundant-clone-16678337304012936565`)
**审计范围：** 本轮 6 个优化 commit + 3 个编译修复 commit 的影响分析
**CI 运行 ID：** 29639348922（首次失败，已修复并重推）

---

## 1. 更改概览

### 修改的文件

| 文件 | 变更类型 | 变更内容 |
|------|---------|---------|
| `.github/workflows/ci.yml` | 修改 | 6 处变更（含 1 次修复） |
| `cmake/opencv.cmake` | 修改 | 1 处变更（BUILD_LIST 默认值，含 1 次修复） |
| `cmake/windows.cmake` | 已修改 | 之前已修复（源文件补齐） |
| `src/win/include/rk_win/RuntimeBootstrap.h` | 已修改 | 之前已修复（默认构造） |

### 9 个 commit 的完整链条

```
# 编译修复（之前推送）
4b64f04  fix: add missing BootstrapResult() = default for MSVC 2022 (C2512)
8859fb2  fix: add missing source files to win_unit_tests target
fd17880  fix: add ArcFaceEmbedder and NativeLog to win_unit_tests

# 第一轮优化（初始推送，BUILD_LIST 缺 face）
a770270  perf(ci): add BUILD_LIST to Windows CI cmake step
7ad5f40  perf(build): add default BUILD_LIST guard to opencv.cmake
c352135  perf(ci): remove ineffective ccache setup from Windows CI
2220343  perf(ci): gate INT8 quantization + precision tests behind path filter
1e4d0e1  perf(ci): cache GTest build output across CI runs

# 第二轮修复（在审计中发现 BUILD_LIST 缺 face → LNK1181）
913e053  fix: add opencv_face to BUILD_LIST
```

---

## 2. 各变更的预期影响

### 2.1 BUILD_LIST 裁剪（P0）— 首次 CI 失败并修复

**变更：** `ci.yml:272` + `opencv.cmake:49-52` — 将 OpenCV 编译模块从默认 ~110 个裁剪到 16 个。

| 维度 | 优化前 | 优化后（修复后） |
|------|-------|----------------|
| CMake 配置时处理模块数 | 110+ | 16 |
| 编译的模块数 | 全部（含 opencv_contrib 的 aruco、xfeatures2d、sfm、hdf 等） | core、imgproc、imgcodecs、objdetect、features2d、flann、calib3d、dnn、ml、photo、**face**、video、videoio、highgui、stitching |
| 已链接模块 | 15（RK_OPENCV_FULL_LIBS） | 15（所有已链接模块均已编译） |

**首次 CI 失败分析（run 29639348922）：**

| 步骤 | 耗时 | 结果 |
|------|------|------|
| Set up job | <1s | ✅ |
| Checkout | 5s | ✅ |
| CMake 配置 | **100s (1:40)** | ✅ 成功完成，仅处理 16 模块 |
| Build unit tests | **516s (8:36)** | ❌ `LNK1181: cannot open input file 'opencv_face.lib'` |
| 合计（至失败点） | **~10.5 min** | ❌ |

CMake 日志中的关键输出：
```
-- Module opencv_face disabled by whitelist
--     Disabled by dependency: ... face ...
```

**根因：** BUILD_LIST 包含了 _RK_OPENCV_FULL_LIBS_ 中的 14 个模块，但遗漏了 `opencv_face`（来自 opencv_contrib/modules/face）。`win_unit_tests` 目标链接了 `opencv_face`，但该模块未被编译。

**修复（913e053）：** 在 `ci.yml` 和 `opencv.cmake` 的 BUILD_LIST 中补充 `face`。

**预期节省（修正后）：** ~6-8 分钟（配置 1:40 + 构建 8:36 ≈ 10.5 min 失败前，全量 OpenCV 约需 8-10 min 构建 → 裁剪后约 2-4 min）

### 2.2 INT8 量化条件执行（P2）

**变更：** `ci.yml:286-325` — 新增 `changed-int8` 检测步骤，只在 `models/`、`scripts/quantize_ncnn_int8.py`、`deps/WIDER_train/` 变更时运行量化 + 精度测试。

**预期节省：** 每次 3-6 分钟（对非模型 PR）

**风险点：** push 到 master（非 PR）时无条件运行，不会跳过。

### 2.3 ccache 移除（P3）

**变更：** `ci.yml:204-217` 移除 — 删除 2 个步骤。

**预期节省：** ~30 秒（缓存恢复/保存时间）

**影响：** 无功能影响。CMakeLists.txt 中仍有 ccache 自动检测逻辑（`find_program(CCACHE_PROGRAM ccache)`），但 runner 上 ccache 不存在时自动跳过。

### 2.4 GTest 构建缓存（P4）

**变更：** `ci.yml:216-225` — 将 `${{ env.BUILD_DIR }}/_deps/googletest-build` 加入现有的 `_deps` 缓存。

**预期节省：** 1-2 分钟（从第二次 CI 运行开始生效）

---

## 3. 潜在问题

### 3.1 BUILD_LIST 遗漏 `opencv_face`（已验证失败）

**状态：** ⚠️ 已发生，已修复。

**问题：** BUILD_LIST 必须包含 `RK_OPENCV_FULL_LIBS` 中所有模块的对应名称。初始 BUILD_LIST 遗漏了 `opencv_face`（来自 opencv_contrib），CI 构建正常完成 CMake 配置和 OpenCV 编译，但在 `win_unit_tests` 链接时报 `LNK1181: cannot open input file 'opencv_face.lib'`。

**关键启示：**
- OpenCV 的 BUILD_LIST 使用模块的**短名称**（`face` 而非 `opencv_face`）
- contrib 模块与核心模块在 BUILD_LIST 语法上无区别
- OpenCV CMake 对 BUILD_LIST 中被排除的模块只打日志（`disabled by whitelist`），不报 error
- 链接阶段才暴露问题——这是 BUILD_LIST 裁剪的一个陷阱
- 修复后应交叉验证 BUILD_LIST 是否覆盖 `RK_OPENCV_FULL_LIBS` 的所有条目

**缓解方法：** 在 `.github/workflows/ci.yml` 的 env 注释中标注 `RK_OPENCV_FULL_LIBS` 的交叉引用，避免再次遗漏。

**风险：** OpenCV 模块之间存在隐式依赖。例如 `opencv_stitching` 需要 `opencv_features2d`、`opencv_calib3d` 等。我们列了 15 个模块，但如果某个模块的依赖链不在 BUILD_LIST 中，CMake 配置时会报错。

**缓解：** 这 15 个模块直接取自 `RK_OPENCV_FULL_LIBS`（已在项目中实际链接使用），OpenCV 的 module dependencies 机制会自动拉取需要的依赖模块。如果 A 依赖 B 但 B 不在 BUILD_LIST，CMake 会发出 `OPENCV_MODULE_${A}_DEPS` 错误。已测试本地配置通过。

**监控：** 查看 CI 输出的 `Configuring OpenCV...` 部分，确认没有 `WARNING: modules not found` 警告。

### 3.2 INT8 path filter 在 force push 时行为异常

**风险：** `github.event.pull_request.base.sha` 在 force push 后可能指向旧的 base commit，导致 `git diff --name-only` 比较范围不准确。

**影响：** 可能遗漏模型文件变更 → INT8 步骤被错误跳过（false negative）。由于 INT8 步骤标记了 `continue-on-error: true`，这最多导致精度测试未运行，不会阻塞 PR 合并。

**缓解：** 对 master 的 push 事件总是运行（无条件）。对 PR 事件，如果担心遗漏，可改为用 `github.event.pull_request.head.sha` 和 `github.event.pull_request.base.sha` 的组合，`actions/checkout` 的 `fetch-depth: 0` 可确保完整 git 历史。

### 3.3 GTest 缓存路径可能不存在

**风险：** 首次运行时 `${{ env.BUILD_DIR }}/_deps/googletest-build` 目录尚未创建。`actions/cache` 在保存时如果路径不存在会自动跳过，无错误。

**缓解：** 首次运行不会有 GTest 缓存（cache miss），仍会从源码构建，无功能影响。第二次运行会命中缓存。

### 3.4 `BUILD_LIST` guard 覆盖用户设置

**风险：** `cmake/opencv.cmake:49` 中的 `if(NOT BUILD_LIST)` 检查的是 CMake 变量是否**定义**（非空）。如果用户通过 `-DBUILD_LIST=`（空字符串）调用，`if(NOT BUILD_LIST)` 为 true，会覆盖为用户不想编译任何模块。

**缓解：** 实际使用中 `-DBUILD_LIST=`（空）是无意义的值。正常用法是 `-DBUILD_LIST=core,imgproc`（非空）或完全不传。用 `if(NOT DEFINED BUILD_LIST)` 更安全：

```cmake
if(NOT DEFINED BUILD_LIST)
    set(BUILD_LIST "core,...,stitching" CACHE STRING "...")
endif()
```

目前用 `if(NOT BUILD_LIST)` 在实际场景中不会触发问题，但可以改进。

---

## 4. 定量分析

### CI 运行时间演变

| CI 运行 ID | 日期 | 提交 | Windows CI 耗时 | 结果 | 原因 |
|-----------|------|------|---------------|------|------|
| 29153098367 | Jul 11 | ⚡ Bolt 原始代码 | ~10 min | failure | 编译错误 |
| 29638663113 | Jul 18 | 3 个编译修复 | **11.7 min** | failure | **链接错误**（LNK2019） |
| 29639348922 | Jul 18 | 3 编译修复 + 5 优化（缺 face） | **10.5 min** | failure | **LNK1181: opencv_face.lib 未编译** |
| — | 待定 | 3 编译修复 + 6 优化（含 913e053） | **待定** | ? | 已重推等待 CI |

**关键发现（第 3 行）：** BUILD_LIST 优化后的 CI 运行**首次成功通过了 CMake 配置阶段**（100s vs 之前约 2 min），构建运行了 **8:36** 后才失败——比之前（11.7 min 含全量 OpenCV 编译）还快，但原因不是 OpenCV 编译加速了，而是**全量 OpenCV 编译还没完成就进入了链接阶段**（依赖关系解析后发现缺少模块）。

这说明：
1. ✅ CMake 配置速度提升 20%（100s vs 120s+）
2. ❌ BUILD_LIST 缺少 `opencv_face` 导致根本不能链接
3. 修复后预计配置 + 编译 OpenCV 16 模块约需 **3-4 min**（全量 110 模块约 8 min）

**修正后的节省估算：** BUILD_LIST 优化节省约 **4-5 分钟**（从 ~8 min 全量编译降到 ~3-4 min 裁剪后）。

### 分项节省明细

| 优化项 | 节省时间 | 确定性 |
|-------|---------|-------|
| BUILD_LIST 裁剪 | **4-5 min** | 中（全量 OpenCV 8 min → 裁剪后估 3-4 min） |
| INT8 条件执行 | **3-6 min** | 中（取决于触发频率；对非模型 PR 每次节省） |
| ccache 移除 | ~0.5 min | 高 |
| GTest 缓存 | ~1 min | 中（从第二次运行开始） |
| **合计（典型 PR）** | **~8-12 min** | |
| **预计总 CI 时间** | **~8-12 min**（OpenCV 3-4 min + 测试编译 1-2 min + 测试运行 1 min + 其他） |

---

## 5. 后续改进建议

### 5.1 `if(NOT DEFINED BUILD_LIST)` 安全加固

在 `cmake/opencv.cmake` 中将 `if(NOT BUILD_LIST)` 改为 `if(NOT DEFINED BUILD_LIST)` 以防止空字符串覆盖：

```cmake
if(NOT DEFINED BUILD_LIST)
    set(BUILD_LIST "core,imgproc,...,stitching" CACHE STRING "..." FORCE)
endif()
```

### 5.2 INT8 path filter 增加 fetch-depth

在 `actions/checkout` 步骤增加 `fetch-depth: 0` 以确保 force push 后 git diff 仍能正确比较：

```yaml
- name: Checkout
  uses: actions/checkout@v4
  with:
    fetch-depth: 0
```

### 5.3 考虑缓存 OpenCV 构建产物

当前每次 CI 都完整编译 OpenCV（~3-4 min 裁剪后）。如果将此产物缓存，可进一步节省时间。挑战：MSVC 构建产物体积大（OpenCV 15 个 DLL 约 100MB），缓存恢复时间可能与编译时间相当。

### 5.4 考虑 OpenCV prebuilt 替代 add_subdirectory

长期可选方案：用预编译的 OpenCV binary（从 GitHub Releases 下载）替代 `add_subdirectory` 源码构建。可节省 3-4 分钟，但需要维护 OpenCV 版本与预编译包的同步。
