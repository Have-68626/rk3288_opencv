# rk3288_opencv 项目治理计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 基于 AUDIT_REPORT.md 的 38 项发现，按模块分 6 批次渐进治理，将综合评分从 2.9/5 提升至 3.6/5+

**Architecture:** 法律合规 → 构建基础设施 → C++ 引擎 → Windows 服务 → Web/Android → 测试覆盖。每个批次独立可验证，批次间有明确依赖链。

**Tech Stack:** CMake, C++17, Gradle, React 18/TypeScript, GitHub Actions, GTest

---

## 批次结构与依赖

```
Batch 0 (看板初始化)        ─ 无依赖
    │
Batch 1 (法律+构建)          ─ 无依赖
    ├ Step 1: CMake 变量抽象
    ├ Step 2: rk_core 静态库
    └──→ Batch 2 (C++ 引擎) ─ 需要 rk_core 库
    │
Batch 2 ───────────────────── 依赖 Batch 1
    ├ 2.3 Engine DI → 5.2 前置
    └──
Batch 3 (Windows 服务) ───── 无依赖（可与 Batch 2 并行）
Batch 4 (SPA+Android) ────── 无依赖（可与 Batch 2/3 并行）
    │
Batch 5 (测试覆盖) ────────── 依赖 Batch 2 + Batch 3
```

---

## Batch 0：治理看板初始化

**目标**: 在 README.md 中写入治理进度追踪表格

**Files:**
- Modify: `README.md`

### Task 0.1: 追加治理进度章节

- [ ] **Step 1: 在 README.md 末尾追加治理计划章节**

编辑 `README.md`，在文件末尾追加：

```markdown
## 项目治理计划

> 基于 2026-07-04 全项目审计报告 ([AUDIT_REPORT.md](AUDIT_REPORT.md))，按模块分批次渐进治理。
> 设计文档: [governance-plan-design.md](docs/superpowers/specs/2026-07-04-governance-plan-design.md)

### 进度总览

| 批次 | 模块 | 评分(前→后) | 状态 | 进度 |
|:----:|:-----|:-----------:|:----:|:----:|
| Batch 0 | 治理看板初始化 | — | ✅ 完成 | 1/1 |
| Batch 1 | 法律 + 构建基础设施 | 2.3→3.5 | ⬜ 待启动 | 0/8 |
| Batch 2 | C++ 核心引擎 | 2.8→3.5 | ⬜ 待启动 | 0/7 |
| Batch 3 | Windows 本地服务 | 2.65→3.5 | ⬜ 待启动 | 0/8 |
| Batch 4 | Web SPA + Android | 3.7/3.1→4.0/3.8 | ⬜ 待启动 | 0/8 |
| Batch 5 | 测试覆盖 | 3.0→4.0 | ⬜ 待启动 | 0/5 |
| 持续 | 文档合规 | 3.0→4.0 | ⬜ 穿插 | 0/7 |

### Batch 1 子任务清单

- [ ] 1.1 添加 LICENSE 文件
- [ ] 1.2 修复 CMake 编译宏泄漏
- [ ] 1.3 CORE_SOURCES 变量抽象
- [ ] 1.4 rk_core 静态库创建
- [ ] 1.5 修复 CI Windows PR 门控
- [ ] 1.6 OpenCV 链接变量提取
- [ ] 1.7 配置 CI ccache + Gradle 缓存
- [ ] 1.8 出口合规声明
```

(后续批次的任务清单在启动时逐批追加，避免 README 过长)

- [ ] **Step 2: 提交**

```bash
git add README.md
git commit -m "docs: 追加治理计划进度看板到 README"
```

---

## Batch 1：法律 + 构建基础设施

**目标**: 修复法律合规问题 + 重构 CMake 构建体系，将评分从 2.3/5 提升至 3.5/5

### Task 1.1: 添加 LICENSE 文件

**Files:**
- Create: `LICENSE`

- [ ] **Step 1: 创建 MIT 许可证文件**

```bash
cat > LICENSE << 'EOF'
MIT License

Copyright (c) 2026 Xiangyang Xue

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
EOF
```

- [ ] **Step 2: 提交**

```bash
git add LICENSE
git commit -m "license: 添加 MIT 许可证文件（修复审计 P0.1）"
# 更新 README 看板：1.1 ✅
```

### Task 1.2: 修复 CMake 编译宏泄漏

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 识别问题代码**

当前 `CMakeLists.txt:969-979` 结构：

```cmake
if(WIN32 AND ...)
  # ... 各种 Windows 配置（约 300 行）
  foreach(t ${RK_ENGINE_TARGETS})
    target_compile_definitions(${t} PRIVATE ${RK_COMMON_DEFS})
  endforeach()
endif()
```

`RK_COMMON_DEFS` 包含 `RK_HAVE_MPP` 和 `RK_HAVE_QUALCOMM`，但 foreach 循环被 `if(WIN32)` 包裹，导致 Android/Linux 目标缺失这两个宏。

- [ ] **Step 2: 将 foreach 外提到 if(WIN32) 块外**

将循环移到 `endif()` 之后：

```cmake
# ... Windows 配置（原内容，保留）
  # ← 此处是原始 endif() 位置
endif(WIN32 AND ...)

# RK_COMMON_DEFS 应用于所有目标，不受平台限制
foreach(t ${RK_ENGINE_TARGETS})
  target_compile_definitions(${t} PRIVATE ${RK_COMMON_DEFS})
endforeach()
```

- [ ] **Step 3: 验证编译**

```bash
cmake -S . -B build \
  -DRK_SKIP_OPENCV=1 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a
cmake --build build --target core_unit_tests 2>&1 | grep -i "error"
```

Expected: 零编译错误，`RK_HAVE_MPP` 和 `RK_HAVE_QUALCOMM` 正确传播到所有 target。

- [ ] **Step 4: 提交**

```bash
git add CMakeLists.txt
git commit -m "fix(build): 将 RK_COMMON_DEFS foreach 循环外提到 WIN32 守卫外（修复 P0.3）"
```

### Task 1.3: CORE_SOURCES 变量抽象

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 提取命名变量**

在 `CORE_SOURCES` 定义之后，添加以下分组变量：

```cmake
# ── CORE_SOURCES 分组变量 ──────────────────────────
# 零 OpenCV 依赖的核心文件（用于 core_unit_tests / rk_core 库）
set(RK_CORE_LITE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/EventManager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FileHash.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FrameInputChannel.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/ThresholdPolicy.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceSearch.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceTemplate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceInferOutcomeJson.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/NativeLog.cpp
)

# face_infer_unit_tests 额外需要的 CORE 文件
set(RK_FACE_INFER_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/ArcFaceEmbedder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/BioAuth.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceAlign.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceInferencePipeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceInferStages.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/MotionDetector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/MppDecoder_stub.cpp  # 注意 stub 替代
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/Storage.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/YoloFaceDetector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/Engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/ModelRegistry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/VideoManager.cpp
)

# 跨平台 bridge 文件（位于 src/win/ 但被 Android 使用）
set(RK_WIN_BRIDGE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/win/src/FaceDetector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/win/src/LbphEmbedder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/win/src/DnnSsdFaceDetector.cpp
)

# Adapter 文件
set(RK_ADAPTER_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/ArcFaceAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/CascadeAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/DnnSsdAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/LbphAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/MobileFaceNetAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/RetinaFaceAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/SFaceAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/YoloFaceAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/YuNetAdapter.cpp
)
```

- [ ] **Step 2: 替换 core_unit_tests 的源文件列表**

在 `core_unit_tests` 目标定义处，将：

```cmake
add_executable(core_unit_tests
  src/cpp/src/FrameInputChannel.cpp
  src/cpp/src/FaceSearch.cpp
  src/cpp/src/ThresholdPolicy.cpp
  src/cpp/src/EventManager.cpp
  src/cpp/src/FileHash.cpp
  # ... 更多手动列出的文件
)
```

改为：

```cmake
add_executable(core_unit_tests
  ${RK_CORE_LITE_SOURCES}
  # ... 测试本身的 .cpp 文件
)
```

- [ ] **Step 3: 替换 face_infer_unit_tests 的源文件列表**

同样方式替换 `face_infer_unit_tests` 中手动列出的 22 个文件为 `${RK_CORE_LITE_SOURCES} ${RK_FACE_INFER_CORE_SOURCES} ${RK_ADAPTER_SOURCES}`。

- [ ] **Step 4: 验证零变化**

```bash
cmake -S . -B build -DRK_SKIP_OPENCV=1
cmake --build build --target core_unit_tests
ctest --test-dir build -R core_unit_tests
```

Expected: 编译通过，全部测试通过，行为零变化。

- [ ] **Step 5: 提交**

```bash
git add CMakeLists.txt
git commit -m "refactor(build): CORE_SOURCES 提取为命名分组变量（P0.6 step 1）"
```

### Task 1.4: rk_core 静态库

**Files:**
- Create: (rk_core 无需单独文件，仅 CMake 目标)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 创建 rk_core 静态库目标**

在 CMakeLists.txt 中添加：

```cmake
# ── rk_core：零 OpenCV 依赖的公共核心库 ──────────────
add_library(rk_core STATIC ${RK_CORE_LITE_SOURCES})

target_include_directories(rk_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include/adapters
)

# 头文件零 OpenCV 依赖防护 — 若有人误引用 OpenCV 则编译失败
target_compile_definitions(rk_core PRIVATE RK_CORE_LIBRARY)

# rk_core 的链接需求为零（纯 C++ STL）
```

- [ ] **Step 2: 迁移 core_unit_tests**

将 `core_unit_tests` 改为链接 `rk_core` 而非编译其源文件：

```cmake
add_executable(core_unit_tests
  # 从列表中移除 ${RK_CORE_LITE_SOURCES}
  # 保留测试自己的源文件
  tests/cpp/test_core.cpp
  # ...
)
target_link_libraries(core_unit_tests PRIVATE rk_core)
```

同时从 `core_unit_tests` 的编译宏中移除 `RK_SKIP_OPENCV=1`（因为 `rk_core` 默认无 OpenCV）。

- [ ] **Step 3: 验证完整编译管线**

```bash
cmake -S . -B build
cmake --build build --target rk_core         # 单独编译核心库
cmake --build build --target core_unit_tests # 链接测试
ctest --test-dir build -R core_unit_tests    # 运行测试

# 验证 Android 尚未破坏
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a
cmake --build build-android --target native-lib
```

- [ ] **Step 4: 提交**

```bash
git add CMakeLists.txt
git commit -m "refactor(build): 创建 rk_core 静态库，迁移 core_unit_tests（P0.6 step 2）"
```

### Task 1.5: 修复 CI Windows PR 门控

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: 修改 CI 触发条件**

找到 `.github/workflows/ci.yml:188` 附近：

```yaml
windows:
  runs-on: windows-latest
  if: github.event_name != 'pull_request' && (contains(github.event.head_commit.message, 'Windows') || github.event_name == 'workflow_dispatch')
```

改为：

```yaml
windows:
  runs-on: windows-latest
  if: github.event_name == 'pull_request' || github.event_name == 'push'
  # 不再限于 Windows 关键词；改用路径过滤避免不必要的触发
  paths:
    - 'src/win/**'
    - 'CMakeLists.txt'
    - '.github/workflows/ci.yml'
```

- [ ] **Step 2: 提交**

```bash
git add .github/workflows/ci.yml
git commit -m "fix(ci): Windows job 针对 PR 和 win/ 路径变更触发（P0.7）"
```

### Task 1.6: OpenCV 链接变量提取

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 定义 OpenCV 模块列表变量**

在 CMakeLists.txt 的变量定义区域添加：

```cmake
# ── OpenCV 模块分组变量 ──────────────────────────────
set(RK_OPENCV_FULL_LIBS
  ${OpenCV_LIBS}  # 使用 find_package 提供的全量变量替代手动列表
)

# 或更精确的手动分组（如果 OpenCV_LIBS 不可靠）：
set(RK_OPENCV_CORE_LIBS opencv_core opencv_imgproc)
set(RK_OPENCV_FULL_LIBS
  opencv_core opencv_imgproc opencv_video opencv_objdetect
  opencv_face opencv_imgcodecs opencv_calib3d opencv_features2d
  opencv_flann opencv_dnn opencv_ml opencv_photo opencv_videoio
  opencv_highgui opencv_stitching
)
```

- [ ] **Step 2: 替换 7 处 target_link_libraries**

搜索 `target_link_libraries` 后紧跟的 `opencv_core opencv_imgproc ...` 长列表，全部替换为 `${RK_OPENCV_FULL_LIBS}`。

对于仅需子集的目标（如 `win_local_service` 只需 5 个模块），保留手动列表但引用分组变量：

```cmake
target_link_libraries(win_local_service PRIVATE
  ${RK_OPENCV_CORE_LIBS}
  opencv_objdetect opencv_imgcodecs opencv_dnn
)
```

- [ ] **Step 3: 验证**

```bash
cmake -S . -B build
cmake --build build --target win_local_service
cmake --build build --target face_infer_unit_tests
ctest --test-dir build
```

- [ ] **Step 4: 提交**

```bash
git add CMakeLists.txt
git commit -m "refactor(build): OpenCV 链接列表提取为 CMake 变量，消除 7 处重复"
```

### Task 1.7: 配置 CI 构建缓存

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: 添加 ccache 缓存步骤**

在 `ci.yml` 的构建 job 中添加：

```yaml
- name: Cache ccache
  uses: actions/cache@v4
  with:
    path: ~/.ccache
    key: ccache-${{ runner.os }}-${{ hashFiles('CMakeLists.txt', 'src/cpp/**/*.cpp', 'src/cpp/**/*.h') }}
    restore-keys: |
      ccache-${{ runner.os }}-

- name: Configure ccache
  run: |
    ccache --max-size=500M
    echo "CCACHE_DIR=~/.ccache" >> $GITHUB_ENV
```

- [ ] **Step 2: 验证 CI**

推送并查看 GitHub Actions 运行结果：

```bash
git add .github/workflows/ci.yml
git commit -m "ci: 添加 ccache 缓存配置"
git push
```

到 GitHub 页面验证 Action 运行正常。

### Task 1.8: 出口合规声明

**Files:**
- Modify: `README.md`

- [ ] **Step 1: 在 README 中添加密码学声明**

在 README 的设备约束或构建说明区域添加：

```markdown
### 密码学声明

本项目使用 AES-256-GCM（通过 Android KeyStore / Windows BCrypt）进行人脸特征模板加密。
密码学功能仅用于本地数据保护，不涉及通信加密或数字签名。
```

- [ ] **Step 2: 提交**

```bash
git add README.md
git commit -m "docs: 添加密码学出口合规声明"
```

---

## Batch 2：C++ 核心引擎（框架）

**目标**: 修复 JNI、依赖注入、命名空间规范化，评分 2.8→3.5
**前置**: Batch 1 完成（需要 rk_core 库）

### Task 2.1: nativeInferFaceFromImage JNI try/catch

**Files:**
- Modify: `src/cpp/native-lib.cpp`

- [ ] **Step 1: 用 try/catch 包裹 JNI 函数体**

```cpp
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeInferFaceFromImage(
    JNIEnv* env, jobject /*thiz*/,
    jstring jImgPath, jstring jModelDir, jstring jDetectorType,
    jstring jEmbedderType, jint jMaxFaces, jfloat jConfidence,
    jboolean jUseTracking, jboolean jDebug) {
  try {
    // ... 现有实现
    auto result = runFaceInferOnce(...);
    return env->NewStringUTF(result.c_str());
  } catch (const std::exception& e) {
    // 返回错误 JSON 而非崩溃 JVM
    std::string errJson = "{\"ok\":false,\"error\":{\"code\":\"INFER_FAILED\",\"message\":\"";
    errJson += e.what();
    errJson += "\"}}";
    return env->NewStringUTF(errJson.c_str());
  } catch (...) {
    return env->NewStringUTF("{\"ok\":false,\"error\":{\"code\":\"UNKNOWN\",\"message\":\"Unknown JNI error\"}}");
  }
}
```

- [ ] **Step 2: 提交**

```bash
git add src/cpp/native-lib.cpp
git commit -m "fix(jni): nativeInferFaceFromImage 添加 try/catch 保护（P0.5）"
```

### Task 2.2: FaceInferStages 静态缓存解耦

**Files:**
- Modify: `src/cpp/src/FaceInferStages.cpp`
- Modify: `src/cpp/include/FaceInferStages.h`

(Task 详细步骤在启动 Batch 2 时展开)

### Task 2.3: Engine 依赖注入

**Files:**
- Modify: `src/cpp/include/Engine.h`
- Modify: `src/cpp/src/Engine.cpp`

(Task 详细步骤在启动 Batch 2 时展开)

### Task 2.4~2.7

(Task 详细步骤在启动 Batch 2 时展开)

---

## Batch 3：Windows 服务（框架）

**目标**: 修复花括号缺陷、404 端点、大文件拆分，评分 2.65→3.5
**前置**: 无（可与 Batch 2 并行）

### Task 3.1: FramePipeline 花括号修复

**Files:**
- Modify: `src/win/src/FramePipeline.cpp`

- [ ] **Step 1: 检查并补全 57-59 行花括号**

读取 FramePipeline.cpp:55-80 区域，确认 `if (!logger_.open(...))` 是否有闭合 `}`。如缺失则补全：

```cpp
if (!logger_.open(...)) {
  // 处理失败...
}  // ← 补全此闭合花括号
```

- [ ] **Step 2: 全文件花括号审计**

```bash
# 用编译器警告检查
g++ -fsyntax-only -Wall -Wextra src/win/src/FramePipeline.cpp 2>&1 | grep -i "brace\|missing\|expected"
```

- [ ] **Step 3: 提交**

```bash
git add src/win/src/FramePipeline.cpp
git commit -m "fix: FramePipeline.cpp 花括号缺陷修复（P0.2）"
```

### Task 3.2~3.8

(Task 详细步骤在启动 Batch 3 时展开)

---

## Batch 4：Web SPA + Android（框架）

**目标**: 修复 MJPEG 泄漏、MainActivity 拆分、TS 类型对齐，评分 SPA 3.7→4.0 / Android 3.1→3.8

(Task 详细步骤在启动 Batch 4 时展开)

## Batch 5：测试覆盖（框架）

**目标**: GTest 迁移 + 核心模块测试补齐，评分 3.0→4.0

(Task 详细步骤在启动 Batch 5 时展开)

## 持续：文档合规（框架）

**目标**: DEVELOP 拆分 + 架构文档 + 分支清理，评分 3.0→4.0

(Task 详细步骤在各批次间隙执行)

---

## 验证

治理完成后重新运行审计度量：

```bash
# 文件统计
find src/ -name "*.cpp" -o -name "*.h" -o -name "*.java" | xargs wc -l | sort -rn | head -10

# 构建验证
cmake --build build --target all
ctest --test-dir build --output-on-failure

# 测试覆盖率（GTest 版本）
ctest --test-dir build -T coverage

# SPA 验证
cd web && npx tsc --noEmit && npx vitest run

# Git 统计（验证分支清理）
git branch -r | wc -l  # 期望: < 50
```
