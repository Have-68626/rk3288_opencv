# 未完成任务改进方案（修订版）

**日期**: 2026-07-19

---

## 任务 A: ConfigValidator 通用化

### 深入调查结论

经过完整依赖分析：

| 发现 | 详情 |
|------|------|
| `JsonLite` 与 `JsonSchemaValidator` 的消费者 | **全部在 Windows 侧**（`HttpFacesServer`、`WinJsonConfig`、`JsonEndpointHandlers`） |
| Core Engine 引用 | **0 处** — `src/cpp/` 和 `tests/` 中无任何文件引用这两个头文件 |
| 原始设计目标 | 为 `Config` 类提供校验 → `Config.h` **已是 header-only 编译期常量**，无运行时加载，无需校验 |
| ARCHITECTURE.md 修正 | 已在 PR #444 中将 Config 标记为 **"已合规（无需修改）"** |
| `core_unit_tests` 现状 | 已编译 `src/win/src/JsonLite.cpp`（`cmake/core.cmake:72`），用于测试中解析 JSON |

### 方案

**结论：ConfigValidator 通用化无实际消费者，建议关闭此任务。**

理由：
1. `Config` 类无需校验 — 已是编译期常量
2. Core Engine 无其他组件需要使用 JSON Schema 校验
3. 如未来需要，`JsonLite` 已可通过 `core_unit_tests` 的现有构建路径直接使用

### 后续建议

如未来跨平台需要 JSON Schema 校验，推荐方案是（无需现在实施）:
- 将 `src/win/src/JsonLite.cpp` 加入 `RK_CORE_LITE_SOURCES`（仅需在 `cmake/core.cmake` 加一行）
- 将 `src/win/include` 加入 `rk_core` 的 INTERFACE include 目录
- **无需移动文件或重命名**，因为 `JsonLite` 纯 C++17，无 Windows API 依赖

---

## 任务 B: C++ 编译器契约

### 深入调查结论

`RK_ENGINE_TARGETS` 列表（`CMakeLists.txt:92-98`）已定义 13 个 target。需要区分：

| 类别 | Target | 应用 flags？ | 原因 |
|:----:|--------|:-----------:|------|
| **生产** | `rk3288_cli`, `opencv_verify_cli`, `inference_bench_cli` | ✅ 是 | 无 GTest 依赖 |
| **生产** | `native-lib` | ✅ 是 | Android JNI 层，无 RTTI 需求 |
| **生产** | `win_local_service`, `win_camera_face_recognition` | ⚠️ 仅 `-fvisibility=hidden` | OpenCV 可能依赖异常 |
| **测试** | `core_unit_tests`, `core_gtest_tests` | ❌ 否 | GTest 需要 RTTI |
| **测试** | `face_infer_unit_tests`, `win_unit_tests`, `ncnn_precision_test` | ❌ 否 | GTest 需要 RTTI |
| **工具** | `win_face_eval_cli`, `win_face_bench_cli` | ⚠️ 可加 | 无 GTest |

### 方案

利用现有的 `RK_ENGINE_TARGETS` 列表 + 排除列表，避免逐 target 手写。

#### 修改 `cmake/core.cmake`（末尾追加）

```cmake
# ── C++ 编译器契约：生产 target 安全编译选项 ──
# 仅对非测试 target 应用 -fno-exceptions -fno-rtti -fvisibility=hidden
# 测试 target 通过 GTest 依赖 RTTI，排除在外
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(RK_SAFETY_EXCLUDED
        core_unit_tests core_gtest_tests
        face_infer_unit_tests win_unit_tests ncnn_precision_test
        win_face_database_perf
    )
    foreach(t ${RK_ENGINE_TARGETS})
        if(NOT ${t} IN_LIST RK_SAFETY_EXCLUDED AND TARGET ${t})
            target_compile_options(${t} PRIVATE
                -fno-exceptions -fno-rtti -fvisibility=hidden)
        endif()
    endforeach()
endif()
```

这样 Core Linux target（`rk3288_cli`、`opencv_verify_cli`、`inference_bench_cli`）获得全部 3 个 flag。
Windows target 的 MSVC 编译路径不受影响（`if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")` 排除 MSVC）。
测试 target 不受影响。

### 验证

```bash
# CI 验证编译通过
cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target core_unit_tests  # 测试仍 PASS
cmake --build build_ci --target rk3288_cli       # 生产 target 带 flags 编译
```

---

## 变更清单

| 任务 | 操作 | 文件 |
|:----:|------|------|
| A | **关闭任务** — 无需修改代码 | — |
| B | 追加 compiler contract 代码 | `cmake/core.cmake` |

**总计**: 0 新增文件，1 修改文件，2 分钟变更。
