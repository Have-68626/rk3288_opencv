# 重构洞察执行落地审计报告（基于 master 分支）

**审计基准**: `master` branch @ `4b4e0e2` (2026-07-19)
**审计范围**: 架构治理设计（Phase 1-3）+ 文档合规推进

---

## 执行总览

| 指标 | 值 |
|------|:-----:|
| 治理期 PR | **27 个**（#432-#466，Batch 1-5 + 架构治理 + perf 优化 + 安全修复） |
| 架构治理新增文件 | **17 个** |
| 架构治理净增代码 | **+1,819 / −70**（`native-lib.cpp` + `jni/` + `cmake/` + `ARCHITECTURE.md` 等） |
| 被修改核心文件 | `native-lib.cpp`, `CMakeLists.txt`, `MainActivity.java`, `Engine.cpp` |

---

## Phase 1: 架构基线 — ✅ 完全实施

| 交付物 | 状态 | 备注 |
|--------|:----:|------|
| `docs/architecture/ARCHITECTURE.md` | ✅ | 161 行，5 条契约 + 合规表 + 检查脚本 |
| `scripts/check-raii-violations.sh` | ✅ | 裸 lock/unlock 自动化 grep 检查 |

---

## Phase 2: 模式对齐 — ✅ 3/4 实施

| 任务 | 文件 | 状态 | 效果 |
|------|------|:----:|------|
| **2.1 ScopedWindowLock / ScopedBitmapLock** | `native-lib.cpp` | ✅ | 消除 2 处裸 lock/unlock |
| **2.2 JniCallbackThrottle** | `JniCallbackThrottle.h` (43 行) | ✅ | 4 事件独立节流（NoFace 2s / AuthFail 1s / Faces 650ms / Verified 800ms） |
| **2.3 JniMethodRegistry + JNI 拆分** | 8 文件（`jni/` ×5 + `JniMethodRegistry.h/.cpp` + `jni_shared.h`） | ✅ | `native-lib.cpp` 精简 188 行；JNI 按 `camera/engine/preview/config` 独立文件 |
| **2.4 ConfigValidator 通用化** | — | ❌ **未实施** | 依赖 `JsonSchemaValidator`（Windows），未解耦 |

---

## Phase 3: 实现填补 — ✅ 5/7 子项实施

| 任务 | 子项 | 状态 | 文件级证据 |
|------|------|:----:|-----------|
| **3.1 Java INI 移除** | 删除 persistInferenceThrottleIni / readInferenceIniIfExists | ✅ | `MainActivity.java` 0 行匹配 |
| | SharedPreferences 单源 | ✅ | 源码确认 |
| **3.2 Web 单元测试** | `http.test.ts` (34 行) | ✅ | 2 tests: settings 解析 + fetch 错误 |
| | `AppStore.test.ts` (15 行) | ✅ | 1 test: mode 切换 |
| | Vitest 3/3 PASS | ✅ | CI 验证 |
| **3.3 CMake 模块化** | `cmake/opencv.cmake` (166 行) | ✅ | OpenCV 查找 + add_subdirectory + BUILD_LIST |
| | `cmake/core.cmake` (90 行) | ✅ | rk_core INTERFACE + core_unit_tests |
| | `cmake/face_infer.cmake` (30 行) | ✅ | 推理管线源文件变量 |
| | `cmake/android.cmake` (63 行) | ✅ | native-lib target + JNI |
| | `cmake/windows.cmake` (315 行) | ✅ | 服务 + CLI + 测试 + 安装 |
| | `CMakeLists.txt` 1000→**341 行** | ✅ | −66% |
| **3.4 C++ 编译器契约** | `-fno-exceptions -fno-rtti -fvisibility=hidden` | ⚠️ **注释状态** | `rk_core` 为 INTERFACE 库，flags 无法生效，已标为待办 |

---

## 文档合规 — ✅ 100%

| 文档 | 更新日期 | 状态 | 变更范围 |
|------|:--------:|:----:|----------|
| **README.md** | 2026-07-06 | ✅ | 架构契约 + 治理进度 + 测试框架 + 目录结构，235→130 行 |
| **DEVELOP.md** | 2026-07-06 | ✅ | 架构契约引用 + GTest 表 + `cmake/jni/` 目录 + 治理文档索引 |
| **CREDITS.md** | 2026-07-06 | ✅ | 日期更新 |
| **AGENTS.md** | 无日期 | ✅ | 架构契约 + JNI 拆分 + CMake 模块化说明 |
| **CHANGELOG.md** | — | ✅ | 追加 2026-07 条目（#441-#448） |
| **docs/index.md** | — | ✅ | 新增 architecture/ superpowers/ 索引 |

---

## 治理总进度表

| 批次 | 模块 | 状态 | PR 范围 |
|:----:|:-----|:----:|:-------:|
| Batch 1 | 法律合规 + 构建基础设施 | ✅ 完成 | #432 |
| Batch 2 | C++ 核心引擎治理 | ✅ 完成 | #433 |
| Batch 3 | Windows 服务治理 | ✅ 完成 | #434 |
| Batch 4 | Web SPA + Android 治理 | ✅ 完成 | #435 |
| Batch 4.7 | MainActivity 拆分 | ✅ 完成 | #437 |
| Batch 5 | 测试覆盖治理（GTest 迁移） | ✅ 完成 | #438 |
| — | 文档合规 + 分支清理 | ✅ 完成 | #439, #440, #442 |
| — | 治理审计修复（CodeQL / 安全 / 死代码） | ✅ 完成 | #443, #466 |
| **架构治理** | **架构契约 + 模式对齐 + 实现填补** | **✅ ~90%** | **#444-#448** |
| — | 后续 perf 优化 | ✅ 完成 | #451, #453, #454 |

---

## 未完成项

| 项 | 原因 | 阻塞依赖 |
|----|------|----------|
| **ConfigValidator 通用化** | `JsonSchemaValidator` 在 `rk_win` 命名空间下，依赖 Windows 的 `JsonLite` | 需先解耦 `JsonSchemaValidator` 为跨平台库 |
| **C++ 编译器契约** | `rk_core` 是 INTERFACE 库（非 STATIC），`target_compile_options(... PRIVATE ...)` 语法被 CMake 拒绝 | 需创建实际编译的 STATIC 库 target，或将 flags 加到具体 executable target |

---

## 项目健康度对比

| 指标 | 治理前（2026-06-23） | 治理后（2026-07-19） |
|------|:-------------------:|:-------------------:|
| `Engine.cpp` 行数 | ~1294 | 874（重构前已由 Batch 2 拆分） |
| `native-lib.cpp` 行数 | 851 | **118**（−86%） |
| `CMakeLists.txt` 行数 | ~1000 | **341**（−66%） |
| 远程分支数 | 19 | **3**（−84%） |
| RAII 违规数 | 2 处（P0） | **0 处**（已修复） |
| Java INI 双源 | 200+ 行 INI 代码 | **0 行**（已删除） |
| Web 单元测试 | **0** | **3 tests（Vitest）** |
| CMake 模块 | **0** | **5 模块文件** |
| 架构契约 | **无** | **ARCHITECTURE.md（5 条）** |
| 文档日期 | 2026-06-23 | **2026-07-06** |
