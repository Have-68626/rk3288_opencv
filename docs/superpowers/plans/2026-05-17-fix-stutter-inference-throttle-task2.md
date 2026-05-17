# 推理节流 Task2 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Android/native Engine 侧增加 `inferenceThrottleMode(auto/manual/off)` + `inferenceIntervalMs` 的运行时更新能力，并在 interval 未到时跳过 `verifyMulti` 但保持 `renderFrame` 更新（且不触发任何识别相关回调）。

**Architecture:** 在 `Engine::processFrame()` 内、`bioAuth->verifyMulti()` 之前做节流判断；节流参数使用原子变量存储，通过 JNI（`NativeBridge.nativeSetInferenceThrottle(String,int)`）在运行时更新；interval 未到时仅更新 renderFrame 并提前返回。

**Tech Stack:** C++17（std::atomic/std::string）、JNI（Android）、OpenCV（既有）。

---

## 变更文件清单

**Modify**
- `src/cpp/include/Engine.h`
- `src/cpp/src/Engine.cpp`
- `src/java/com/example/rk3288_opencv/NativeBridge.java`
- `src/cpp/native-lib.cpp`
- `.trae/specs/fix-stutter-inference-throttle/tasks.md`

**Add**
- `tests/cpp/test_inference_throttle.cpp`

**Modify (tests main)**
- `tests/cpp/core_unit_tests_main.cpp`

---

### Task 1：为 Engine 增加节流配置（原子）与更新接口

**Files:**
- Modify: `src/cpp/include/Engine.h`

- [ ] **Step 1: 在 Engine.h 定义节流模式与原子字段**

在 `#include <atomic>` 之后，补充 `#include <string>`（若已存在则跳过），并在 `class Engine` 的 `public` 区域加入类型与接口，在 `private` 区域加入原子字段（放在 flip/运行状态原子附近即可）。

```cpp
enum class InferenceThrottleMode {
    Off = 0,
    Manual = 1,
    Auto = 2,
};

static constexpr int kInferenceIntervalDefaultMs = 150;
static constexpr int kInferenceIntervalMinMs = 80;
static constexpr int kInferenceIntervalMaxMs = 500;

static InferenceThrottleMode parseInferenceThrottleMode(const std::string& s);
static int clampInferenceIntervalMs(int v);

void updateInferenceThrottle(const std::string& mode, int intervalMs);
InferenceThrottleMode getInferenceThrottleMode() const;
int getInferenceIntervalMs() const;
```

并在 `private:` 增加：

```cpp
std::atomic<InferenceThrottleMode> inferenceThrottleMode{InferenceThrottleMode::Off};
std::atomic<int> inferenceIntervalMs{kInferenceIntervalDefaultMs};
std::atomic<long long> lastInferenceStartMs{0};
```

- [ ] **Step 2: 本地编译检查（仅 C++ 语法）**

运行（Windows 环境）：

```powershell
cmake --build .\build --config Release
```

预期：编译通过；如本仓库未配置 `build/`，先跳到 Task 6 用 Gradle 验证 Android 构建。

---

### Task 2：在 Engine::processFrame 增加节流判断（interval 未到：仅更新 renderFrame）

**Files:**
- Modify: `src/cpp/src/Engine.cpp`

- [ ] **Step 1: 在 Engine.cpp 实现 parse/clamp/update/get**

在 `Engine.cpp` 中（通常放在 `Engine` 方法实现区域靠前位置），实现：

```cpp
InferenceThrottleMode Engine::parseInferenceThrottleMode(const std::string& s) {
    std::string v;
    v.reserve(s.size());
    for (char c : s) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (v == "auto") return InferenceThrottleMode::Auto;
    if (v == "manual") return InferenceThrottleMode::Manual;
    return InferenceThrottleMode::Off;
}

int Engine::clampInferenceIntervalMs(int v) {
    if (v < kInferenceIntervalMinMs) return kInferenceIntervalMinMs;
    if (v > kInferenceIntervalMaxMs) return kInferenceIntervalMaxMs;
    return v;
}

void Engine::updateInferenceThrottle(const std::string& mode, int intervalMs) {
    inferenceThrottleMode.store(parseInferenceThrottleMode(mode));
    inferenceIntervalMs.store(clampInferenceIntervalMs(intervalMs));
}

InferenceThrottleMode Engine::getInferenceThrottleMode() const {
    return inferenceThrottleMode.load();
}

int Engine::getInferenceIntervalMs() const {
    return inferenceIntervalMs.load();
}
```

- [ ] **Step 2: 在 verifyMulti 之前插入节流判断并实现“跳过推理仅渲染”**

在 `Engine::processFrame()` 内，保持当前逻辑结构不重排（先 resize/flip，再 MOTION_TRIGGERED 判断），在进入：

```cpp
std::vector<BioAuth::FaceAuthResult> results;
bool faceDetected = bioAuth->verifyMulti(frame, results, 4);
```

之前插入：

```cpp
const auto mode = inferenceThrottleMode.load();
const int interval = inferenceIntervalMs.load();
const long long now = nowMs();
const long long last = lastInferenceStartMs.load();

const bool throttleEnabled = (mode != InferenceThrottleMode::Off);
if (throttleEnabled && last > 0 && (now - last) < static_cast<long long>(interval)) {
    std::lock_guard<std::mutex> lock(renderMutex);
    frame.copyTo(renderFrame);
    renderFrameSeq++;
    stats.inferMs = 0.0;
    stats.postMs = 0.0;
    return;
}

if (throttleEnabled) {
    lastInferenceStartMs.store(now);
}
```

然后保留原有 `verifyMulti + track/回调 + renderFrame` 路径不变。

---

### Task 3：增加 NativeBridge JNI 接口（String mode + int interval）

**Files:**
- Modify: `src/java/com/example/rk3288_opencv/NativeBridge.java`

- [ ] **Step 1: 添加 nativeSetInferenceThrottle 声明**

在 `NativeBridge` 内新增：

```java
static native void nativeSetInferenceThrottle(String mode, int intervalMs);
```

---

### Task 4：JNI 接线到 Engine::updateInferenceThrottle

**Files:**
- Modify: `src/cpp/native-lib.cpp`

- [ ] **Step 1: 实现 JNI 函数并调用 g_engine->updateInferenceThrottle**

在 `native-lib.cpp` 中添加：

```cpp
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_NativeBridge_nativeSetInferenceThrottle(
        JNIEnv* env,
        jclass,
        jstring mode,
        jint intervalMs) {
    if (!g_engine) return;
    const char* m = mode ? env->GetStringUTFChars(mode, nullptr) : nullptr;
    std::string mStr = m ? m : "off";
    if (m) env->ReleaseStringUTFChars(mode, m);
    g_engine->updateInferenceThrottle(mStr, static_cast<int>(intervalMs));
}
```

---

### Task 5：补充最小单测（仅覆盖 clamp/parse/节流判定关键点）

**Files:**
- Add: `tests/cpp/test_inference_throttle.cpp`
- Modify: `tests/cpp/core_unit_tests_main.cpp`
- Test: `tests/cpp/core_unit_tests_main.cpp`（运行 core 单测）

- [ ] **Step 1: 新增 test_inference_throttle.cpp**

创建文件内容：

```cpp
#include "Engine.h"

bool test_inference_throttle_parse_and_clamp() {
    if (Engine::parseInferenceThrottleMode("off") != InferenceThrottleMode::Off) return false;
    if (Engine::parseInferenceThrottleMode("AUTO") != InferenceThrottleMode::Auto) return false;
    if (Engine::parseInferenceThrottleMode("manual") != InferenceThrottleMode::Manual) return false;
    if (Engine::parseInferenceThrottleMode("x") != InferenceThrottleMode::Off) return false;

    if (Engine::clampInferenceIntervalMs(0) != Engine::kInferenceIntervalMinMs) return false;
    if (Engine::clampInferenceIntervalMs(9999) != Engine::kInferenceIntervalMaxMs) return false;
    if (Engine::clampInferenceIntervalMs(150) != 150) return false;
    return true;
}
```

- [ ] **Step 2: 将测试挂到 core_unit_tests_main.cpp**

在 `core_unit_tests_main.cpp` 的声明区域新增：

```cpp
bool test_inference_throttle_parse_and_clamp();
```

并在 `cases[]` 中加入：

```cpp
{"inference_throttle_parse_and_clamp", test_inference_throttle_parse_and_clamp},
```

- [ ] **Step 3: 运行 core 单测**

运行：

```powershell
cmake --build .\build --config Release
.\build\Release\core_unit_tests.exe
```

预期：`TEST_SUMMARY ... fail=0`。

---

### Task 6：Android 构建验证（符合仓库提交前规则）

**Files:**
- Test: Gradle 任务

- [ ] **Step 1: 运行 Debug 构建 + 单测 + Lint**

在仓库根目录运行：

```powershell
.\gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

预期：全部任务成功；如失败，优先修复编译/签名/NDK 相关报错后再继续。

---

### Task 7：勾选 Task2 完成状态

**Files:**
- Modify: `.trae/specs/fix-stutter-inference-throttle/tasks.md`

- [ ] **Step 1: 勾选 Task2 及其子项**

将：

```md
- [ ] Task 2: Native 侧节流实现点设计与接线
  - [ ] ...
```

更新为：

```md
- [x] Task 2: Native 侧节流实现点设计与接线
  - [x] ...
```

