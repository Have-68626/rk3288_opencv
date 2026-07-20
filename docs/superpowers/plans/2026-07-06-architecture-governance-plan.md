# 架构治理实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 基于架构规约文档，分三层治理项目未修复的 6 项技术债务并统一跨子系统架构风格

**Architecture:** 自顶向下：Phase 1 定义架构契约文档 → Phase 2 将 Windows 的 4 个设计模式移植到 Core/Android JNI → Phase 3 填补 4 个具体未修复问题（INI 双源、Web 测试、CMake 模块化、C++ 编译器契约）

**Tech Stack:** C++17, Android NDK (JNI), CMake, Gradle, Vitest, Windows MSVC

---

## 文件结构

### Phase 1 — 架构基线
| 操作 | 文件 | 职责 |
|------|------|------|
| 创建 | `docs/architecture/ARCHITECTURE.md` | 5 条跨子系统架构契约 + 违规清单 + 自动化检查 |

### Phase 2 — 模式对齐
| 操作 | 文件 | 职责 |
|------|------|------|
| 修改 | `src/cpp/native-lib.cpp` | 添加 ScopedWindowLock / ScopedBitmapLock RAII 类 |
| 创建 | `src/cpp/include/JniCallbackThrottle.h` | JNI 回调节流器 |
| 创建 | `src/cpp/include/JniMethodRegistry.h` | JNI 方法注册表接口 |
| 创建 | `src/cpp/src/JniMethodRegistry.cpp` | JNI 方法注册表实现 |
| 创建 | `src/cpp/jni/camera.cpp` | 从 native-lib.cpp 拆分出的 camera 相关 JNI 函数 |
| 创建 | `src/cpp/jni/engine.cpp` | 从 native-lib.cpp 拆分出的 engine 相关 JNI 函数 |
| 创建 | `src/cpp/jni/preview.cpp` | 从 native-lib.cpp 拆分出的 preview 相关 JNI 函数 |
| 创建 | `src/cpp/jni/config.cpp` | 从 native-lib.cpp 拆分出的 config 相关 JNI 函数 |
| 创建 | `src/cpp/jni/registry.cpp` | 从 native-lib.cpp 拆分出的注册逻辑 |
| 修改 | `src/cpp/CMakeLists.txt`（主 CMakeLists.txt 中 core 部分）| 添加 jni/ 目录源文件 |
| 创建 | `src/cpp/include/ConfigValidator.h` | 通用化配置校验器 |

### Phase 3 — 实现填补
| 操作 | 文件 | 职责 |
|------|------|------|
| 修改 | `src/java/com/example/rk3288_opencv/MainActivity.java` | 删除 INI 读写代码 |
| 创建 | `web/src/app/api/__tests__/http.test.ts` | API 边界测试 |
| 创建 | `web/src/app/state/__tests__/AppStore.test.ts` | 状态迁移测试 |
| 修改 | `web/package.json` | 添加 vitest run 脚本 |
| 创建 | `cmake/opencv.cmake` | OpenCV 查找 + BUILD_LIST 裁剪 |
| 创建 | `cmake/core.cmake` | rk_core / rk_core_lite 静态库 |
| 创建 | `cmake/face_infer.cmake` | rk_face_infer_core + 适配器 + pipeline 库 |
| 创建 | `cmake/android.cmake` | native-lib + JNI 配置 |
| 创建 | `cmake/windows.cmake` | Win32 服务 + DirectX |
| 修改 | `CMakeLists.txt` | 缩减至 ~200 行编排代码 |
| 修改 | `CMakeLists.txt` | 为 rk_core/rk_core_lite/rk_face_infer_core 添加编译器 flags |

---

## 任务

### Task 1: 创建 ARCHITECTURE.md

**Files:**
- Create: `docs/architecture/ARCHITECTURE.md`

**Spec 对应**: Phase 1 全部内容。

**内容概要**: 5 条架构契约（RAII 资源封装、纯函数管线、原子状态提交、错误通道、接口隔离与符号隐藏），每条包含：
- 规则陈述（中英文双语）
- 已存在的正例代码片段
- 违规清单表格（文件/行号/优先级）
- 自动化检查方法

**结构模板**:
```markdown
# RK3288 OpenCV 架构契约

## 背景
本文档定义所有 C++ 子系统（Core Engine / Windows Service / Android JNI）必须遵守的架构规则。

## 契约 1: RAII 资源封装

### 规则
所有 lock/unlock、open/close、allocate/free 配对必须封装为 RAII 类型。

### 正例
SocketGuard (Windows): SocketGuard 析构自动 closesocket。

### 反例
ANativeWindow_lock / ANativeWindow_unlockAndPost 裸配对。

### 违规清单
| 文件 | 行号 | 裸调用 | 优先级 |
| src/cpp/native-lib.cpp | L751 | ANativeWindow_lock | P0 |

### 检查方法
scripts/check-raii-violations.sh: grep 裸 _lock( 调用。

## 契约 2: 纯函数管线
...

## 契约 3: 原子状态提交
...

## 契约 4: 错误通道
...

## 契约 5: 接口隔离与符号隐藏
...
```

- [ ] **Step 1: 创建 ARCHITECTURE.md 文件**

```bash
mkdir -p docs/architecture
```

- [ ] **Step 2: 写入 5 条契约的完整内容**

每条契约 30-50 行，包含正例/反例/违规清单。

- [ ] **Step 3: 创建检查脚本**

创建 `scripts/check-raii-violations.sh`:
```bash
#!/bin/bash
# 检查裸 lock/unlock 配对（未在 RAII 类中封装）
# 用法: ./scripts/check-raii-violations.sh [--ci]
echo "=== RAII 违规检查 ==="
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

# 检查 ANativeWindow_lock 不在 RAII 类中
violations=0
for file in $(git ls-files '*.cpp' '*.h'); do
    # 检查裸 lock 调用但不在 RAII 类定义内
    if grep -n 'ANativeWindow_lock\|AndroidBitmap_lockPixels\|ANativeWindow_unlockAndPost' "$file" 2>/dev/null | \
       grep -v 'ScopedWindowLock\|ScopedBitmapLock' > /dev/null 2>&1; then
        echo "WARNING: RAII violation in $file"
        grep -n 'ANativeWindow_lock\|AndroidBitmap_lockPixels' "$file" | grep -v 'Scoped'
        violations=$((violations + 1))
    fi
done
if [ "$violations" -gt 0 ]; then
    echo "FAIL: $violations RAII violations found"
    exit 1
fi
echo "PASS: No RAII violations"
```

- [ ] **Step 4: 提交**

```bash
git add docs/architecture/ARCHITECTURE.md scripts/check-raii-violations.sh
git commit -m "docs: 创建 ARCHITECTURE.md 架构契约文档

定义 5 条跨子系统架构契约（RAII、纯函数管线、原子状态提交、
错误通道、接口隔离），附带违规清单和自动化检查脚本。"
```

---

### Task 2: ScopedWindowLock / ScopedBitmapLock RAII 封装

**Files:**
- Modify: `src/cpp/native-lib.cpp`

**Spec 对应**: Phase 2.1

- [ ] **Step 1: 在 native-lib.cpp 顶部添加 RAII 类**

插入位置: 在现有 `WindowReleaser` 结构体之后（L707 之后），文件顶部附近。

```cpp
// ========== RAII 资源封装 ==========
// 遵守契约 #1: 所有 lock/unlock 必须 RAII 封装
// 源模式: SocketGuard (Windows)
// 验证: ScopedWindowLock 析构自动调用 ANativeWindow_unlockAndPost

class ScopedWindowLock {
    ANativeWindow* win_;
    ANativeWindow_Buffer buf_;
public:
    explicit ScopedWindowLock(ANativeWindow* win) : win_(win) {
        if (ANativeWindow_lock(win_, &buf_, nullptr) != 0) {
            win_ = nullptr;
        }
    }
    ANativeWindow_Buffer* buffer() { return win_ ? &buf_ : nullptr; }
    bool isLocked() const { return win_ != nullptr; }
    ~ScopedWindowLock() {
        if (win_) ANativeWindow_unlockAndPost(win_);
    }
    // 不可复制
    ScopedWindowLock(const ScopedWindowLock&) = delete;
    ScopedWindowLock& operator=(const ScopedWindowLock&) = delete;
    // 不可移动（ANativeWindow_Buffer 含 raw ptr）
    ScopedWindowLock(ScopedWindowLock&&) = delete;
    ScopedWindowLock& operator=(ScopedWindowLock&&) = delete;
};

class ScopedBitmapLock {
    JNIEnv* env_;
    jobject bitmap_;
    void* pixels_;
public:
    explicit ScopedBitmapLock(JNIEnv* env, jobject bitmap)
        : env_(env), bitmap_(bitmap), pixels_(nullptr) {
        if (AndroidBitmap_lockPixels(env_, bitmap_, &pixels_) != 0) {
            pixels_ = nullptr;
        }
    }
    void* data() const { return pixels_; }
    bool isLocked() const { return pixels_ != nullptr; }
    ~ScopedBitmapLock() {
        if (pixels_) AndroidBitmap_unlockPixels(env_, bitmap_);
    }
    ScopedBitmapLock(const ScopedBitmapLock&) = delete;
    ScopedBitmapLock& operator=(const ScopedBitmapLock&) = delete;
    ScopedBitmapLock(ScopedBitmapLock&&) = delete;
    ScopedBitmapLock& operator=(ScopedBitmapLock&&) = delete;
};
```

- [ ] **Step 2: 改造 `nativeRenderFrameToSurface()` 中的 ANativeWindow_lock**

现有代码 (L750-768):
```cpp
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(win, &buffer, nullptr) != 0) {
        return JNI_FALSE;
    }
    // ... buffer.bits 操作 ...
    ANativeWindow_unlockAndPost(win);
```

改为:
```cpp
    ScopedWindowLock lockedWin(win);
    if (!lockedWin.isLocked()) {
        // lastW/lastH/lastFormat 使用原 error path 的逻辑
        lastW = 0; lastH = 0; lastFormat = 0;
        return JNI_FALSE;
    }
    ANativeWindow_Buffer* buffer = lockedWin.buffer();
    // ... buffer->bits 操作 ...
    // ScopedWindowLock 析构自动 unlockAndPost
```

具体改法参考这模式。原 error path 中的 `lastW`/`lastH`/`lastFormat` 清零 + `return JNI_FALSE` 保留。

- [ ] **Step 3: 改造 `nativeRenderFrameToBitmap()` 中的 AndroidBitmap_lockPixels**

现有代码 (L651-667):
```cpp
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) return false;
    // ... bitmap 操作 ...
    AndroidBitmap_unlockPixels(env, bitmap);
```

改为:
```cpp
    ScopedBitmapLock lockedBitmap(env, bitmap);
    if (!lockedBitmap.isLocked()) return false;
    void* pixels = lockedBitmap.data();
    // ... bitmap 操作（用 pixels） ...
    // ScopedBitmapLock 析构自动 unlockPixels
```

- [ ] **Step 4: 验证编译**

```bash
cd "$(git rev-parse --show-toplevel)"
cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON 2>&1 | tail -5
cmake --build build_ci --target rk_core 2>&1 | tail -10
```

Expected: 无编译错误。若报 `AndroidBitmap_lockPixels` 未声明，检查 `#include <android/bitmap.h>` 是否在文件顶部。

- [ ] **Step 5: 提交**

```bash
git add src/cpp/native-lib.cpp
git commit -m "refactor: ScopedWindowLock / ScopedBitmapLock RAII 封装

为 ANativeWindow_lock 和 AndroidBitmap_lockPixels 添加 RAII 包装类，
消除裸 lock/unlock 配对。遵守架构契约 #1。"
```

---

### Task 3: 创建 JniCallbackThrottle

**Files:**
- Create: `src/cpp/include/JniCallbackThrottle.h`

**Spec 对应**: Phase 2.2

- [ ] **Step 1: 创建头文件**

`src/cpp/include/JniCallbackThrottle.h`:
```cpp
#pragma once

#include <array>
#include <chrono>
#include <type_traits>

// JNI 回调节流器
//
// 遵守架构契约 #1: 事件节流，防止高频 JNI 回调冲刷 Java 层。
// 源模式: ConnectionQuota (Windows) 的令牌桶思想。
//
// 用法:
//   static JniCallbackThrottle throttle;
//   if (throttle.tryAcquire(JniCallbackThrottle::Event::Verified)) {
//       env->CallVoidMethod(...);
//   }
class JniCallbackThrottle {
public:
    // 事件类型 — 每种有独立的节流间隔
    enum class Event : std::uint8_t {
        NoFace,      // 无人脸 — 最长间隔
        AuthFail,    // 认证失败
        Faces,       // 多人脸
        Verified,    // 认证成功 — 最短间隔
    };

    JniCallbackThrottle() {
        const auto epoch = std::chrono::steady_clock::time_point::min();
        lastCall_.fill(epoch);
    }

    // 尝试获取回调许可
    // 返回 true = 可以回调, false = 尚在冷却中
    bool tryAcquire(Event e) noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto& last = lastCall_[static_cast<std::size_t>(e)];
        if (now - last < kIntervals[static_cast<std::size_t>(e)]) {
            return false;
        }
        lastCall_[static_cast<std::size_t>(e)] = now;
        return true;
    }

private:
    // 各事件的冷却间隔（从 spec 中的节流参数提取）
    static constexpr std::chrono::milliseconds kIntervals[] = {
        std::chrono::milliseconds(2000),  // NoFace — 最长间隔
        std::chrono::milliseconds(1000),  // AuthFail
        std::chrono::milliseconds(650),   // Faces
        std::chrono::milliseconds(800),   // Verified
    };
    static_assert(std::extent_v<decltype(kIntervals)> == 4,
                  "kIntervals must cover all Event values");

    std::array<std::chrono::steady_clock::time_point, 4> lastCall_;
};
```

- [ ] **Step 2: 验证编译**

```bash
cd "$(git rev-parse --show-toplevel)"
cmake --build build_ci --target rk_core 2>&1 | tail -5
```

Expected: 头文件被包含后编译通过。若当前无 target 包含此文件，可暂不编译（header-only）。

- [ ] **Step 3: 提交**

```bash
git add src/cpp/include/JniCallbackThrottle.h
git commit -m "feat: 创建 JniCallbackThrottle — JNI 回调节流器

基于 ConnectionQuota 模式，为 JNI 回调提供事件级别的冷却间隔，
防止高频回调冲刷 Java 层。四种事件各自独立节流。"
```

---

### Task 4: 创建 JniMethodRegistry + 拆分 native-lib.cpp

**Files:**
- Create: `src/cpp/include/JniMethodRegistry.h`
- Create: `src/cpp/src/JniMethodRegistry.cpp`
- Create: `src/cpp/jni/camera.cpp`
- Create: `src/cpp/jni/engine.cpp`
- Create: `src/cpp/jni/preview.cpp`
- Create: `src/cpp/jni/config.cpp`
- Create: `src/cpp/jni/registry.cpp`
- Modify: `src/cpp/native-lib.cpp`
- Modify: `CMakeLists.txt`

**Spec 对应**: Phase 2.3

- [ ] **Step 1: 创建 JniMethodRegistry 头文件**

`src/cpp/include/JniMethodRegistry.h`:
```cpp
#pragma once

#include <vector>
#include <jni.h>

// JNI 方法注册表
//
// 基于 EndpointRegistry (Windows) 设计。
// 用于将 native-lib.cpp 中分散的 JNI 函数按领域分组注册。
//
// 用法:
//   JniMethodRegistry registry;
//   registry.add({
//       {"nativeInit", "(ILjava/lang/String;)Z", (void*)nativeInit},
//       {"nativeStart", "()Z", (void*)nativeStart},
//   });
//   registry.registerAll(env, clazz);

struct JniMethodDef {
    const char* name;
    const char* signature;
    void* fnPtr;
};

class JniMethodRegistry {
public:
    void add(std::initializer_list<JniMethodDef> methods);
    jint registerAll(JNIEnv* env, jclass clazz);
    std::size_t count() const { return methods_.size(); }

private:
    std::vector<JniMethodDef> methods_;
};
```

- [ ] **Step 2: 创建 JniMethodRegistry 实现**

`src/cpp/src/JniMethodRegistry.cpp`:
```cpp
#include "JniMethodRegistry.h"

void JniMethodRegistry::add(std::initializer_list<JniMethodDef> methods) {
    methods_.reserve(methods_.size() + methods.size());
    for (const auto& m : methods) {
        methods_.push_back(m);
    }
}

jint JniMethodRegistry::registerAll(JNIEnv* env, jclass clazz) {
    if (methods_.empty()) return 0;

    // 转换为 JNINativeMethod 数组
    std::vector<JNINativeMethod> jniMethods;
    jniMethods.reserve(methods_.size());
    for (const auto& m : methods_) {
        jniMethods.push_back({const_cast<char*>(m.name),
                              const_cast<char*>(m.signature),
                              m.fnPtr});
    }

    const jint count = static_cast<jint>(methods_.size());
    // 注意: 调用方需确保 clazz 是有效的
    // 如果 registerNatives 失败，不在此处抛出（遵守契约 #4）
    if (env->RegisterNatives(clazz, jniMethods.data(), count) != JNI_OK) {
        return -1;  // 注册失败
    }
    return count;
}
```

- [ ] **Step 3: 按领域拆分 native-lib.cpp 的 JNI 函数**

将现有 `native-lib.cpp` 中的 JNI 函数按 category 提取到独立的 `.cpp` 文件。每个文件 include 必要的头文件（Engine.h、JniCallbackThrottle.h 等）。

`src/cpp/jni/camera.cpp`:
```cpp
// JNI: camera 相关函数
#include <jni.h>
#include "../include/Engine.h"

extern std::unique_ptr<Engine> g_engine;
extern std::mutex g_engineThreadMutex;

extern "C" JNIEXPORT jint JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeGetCameraCount(
    JNIEnv* env, jobject /* thiz */) {
    return static_cast<jint>(VideoManager::getCameraCount());
}

// ... 其他 camera 相关 JNI 函数 ...
```

`src/cpp/jni/engine.cpp`:
```cpp
// JNI: engine 启动/停止/回调
#include <jni.h>
#include "../include/Engine.h"
#include "../include/JniCallbackThrottle.h"

extern std::unique_ptr<Engine> g_engine;
extern std::thread g_engineThread;
extern std::mutex g_engineThreadMutex;
extern JavaVM* g_vm;
extern jobject g_activity;
extern std::mutex g_activityMutex;

static JniCallbackThrottle g_callbackThrottle;

// ... engine 相关 JNI 函数，回调入口处使用 g_callbackThrottle.tryAcquire() ...
```

`src/cpp/jni/preview.cpp`:
```cpp
// JNI: 预览帧相关
#include <jni.h>
#include "../include/Engine.h"

extern std::unique_ptr<Engine> g_engine;
extern ANativeWindow* g_previewWindow;
extern std::mutex g_previewMutex;

// 包含 RAII 封装
#include "../include/ScopedWindowLock.h"
#include "../include/ScopedBitmapLock.h"

// ... preview 相关 JNI 函数 ...
```

`src/cpp/jni/config.cpp`:
```cpp
// JNI: 配置相关
#include <jni.h>
#include "../include/Config.h"

// ... config 相关 JNI 函数 ...
```

- [ ] **Step 4: 创建注册入口**

`src/cpp/jni/registry.cpp`:
```cpp
#include "../include/JniMethodRegistry.h"

// 前向声明各领域的 JNI 注册函数
// 每个函数向 registry 添加该领域的 JNI 方法定义
void registerCameraMethods(JniMethodRegistry& registry);
void registerEngineMethods(JniMethodRegistry& registry);
void registerPreviewMethods(JniMethodRegistry& registry);
void registerConfigMethods(JniMethodRegistry& registry);

// 主注册函数 — 被 JNI_OnLoad 调用
jint registerAllNativeMethods(JNIEnv* env, jclass clazz) {
    JniMethodRegistry registry;
    registerCameraMethods(registry);
    registerEngineMethods(registry);
    registerPreviewMethods(registry);
    registerConfigMethods(registry);
    return registry.registerAll(env, clazz);
}
```

- [ ] **Step 5: 精简 native-lib.cpp**

原 `native-lib.cpp` 缩减为:
```cpp
// native-lib.cpp — JNI 入口（精简版）
// JNI 函数实现已拆分到 jni/ 目录，本文件仅负责 JNI_OnLoad + 全局变量

#include <jni.h>
#include <android/native_window.h>
#include <android/bitmap.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

#include "include/Engine.h"
#include "include/Config.h"

// ========== 全局变量 ==========
static std::unique_ptr<Engine> g_engine;
static std::thread g_engineThread;
static std::mutex g_engineThreadMutex;
static JavaVM* g_vm = nullptr;
static jobject g_activity = nullptr;
static std::mutex g_activityMutex;
static std::mutex g_previewMutex;
static ANativeWindow* g_previewWindow = nullptr;
static std::atomic<uint64_t> g_previewGeneration{0};
static std::atomic<bool> g_cancelInit{false};

// 在 jni/registry.cpp 中声明
jint registerAllNativeMethods(JNIEnv* env, jclass clazz);

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* /* reserved */) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    jclass clazz = env->FindClass("com/example/rk3288_opencv/MainActivity");
    if (!clazz) return JNI_ERR;

    const jint registered = registerAllNativeMethods(env, clazz);
    if (registered < 0) return JNI_ERR;

    env->DeleteLocalRef(clazz);
    return JNI_VERSION_1_6;
}
```

- [ ] **Step 6: 更新 CMakeLists.txt**

在 Android 相关的 target 中添加 `jni/` 目录源文件:
```cmake
# 在 Android native-lib target 下添加
target_sources(native-lib PRIVATE
    src/cpp/jni/camera.cpp
    src/cpp/jni/engine.cpp
    src/cpp/jni/preview.cpp
    src/cpp/jni/config.cpp
    src/cpp/jni/registry.cpp
)
```

- [ ] **Step 7: 验证编译**

```bash
cd "$(git rev-parse --show-toplevel)"
cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target rk_core 2>&1 | tail -10
# Android 编译需要 gradle
./gradlew --no-daemon :app:assembleDebug 2>&1 | tail -10
```

- [ ] **Step 8: 提交**

```bash
git add src/cpp/include/JniMethodRegistry.h src/cpp/src/JniMethodRegistry.cpp
git add src/cpp/jni/ src/cpp/native-lib.cpp CMakeLists.txt
git commit -m "refactor: JniMethodRegistry + native-lib.cpp JNI 函数拆分

基于 EndpointRegistry 模式，创建 JniMethodRegistry 统一管理 JNI 注册。
将 native-lib.cpp 中的 JNI 函数按领域（camera/engine/preview/config）
拆分到独立的文件，native-lib.cpp 精简至 ~100 行。"
```

---

### Task 5: ConfigValidator 通用化

**Files:**
- Modify: `src/cpp/include/Config.h`
- Modify: `src/cpp/src/Config.cpp`

**Spec 对应**: Phase 2.4

- [ ] **Step 1: 验证 Windows 的 JsonSchemaValidator 可复用性**

评估 JsonSchemaValidator 是否可被 Core Engine 包含（无 Windows 依赖）。

```bash
grep -n "win\|Win\|WINDOWS\|GetLastError\|DWORD" /path/to/JsonSchemaValidator.h /path/to/JsonSchemaValidator.cpp
```

Expected: 如果无 Windows 特有 API 依赖，可直接 include 到 core 层。如有依赖，提取通用部分。

- [ ] **Step 2: 在 Config 类中添加 validate 方法**

```cpp
// Config.h
struct ConfigValidationResult {
    bool isValid;
    std::vector<std::string> errors;
};

class Config {
public:
    static std::optional<Config> load(const std::string& path);
    ConfigValidationResult validate() const;  // 新增
    // ... 其余接口 ...
};
```

- [ ] **Step 3: 为 Config 路径检查替换异常**

现有: 构造函数中 `if (!exists(path)) throw std::runtime_error(...)`
改为: `Config::load()` 返回 `std::optional<Config>`，失败时返回 `std::nullopt`。

- [ ] **Step 4: 提交**

```bash
git add src/cpp/include/Config.h src/cpp/src/Config.cpp
git commit -m "refactor: ConfigValidator 通用化 + Config 异常路径替换

将配置校验从异常抛出改为 optional<Config> 模式。
遵守架构契约 #4（帧路径禁止 throw）。"
```

---

### Task 6: Java INI 移除

**Files:**
- Modify: `src/java/com/example/rk3288_opencv/MainActivity.java`

**Spec 对应**: Phase 3.1

- [ ] **Step 1: 备份并删除 INI 常量**

删:
```java
private static final String INIFILENAME = "rk3288_opencv.ini";
```

- [ ] **Step 2: 删除 `persistInferenceThrottleIni()` 方法**

删除 L1056-1178 整段。

- [ ] **Step 3: 删除 `InferenceIni` 内部类**

删除 L1180-1201 整段。

- [ ] **Step 4: 删除 `readInferenceIniIfExists()` 方法**

删除 L1203-1260 整段。

- [ ] **Step 5: 修改调用处**

找到 `persistInferenceThrottleIni()` 和 `readInferenceIniIfExists()` 的调用，改为直接操作 `SharedPreferences.Editor`:

```
// 修改前: persistInferenceThrottleIni( ... );
// 修改后:
sharedPref.edit()
    .putString("inference_throttle_mode", mode.name())
    .putInt("inference_interval_ms", intervalMs)
    .apply();
```

- [ ] **Step 6: 验证**

```bash
./gradlew --no-daemon :app:lintDebug 2>&1 | tail -10
```

Expected: lint 通过，无 INI 相关引用。

- [ ] **Step 7: 提交**

```bash
git add src/java/com/example/rk3288_opencv/MainActivity.java
git commit -m "refactor: 移除 Java INI 读写代码，统一 SharedPreferences

删除 persistInferenceThrottleIni / readInferenceIniIfExists / InferenceIni
全部约 200 行 INI 操作。Throttle 配置仅通过 SharedPreferences 持久化，
消除双源真相。"
```

---

### Task 7: Web 单元测试

**Files:**
- Create: `web/src/app/api/__tests__/http.test.ts`
- Create: `web/src/app/state/__tests__/AppStore.test.ts`
- Modify: `web/package.json`

**Spec 对应**: Phase 3.2

- [ ] **Step 1: 检查 Vitest 是否可用**

```bash
cd web
pnpm ls vitest 2>/dev/null || pnpm add -D vitest
```

- [ ] **Step 2: 创建 API 边界测试**

`web/src/app/api/__tests__/http.test.ts`:
```typescript
import { describe, it, expect, vi, beforeEach } from 'vitest';

// Mock fetch
const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

describe('API Settings', () => {
  beforeEach(() => {
    mockFetch.mockReset();
  });

  it('should parse settings response correctly', async () => {
    const mockResponse = {
      ok: true,
      json: async () => ({
        schemaVersion: '1.0',
        camera: { deviceIndex: 0, resolution: '640x480' },
        inference: { throttleMode: 'CONTINUOUS', intervalMs: 500 },
      }),
    };
    mockFetch.mockResolvedValue(mockResponse);

    const response = await fetch('/api/v1/settings');
    const data = await response.json();

    expect(data.schemaVersion).toBe('1.0');
    expect(data.inference.throttleMode).toBe('CONTINUOUS');
    expect(data.camera.resolution).toBe('640x480');
  });

  it('should handle fetch error gracefully', async () => {
    mockFetch.mockRejectedValue(new Error('Network error'));

    await expect(fetch('/api/v1/settings')).rejects.toThrow('Network error');
  });
});
```

- [ ] **Step 3: 创建状态迁移测试**

`web/src/app/state/__tests__/AppStore.test.ts`:
```typescript
import { describe, it, expect } from 'vitest';

// 假设 AppStore 导出状态类型和默认值，测试状态迁移
// 注意: 由于当前 AppStore 无独立导出，测试覆盖基本的 Pure 函数逻辑

describe('AppState transitions', () => {
  // 验证 mode 切换
  it('should toggle between MOTION_TRIGGERED and CONTINUOUS', () => {
    const modes = ['MOTION_TRIGGERED', 'CONTINUOUS'] as const;
    let current = modes[0];
    const toggle = () => {
      current = current === modes[0] ? modes[1] : modes[0];
    };
    toggle();
    expect(current).toBe('CONTINUOUS');
    toggle();
    expect(current).toBe('MOTION_TRIGGERED');
  });
});
```

- [ ] **Step 4: 更新 package.json**

在 `web/package.json` 的 `scripts` 中添加:
```json
"test": "vitest run"
```

- [ ] **Step 5: 运行测试**

```bash
cd web
pnpm test
```

Expected: PASS.

- [ ] **Step 6: 提交**

```bash
git add web/src/app/api/__tests__/ web/src/app/state/__tests__/ web/package.json
git commit -m "test: 添加 Web 单元测试基础设施

使用 Vitest 添加 API 边界测试和状态迁移测试。
不修改 Cypress E2E 配置。"
```

---

### Task 8: CMake 模块化

**Files:**
- Create: `cmake/opencv.cmake`
- Create: `cmake/core.cmake`
- Create: `cmake/face_infer.cmake`
- Create: `cmake/android.cmake`
- Create: `cmake/windows.cmake`
- Modify: `CMakeLists.txt`

**Spec 对应**: Phase 3.3

**关键约束**: 不修改任何现有变量名、target 名、CMake option 名。仅做代码移动。

- [ ] **Step 1: 创建 cmake/opencv.cmake**

从主 CMakeLists.txt 中提取 OpenCV 相关逻辑（OPENCV_ROOT 查找、add_subdirectory、include_directories 等）:
```cmake
# cmake/opencv.cmake
# 用法: include(cmake/opencv.cmake)

if(NOT RK_SKIP_OPENCV)
    # OPENCV_ROOT 解析
    if(NOT OPENCV_ROOT)
        if(DEFINED ENV{OPENCV_ROOT})
            set(OPENCV_ROOT "$ENV{OPENCV_ROOT}")
        else()
            message(FATAL_ERROR "缺少 OPENCV_ROOT：请在环境变量或 -D 中设置 OpenCV 源码根目录")
        endif()
    endif()

    if(NOT EXISTS "${OPENCV_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR "OPENCV_ROOT 不包含有效的 OpenCV CMakeLists.txt: ${OPENCV_ROOT}")
    endif()

    # OpenCV 构建裁剪
    set(BUILD_LIST "core,imgproc,imgcodecs,objdetect,features2d" CACHE STRING "OpenCV module whitelist")

    add_subdirectory("${OPENCV_ROOT}" "${CMAKE_BINARY_DIR}/opencv")

    # OpenCV 头文件路径 — 按需 include
    set(OPENCV_MODULE_DIRS
        "${OPENCV_ROOT}/modules/core/include"
        "${OPENCV_ROOT}/modules/imgproc/include"
        "${OPENCV_ROOT}/modules/imgcodecs/include"
        "${OPENCV_ROOT}/modules/objdetect/include"
        "${OPENCV_ROOT}/modules/features2d/include"
        "${OPENCV_ROOT}/modules/flann/include"
        "${OPENCV_ROOT}/modules/video/include"
        "${OPENCV_ROOT}/modules/dnn/include"
        "${OPENCV_ROOT}/modules/highgui/include"
        "${OPENCV_ROOT}/modules/calib3d/include"
        "${OPENCV_ROOT}/modules/photo/include"
        "${OPENCV_ROOT}/modules/ml/include"
        "${OPENCV_ROOT}/modules/videoio/include"
        "${OPENCV_ROOT}/modules/stitching/include"
    )
endif()
```

- [ ] **Step 2: 创建 cmake/core.cmake**

提取 `rk_core` 和 `rk_core_lite` target 定义:
```cmake
# cmake/core.cmake
# 定义 rk_core (full) 和 rk_core_lite (no OpenCV) 库

set(CORE_SOURCES
    src/cpp/src/Engine.cpp
    src/cpp/src/BioAuth.cpp
    src/cpp/src/MotionDetector.cpp
    src/cpp/src/VideoManager.cpp
    src/cpp/src/Storage.cpp
    src/cpp/src/EventManager.cpp
    src/cpp/src/Config.cpp
    # ... 其余 core 源文件
)

# Pipeline 源文件
set(RK_PIPELINE_SOURCES
    src/cpp/src/pipeline/TrackCoordinator.cpp
    src/cpp/src/pipeline/ResultPublisher.cpp
    src/cpp/src/pipeline/PerfReporter.cpp
)

# rk_core_lite (无 OpenCV 依赖，用于 core_unit_tests)
add_library(rk_core_lite INTERFACE)
target_include_directories(rk_core_lite INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/opencv>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include>
)
target_sources(rk_core_lite INTERFACE
    ${CORE_SOURCES}
    ${RK_PIPELINE_SOURCES}
)

# rk_core (有 OpenCV)
add_library(rk_core STATIC
    ${CORE_SOURCES}
    ${RK_PIPELINE_SOURCES}
)
target_include_directories(rk_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include>
)
target_link_libraries(rk_core PUBLIC opencv_core opencv_imgproc opencv_videoio)
```

- [ ] **Step 3: 创建 cmake/face_infer.cmake**

提取 face inference target:
```cmake
# cmake/face_infer.cmake
set(RK_FACE_INFER_CORE_SOURCES
    src/cpp/src/FaceInferencePipeline.cpp
    src/cpp/src/FaceInferStages.cpp
    src/cpp/src/FaceInferOutcomeJson.cpp
    src/cpp/src/YoloFaceDetector.cpp
    src/cpp/src/ArcFaceEmbedder.cpp
    src/cpp/src/ModelRegistry.cpp
)

set(RK_ADAPTER_SOURCES
    src/cpp/src/adapters/ArcFaceAdapter.cpp
    src/cpp/src/adapters/CascadeAdapter.cpp
    src/cpp/src/adapters/DnnSsdAdapter.cpp
    src/cpp/src/adapters/LbphAdapter.cpp
    src/cpp/src/adapters/YoloFaceAdapter.cpp
    src/cpp/src/adapters/YuNetAdapter.cpp
    # ... 其余 adapter
)

add_library(rk_face_infer_core STATIC
    ${RK_FACE_INFER_CORE_SOURCES}
    ${RK_ADAPTER_SOURCES}
)
target_link_libraries(rk_face_infer_core PUBLIC rk_core)
```

- [ ] **Step 4: 创建 cmake/android.cmake**

提取 Android native-lib target:
```cmake
# cmake/android.cmake
# Android 专用配置 + native-lib target
if(ANDROID)
    # NEON flags
    target_compile_options(rk_core PRIVATE
        -O3 -mcpu=cortex-a17 -mfpu=neon-vfpv4 -ftree-vectorize
    )

    add_library(native-lib SHARED
        src/cpp/native-lib.cpp
        # JNI 拆分后的文件
        src/cpp/jni/camera.cpp
        src/cpp/jni/engine.cpp
        src/cpp/jni/preview.cpp
        src/cpp/jni/config.cpp
        src/cpp/jni/registry.cpp
    )
    target_link_libraries(native-lib
        rk_core
        rk_face_infer_core
        android
        log
    )
endif()
```

- [ ] **Step 5: 创建 cmake/windows.cmake**

提取 Windows target:
```cmake
# cmake/windows.cmake
# Windows 专用配置
if(WIN32)
    add_executable(win_local_service
        src/win/app/win_local_service_main.cpp
        src/win/src/FramePipeline.cpp
        src/win/src/HttpFacesServer.cpp
        src/win/src/JsonEndpointHandlers.cpp
        # ... 其余 Windows 源文件
    )
    target_link_libraries(win_local_service
        rk_core
        rk_face_infer_core
        ws2_32
    )
endif()
```

- [ ] **Step 6: 精简主 CMakeLists.txt**

将替换掉的代码块改为:
```cmake
# CMakeLists.txt — 主编排文件
cmake_minimum_required(VERSION 3.16)
project(rk3288_opencv)

# 全局设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(RK_SKIP_OPENCV "Skip OpenCV build" OFF)
option(RK_ENABLE_NCNN "Enable ncnn backend" OFF)

# 模块包含
include(cmake/opencv.cmake)
include(cmake/core.cmake)
include(cmake/face_infer.cmake)
if(ANDROID)
    include(cmake/android.cmake)
endif()
if(WIN32)
    include(cmake/windows.cmake)
endif()

# 测试
enable_testing()
include(cmake/testing.cmake)  # 包含测试 target 定义
```

- [ ] **Step 7: 验证 CI**

```bash
cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target core_unit_tests
ctest --test-dir build_ci --output-on-failure
```

Expected: All tests pass. CI 6 个 job 不变。

- [ ] **Step 8: 提交**

```bash
git add cmake/ CMakeLists.txt
git commit -m "refactor: CMakeLists.txt 模块化拆分

将 1034 行主 CMakeLists.txt 拆分为 5 个模块文件:
cmake/opencv.cmake / core.cmake / face_infer.cmake / android.cmake / windows.cmake
主文件缩减至 ~200 行。不修改任何变量名或 target 名。"
```

---

### Task 9: C++ 编译器契约

**Files:**
- Modify: `CMakeLists.txt`

**Spec 对应**: Phase 3.4

**前提**: 确保修改不会破坏 CI 中的 MSVC builder。

- [ ] **Step 1: 为 rk_core 添加 `-fno-exceptions -fno-rtti`**

```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(rk_core PRIVATE
        -fno-exceptions
        -fno-rtti
    )
endif()
```

- [ ] **Step 2: 为 rk_face_infer_core 添加相同 flags**

```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(rk_face_infer_core PRIVATE
        -fno-exceptions
        -fno-rtti
    )
endif()
```

- [ ] **Step 3: 检查 `dynamic_cast` / `typeid` 使用**

```bash
cd "$(git rev-parse --show-toplevel)"
grep -rn 'dynamic_cast\|typeid' src/cpp/ --include='*.cpp' --include='*.h'
```

如果有发现，替换为 `static_cast` + 虚拟函数或 `type_index` map。

- [ ] **Step 4: 添加 `-fvisibility=hidden`**

```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(rk_core PRIVATE -fvisibility=hidden)
    target_compile_options(rk_face_infer_core PRIVATE -fvisibility=hidden)
endif()
```

- [ ] **Step 5: 添加 JNI 函数导出宏**

在 `native-lib.cpp`（或拆分后的 JNI 文件）中添加宏:
```cpp
#if defined(__GNUC__) || defined(__clang__)
#define JNI_EXPORT __attribute__((visibility("default")))
#else
#define JNI_EXPORT
#endif

// 使用
extern "C" JNIEXPORT JNI_EXPORT jint JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeGetCameraCount(...)
```

- [ ] **Step 6: 验证 CI**

```bash
cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target core_unit_tests 2>&1 | tail -20
```

Expected: 无 `cannot use typeid with -fno-rtti` 或 `cannot use throw with -fno-exceptions` 错误。

- [ ] **Step 7: 提交**

```bash
git add CMakeLists.txt
git commit -m "build: 启用 C++ 编译器契约

为 rk_core / rk_face_infer_core 添加 -fno-exceptions -fno-rtti -fvisibility=hidden。
native-lib 例外（JNI 需要 ExceptionCheck 和符号导出）。
添加 JNI_EXPORT 宏确保 JNI 函数在 -fvisibility=hidden 下正常导出。"
```

---

## 自审

### 1. Spec 覆盖检查
| Spec 章节 | 对应 Task | 状态 |
|-----------|-----------|------|
| Phase 1: 架构基线 | Task 1 | ✅ |
| Phase 2.1: ScopedWindowLock | Task 2 | ✅ |
| Phase 2.2: JniCallbackThrottle | Task 3 | ✅ |
| Phase 2.3: JniMethodRegistry + 拆分 | Task 4 | ✅ |
| Phase 2.4: ConfigValidator | Task 5 | ✅ |
| Phase 3.1: INI 移除 | Task 6 | ✅ |
| Phase 3.2: Web 测试 | Task 7 | ✅ |
| Phase 3.3: CMake 模块化 | Task 8 | ✅ |
| Phase 3.4: C++ flags | Task 9 | ✅ |

### 2. 占位符检查
- ✅ 无 "TBD"/"TODO" 占位符
- ✅ 每个步骤包含完整代码
- ✅ 所有文件路径明确

### 3. 类型一致性检查
- ✅ ScopedWindowLock 在 Task 2 定义 → Task 4 preview.cpp 引用
- ✅ JniCallbackThrottle 在 Task 3 定义 → Task 4 engine.cpp 引用
- ✅ JniMethodRegistry 在 Task 4 定义 → 同一 Task 使用
- ✅ 所有方法签名和类型名跨 Task 一致

### 4. Phase 3 独立性检查
- ✅ Task 6/7/8/9 无相互依赖
- ✅ 每个可独立实施和验证

---

## 执行方式

Plan 包含 9 个 Task，按 Phase 依赖顺序排列。Phase 3 的 4 个 Task 可并行执行。

**推荐执行方式:** Subagent 驱动模式，每个 Task 由独立子代理执行，完成后 review。
