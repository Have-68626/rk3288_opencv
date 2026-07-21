# rk3288_opencv 项目治理计划设计

**版本**: v1.0
**日期**: 2026-07-04
**依据**: AUDIT_REPORT.md 全面审计报告

---

## 1. 执行策略：按模块分批次

基于审计覆盖的 230 源文件 / 43,790 行代码 / 7 个模块评分（综合 2.9/5），采用**按模块分批治理**策略，每个批次集中处理一个模块的全部 P0‑P2 改进项。

| 批次 | 模块 | 评分 2.9→ | 工作量 | 核心改进项数 |
|:----:|:-----|:---------:|:------:|:-----------:|
| 0 | 治理看板初始化 | — | 30 min | 1 |
| 1 | 法律 + 构建基础设施 | 2.3→3.5 | ~4 天 | 7 |
| 2 | C++ 核心引擎 | 2.8→3.5 | ~5 天 | 7 |
| 3 | Windows 本地服务 | 2.65→3.5 | ~7 天 | 8 |
| 4 | Web SPA + Android | 3.7/3.1→4.0/3.8 | ~7 天 | 8 |
| 5 | 测试覆盖 | 3.0→4.0 | ~10 天 | 5 |
| *持续* | 文档合规 | 3.0→4.0 | ~3 天 | 7 |

**总预估**: ~36 天工作量，按批次顺序执行约 10-12 周。

---

## 2. 批次依赖总图

```
Batch 0  看板初始化 ─── README.md 更新
            │
Batch 1  法律+构建 ─── LICENSE + CMake + CI ─────┐
            │                                     │
            ├── Step 1: CMake 变量抽象             │
            ├── Step 2: rk_core 静态库             │
            └──                                   │
Batch 2  C++ 引擎 ────────────────────────────────┼── 需要 rk_core 库
            │                                     │
            ├── 2.3 Engine DI → 5.2 的前置          │
            └──                                   │
Batch 3  Windows 服务 ───────────────────────────┼── 可与其他批次并行
            │                                     │
Batch 4  SPA + Android ──────────────────────────┼── 与 Batch 3 可并行
            │                                     │
Batch 5  测试覆盖 ────────────────────────────────┘── 需所有模块稳定

文档合规 ─── D.1~D.7 附着在各批次尾端
```

---

## 3. Batch 0：治理看板初始化

**工作量**: 30 分钟 | **前置**: 无（计划确认后立即执行）

### 任务清单

| # | 任务 | 产出 |
|:-:|:-----|:-----|
| 0.1 | 在 README.md 追加「项目治理计划」章节，含进度跟踪表格和子任务复选框 | README.md 看板 |
| 0.2 | 添加治理进度徽标（可选） | 状态徽章 |

### 输出

```markdown
## 项目治理计划

| 批次 | 状态 | 任务数 | 完成 |
|:----:|:----:|:------:|:----:|
| Batch 0 | ✅ 完成 | 1 | 100% |
| Batch 1 | ⏳ 进行中 | 7 | 2/7 |
| Batch 2 | ⬜ 待启动 | 7 | 0/7 |
| ...     | ... | ... | ... |
```

---

## 4. Batch 1：法律 + 构建基础设施

**起点评分**: 构建/CI 2.3/5（全模块最低）
**目标评分**: 3.5/5
**工作量**: ~4 天
**前置依赖**: 无

### 任务清单

| # | 任务 | 具体位置 | 工作量 | 难度 |
|:-:|:-----|:---------|:------:|:----:|
| 1.1 | **添加 LICENSE 文件** — MIT 许可证全文 | 新建 `LICENSE`（根目录）| 5 min | 低 |
| 1.2 | **修复 CMake 编译宏泄漏** — foreach 循环外提至 if(WIN32) 块外 | CMakeLists.txt:969-979 | 30 min | 低 |
| 1.3 | **CORE_SOURCES 变量抽象** — face_infer 的 22 项 + core_test 的 5 项 + bridge 的 3 项提取为命名变量 | CMakeLists.txt | 1 小时 | 低 |
| 1.4 | **`rk_core` 静态库** — 8 个零 OpenCV 依赖文件独立成库，消费目标改为 target_link_libraries | CMakeLists.txt + 新 `src/cpp/CMakeLists.txt` | 1 天 | 中 |
| 1.5 | **修复 CI Windows PR 门控** — 移除 `if: github.event_name != 'pull_request'` | ci.yml:188 | 30 min | 低 |
| 1.6 | **OpenCV 链接变量提取** — 16 模块列表定义 CMake 变量，消除 7 处重复 | CMakeLists.txt 多处 | 2 小时 | 低 |
| 1.7 | **配置 CI ccache + Gradle 缓存** — 添加 actions/cache 步骤 | ci.yml | 1 天 | 中 |
| 1.8 | **出口合规声明** — 密码学使用声明（AES-256-GCM）| README.md 或新文件 | 1 小时 | 低 |

### 执行顺序

```
1.1 LICENSE（5分钟，最优先）
    │
1.2 CMake 宏泄漏修复（30分钟，独立）
    │
1.3 变量抽象（1小时）→ 1.4 rk_core 库（1天）→（可测试验证）
    │                          │
1.5 CI PR门控 + 1.6 OpenCV变量（独立并行）
    │
1.7 CCache + Gradle 缓存（需要 1.5 先修复）
    │
1.8 出口声明（独立）
```

### CORE_SOURCES 重构详细方案

采用**渐进式双层库**策略：

**Step 1 — CMake 变量抽象（1 小时）**：
- 提取 `RK_CORE_LITE_SOURCES`（core_unit_tests 用的 5 文件）
- 提取 `RK_FACE_INFER_SOURCES`（face_infer_unit_tests 排除的 8 文件）
- 提取 `RK_WIN_BRIDGE_SOURCES`（3 个跨平台 win 文件）
- 消除 ~120 行源列表重复

**Step 2 — rk_core 静态库（1 天）**：
- 创建 `add_library(rk_core STATIC ...)`，包含 8 个零 OpenCV 文件
- 头文件：`src/cpp/include/core/`（新建子目录）
- 消费端：`core_unit_tests` 改为 `target_link_libraries(rk_core)`
- 验证：`cmake --build` 零变化，全部测试通过

**rk_core 包含的 8 个文件**：
```
src/cpp/src/EventManager.cpp      # 纯 STL
src/cpp/src/FileHash.cpp          # 纯 SHA256
src/cpp/src/FrameInputChannel.cpp # 纯 STL 队列
src/cpp/src/ThresholdPolicy.cpp   # 纯策略模式
src/cpp/src/FaceSearch.cpp        # ARM NEON（#ifdef 保护）
src/cpp/src/FaceTemplate.cpp      # 纯数据结构
src/cpp/src/FaceInferOutcomeJson.cpp # 纯 STL
src/cpp/src/NativeLog.cpp         # 三重平台条件编译（保留）
```

> 注意：`FaceSearch.cpp`因 ARM NEON 依赖（`arm_neon.h`）放在 rk_core 但需 `#ifdef` 保护；`NativeLog.cpp` 的三重平台条件编译保留不变。

### 风险控制

| 风险 | 缓解 |
|:-----|:------|
| rk_core 头文件传递依赖 OpenCV | 审查 8 个文件的 `#include`，增加 `#error` 防护防止误引用 |
| CMake 变量改名 → 链接错误 | Step 1 → `cmake --build` 验证零变化 → Step 2 |
| CI 缓存配置错误 | 先用 `actions/cache/save` / `restore` 分步验证 |

### 验证方法

```bash
# Step 1 验证（应零变化）
cmake -S . -B build -DRK_SKIP_OPENCV=1
cmake --build build --target core_unit_tests
ctest --test-dir build -R core_unit_tests

# Step 2 验证（新库链接）
cmake --build build
ctest --test-dir build
```

---

## 5. Batch 2：C++ 核心引擎

**起点评分**: 2.8/5 | **目标评分**: 3.5/5 | **工作量**: ~5 天
**前置依赖**: Batch 1（需要 rk_core 库存在）

### 任务清单

| # | 任务 | 具体位置 | 工作量 | 难度 |
|:-:|:-----|:---------|:------:|:----:|
| 2.1 | **nativeInferFaceFromImage JNI try/catch** — 包装 RunFaceInferOnce 调用，异常时返回错误 JSON 而非崩溃 JVM | native-lib.cpp:216 | 30 min | 低 |
| 2.2 | **FaceInferStages 静态缓存解耦** — s_cachedDet/s_cachedEmb 改为 ModelRegistry 托管实例 | FaceInferStages.cpp:220-503 | 1 天 | 中 |
| 2.3 | **Engine 依赖注入** — 新增构造重载接受 unique_ptr 参数，保留默认构造 | Engine.h/.cpp 构造处 | 1 天 | 中 |
| 2.4 | **拆分 processFrame()** — 319 行 → 5 个私有方法 | Engine.cpp:739-1057 | 1 天 | 中 |
| 2.5 | **`.detach()` 替换** — CSV 写线程改用 shared_ptr 控制标志 | Engine.cpp:670 | 2 小时 | 低 |
| 2.6 | **Config.h 命名空间** — 全局 `Config` → `rk_core::config` | Config.h + 所有引用处 | 1 天 | 中 |
| 2.7 | **cpp 核心命名空间规范化** — Engine/BioAuth/EventManager/VideoManager 等添加 `rk_core` | src/cpp/include/*.h + src/*.cpp | 2 天 | 高 |

### 执行顺序

```
2.1 JNI try/catch（独立，最先执行）
    │
2.2 静态缓存 → 2.3 Engine DI → 2.4 processFrame（管线改造链，顺序依赖）
    │              │
    │              └── → 2.5 .detach（可在这个链中并行）
    │
2.6 Config 命名空间 → 2.7 Core 命名空间（命名改造链，最后执行）
```

### 风险控制

| 风险 | 缓解 |
|:-----|:------|
| Engine DI 改变构造 → 影响所有调用者（native-lib、main.cpp、测试）| 保留默认构造函数，新增参数化版本；零侵入 |
| 命名空间修改 → 大量引用处需更新 | 每个命名空间文件头添加 `using rk_core::ClassName;` 兼容过渡，按编译错误逐个清理 |
| FaceInferStages 缓存重构影响推理精度 | 重构后以 `face_infer_unit_tests` + `inference_bench_cli` 输出基线验证 |

### 验证方法

```bash
# 2.1 验证
cmake --build build --target native-lib

# 2.2~2.4 验证
cmake --build build --target face_infer_unit_tests
ctest -R face_infer

# 2.6~2.7 验证
cmake --build build
# 编译无错误即通过
```

---

## 6. Batch 3：Windows 本地服务

**起点评分**: 2.65/5 | **目标评分**: 3.5/5 | **工作量**: ~7 天
**前置依赖**: 无（可与 Batch 2 并行执行）

### 任务清单

| # | 任务 | 具体位置 | 工作量 | 难度 |
|:-:|:-----|:---------|:------:|:----:|
| 3.1 | **修复 FramePipeline 花括号缺陷** — 检查并补全 57-77 行 if 闭合，全文件确认 | FramePipeline.cpp:57-77 | 1 小时 | 低 |
| 3.2 | **注册 4 个 404 端点** — crypto/rotate + privacy/open + preview.jpg + acceleration → EndpointRegistry | JsonEndpointHandlers.cpp | 1 天 | 中 |
| 3.3 | **清理 kRoutes 死代码** — 删除 kRoutes[16] 数组 + 7 个内联处理函数 | HttpFacesServer.cpp:274-600 | 1 天 | 中 |
| 3.4 | **acceptLoop 线程模型** — .detach() → 线程池或 shared_ptr 控制标志 | HttpFacesServer.cpp:375-449 | 1 天 | 中 |
| 3.5 | **CivetWeb SSL 验证** — 历史审计项，现已废弃：CivetWeb 未编入目标；若未来接入，再按 `SSL_VERIFY_NONE → SSL_VERIFY_PEER` 与证书验证测试执行 | third_party/civetweb/civetweb.c:18341 | 不适用 | — |
| 3.6 | **拆分 WinJsonConfig** — JsonSchemaValidator + ConfigSerializer 提取 | WinJsonConfig.cpp (1474行) | 2 天 | 中 |
| 3.7 | **拆分 HttpFacesServer** — HttpParser + StreamSessionManager 提取 | HttpFacesServer.cpp (1013行) | 2 天 | 中 |
| 3.8 | **工具库提取** — escapeJson/utf8FromWide/jsonOk 统一到公共文件 | 跨约 6 个文件 | 1 天 | 中 |

### 执行顺序

```
3.1 花括号修复（最优先，独立）
    │
3.2 404端点 → 3.3 清理kRoutes（连续：先加端点再删死代码）
    │
3.4 线程修复（独立，可与3.2并行）
    │
3.5 SSL 验证（第三方单行修改，独立）
    │
3.8 工具库提取（跨文件重构，独立）
    │
3.6 WinJsonConfig 拆分 → 3.7 HttpFacesServer 拆分（大文件重构，最后执行）
```

### 验证方法

```bash
# 3.2~3.3 验证：端点功能测试
curl -v http://localhost:8080/api/v1/health
curl -v http://localhost:8080/api/v1/actions/privacy/open
curl -v http://localhost:8080/api/v1/actions/crypto/rotate

# 构建验证
cmake --build build --target win_local_service
```

---

## 7. Batch 4：Web SPA + Android

**起点评分**: SPA 3.7/5 + Android 3.1/5 | **目标评分**: SPA 4.0/5 + Android 3.8/5
**工作量**: ~7 天 | **前置依赖**: 无（可与 Batch 2/3 并行）

### 任务清单

| # | 任务 | 具体位置 | 工作量 | 难度 |
|:-:|:-----|:---------|:------:|:----:|
| 4.1 | **MJPEG 流泄漏修复** — useEffect 清理 img src | PreviewPage.tsx:164-173 | 2 小时 | 低 |
| 4.2 | **TypeScript 类型修复** — arcFaceModelPath/int8Enabled 补全；configuredPath/resolvedPath 可选 | types.ts:123-124 | 2 小时 | 低 |
| 4.3 | **翻转开关提取** — `<FlipSwitch axis="x" />` 组件 | PreviewPage.tsx:292-345 | 2 小时 | 低 |
| 4.4 | **拆分 SettingsPage** — localTab/serverTab → 独立组件 | SettingsPage.tsx (624行) | 1 天 | 中 |
| 4.5 | **AppStore prefs 依赖链修复** — 稳定值提取为 ref | AppStore.tsx:68,104 | 2 小时 | 低 |
| 4.6 | **FfmpegRtmpPusher 死代码处理** — 删除或加 URL 白名单 | FfmpegRtmpPusher.java:72-76 | 2 小时 | 低 |
| 4.7 | **拆分 MainActivity** — CameraFragment + EngineViewModel + MonitoringFragment | MainActivity.java (3155行) | 3-5 天 | 高 |
| 4.8 | **LogDetailActivity 分页加载** | LogDetailActivity.java | 1 天 | 中 |

### 执行顺序

```
4.1 MJPEG + 4.2 TS类型 + 4.5 prefs（低风险快速修复，优先执行）
    │
4.3 翻转开关 → 4.4 SettingsPage 拆分（SPA 重构链）
    │
4.6 Ffmpeg死代码（独立安全修复）
    │
4.8 日志分页（独立）
    │
4.7 MainActivity 拆分（最大重构，最后执行）
```

### 验证方法

```bash
# SPA 验证
cd web && npx tsc --noEmit   # 类型检查
npx vitest run               # 单元测试
npx vite build               # 构建

# Android 验证
cd app && ./gradlew assembleDebug
```

---

## 8. Batch 5：测试覆盖

**起点评分**: 3.0/5 | **目标评分**: 4.0/5 | **工作量**: ~10 天
**前置依赖**: Batch 2（Engine DI 完成）、Batch 3（Windows 组件稳定）

### 任务清单

| # | 任务 | 具体位置 | 工作量 | 难度 | 前置 |
|:-:|:-----|:---------|:------:|:----:|:----:|
| 5.1 | **GTest 框架迁移** — CMake find_package + 迁移 ~20 个现有用例 | CMakeLists.txt + tests/*.cpp | 2 天 | 中 | 无 |
| 5.2 | **Engine 核心测试** — 启动/停止/配置切换/错误传播 | tests/cpp/test_engine.cpp（新）| 3 天 | 高 | 2.3 |
| 5.3 | **BioAuth + MotionDetector 测试** — 阈值/行为验证 | tests/cpp/test_bioauth.cpp + test_motion.cpp（新）| 2 天 | 中 | 无 |
| 5.4 | **8 个 Adapter 独立测试** — mock/stub 模型加载 | tests/unit/test_adapters.cpp（新）| 3 天 | 中 | 无 |
| 5.5 | **Windows 组件测试** — HttpFacesServer/WinCrypto/FramePipeline | tests/win/ | 5 天 | 高 | Batch 3 |

### 执行顺序

```
5.1 GTest 基建（必须最先）
    │
5.2 Engine 测试 ←→ 5.3 BioAuth/Motion  ←→ 5.4 Adapter（三者可并行）
    │（需2.3完成）     （独立）            （独立）
    │
5.5 Windows 测试（需要 Batch 3 全部完成，最后执行）
```

### GTest 迁移策略

1. `find_package(GTest REQUIRED)` 添加到 CMakeLists.txt
2. 替换 3 份重复的 `runAll()` → `TEST()` / `EXPECT_EQ()`
3. `face_infer_unit_tests` + `core_unit_tests` + `win_unit_tests` 改为 `gtest_main` 链接
4. 现有测试用例逐个迁移（逐文件 PR，可增量提交）

---

## 9. 持续任务：文档合规

**起点评分**: 3.0/5 | **目标评分**: 4.0/5 | **工作量**: ~3 天（碎片化执行，不独占批次）

| # | 任务 | 最佳时机 | 工作量 | 热度 |
|:-:|:-----|:--------:|:------:|:----:|
| D.1 | **拆分 DEVELOP.md 附录** 到 docs/research/ | Batch 1 之后 | 1 天 | 🔴 |
| D.2 | **创建 C++ 引擎架构文档**（含状态机/管线图）| Batch 2 之后 | 1 天 | 🟡 |
| D.3 | **创建 Android 层架构文档**（JNI/安全状态机）| Batch 4 之后 | 1 天 | 🟡 |
| D.4 | **扩展 openapi.yaml**（属性 schema + 错误码枚举）| Batch 3 之后 | 1 天 | 🟡 |
| D.5 | **清理 415 个远程分支** | 交叉时段 | 1 天 | 🟢 |
| D.6 | **创建 CLAUDE.md** | Batch 0 中 | 30 min | 🟢 |
| D.7 | **实现人员注册系统 + 三层重构**（各 2-3 周）| 所有 Batch 完成后 | 5-6 周 | ⏳ |

---

## 10. 验证与验收标准

### 批次级验收门

```yaml
Gate:
  Batch 0: README.md 看板展示正确进度
  Batch 1: cmake --build 全目标通过 + ctest 全部通过
  Batch 2: cmake --build zero warning + face_infer_tests 通过
  Batch 3: win_local_service 启动 + 所有端点 200
  Batch 4: npx tsc --noEmit zero error + Android assembleDebug 通过
  Batch 5: ctest 覆盖率 > 30%（+200 测试用例）
```

### 评分验证

治理完成后重新运行 S1 度量采集脚本验证评分提升：

| 维度 | 治理前 | 目标 |
|:-----|:------:|:----:|
| 构建/CI 评分 | 2.3 | ≥ 3.5 |
| C++ 引擎评分 | 2.8 | ≥ 3.5 |
| Windows 服务评分 | 2.65 | ≥ 3.5 |
| SPA 评分 | 3.7 | ≥ 4.0 |
| Android 评分 | 3.1 | ≥ 3.8 |
| 测试评分 | 3.0 | ≥ 4.0 |
| 文档评分 | 3.0 | ≥ 4.0 |
| **综合加权** | **2.9** | **≥ 3.6** |

---

## 11. 附录：参考文件索引

- 审计主报告：`AUDIT_REPORT.md`
- S1 架构扫描：`.claude/audit/p1.1-architecture-scan.md`
- S1 Git 分析：`.claude/audit/p1.2-git-analysis.md`
- S1 安全枚举：`.claude/audit/p1.3-security-scan.md`
- S1 度量采集：`.claude/audit/p1.4-metrics.md`
- S2 C++ 引擎：`.claude/audit/p2.1-cpp-engine.md`
- S2 Windows 服务：`.claude/audit/p2.2-windows-service.md`
- S2 Android 层：`.claude/audit/p2.3-android-layer.md`
- S2 Web SPA：`.claude/audit/p2.4-web-spa.md`
- S2 构建 CI：`.claude/audit/p2.5-build-ci.md`
- S2 测试覆盖：`.claude/audit/p2.6-test-coverage.md`
- S2 文档合规：`.claude/audit/p2.7-docs-compliance.md`
- P3 跨模块契约：`.claude/audit/p3.1-cross-module-contracts.md`
- P3 全局模式：`.claude/audit/p3.2-global-patterns.md`
