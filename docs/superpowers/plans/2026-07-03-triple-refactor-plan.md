# 三层重构实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 FramePipeline、HttpFacesServer、Engine 三个模块从"大而全执行类"重构为"薄协调器 + 可替换阶段"，在不改变外部接口的前提下实现局部变化只影响局部对象、每个阶段独立可测。

**Architecture:** 每个模块遵循相同模式——抽出独立对象（装配/会话/处理/副作用），原类变为薄协调器。Engine 采用"值流管线"（3C），将 317 行的 processFrame 替换为类型安全的数据流链：FramePacket → PreResult → InferResult → TrackResult → FrameOutcome，副作用延迟到 ResultPublisher。

**Tech Stack:** C++17, OpenCV 4.x, CMake 3.22+, 自定义 bool 函数测试框架

---

## Phase 1: FramePipeline 拆分（文件 1-10）

### Task 1: RuntimeBootstrap — 装配对象抽取

**Files:**
- Create: `src/win/include/rk_win/RuntimeBootstrap.h`
- Create: `src/win/src/RuntimeBootstrap.cpp`
- Create: `tests/win/test_runtime_bootstrap.cpp`
- Modify: `tests/win/win_unit_tests_main.cpp`

**Context:** FramePipeline::initialize() (行 53-180) 串起设备枚举、日志初始化、识别器工厂、DNN 初始化、模型快照和后台线程启动。第一步只抽取纯装配逻辑（构造识别器 + DNN + 模型快照），不涉及线程启动、不写 render_.status。

- [ ] **Step 1: 定义 BootstrapResult 和 RuntimeBootstrap 头文件**

```cpp
// src/win/include/rk_win/RuntimeBootstrap.h
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "rk_win/AppConfig.h"

namespace rk_win {

struct ModelSnapshot {
    std::string id;
    std::string displayName;
    std::string taskType;
    std::string configuredPath;
    std::string resolvedPath;
    std::string backend;
    std::string hash;
    std::string status;       // "loaded" | "failed" | "missing" | "disabled"
    bool isInUse = false;
    std::string lastError;
};

struct BootstrapResult {
    std::unique_ptr<IRecognizer> recognizer;
    std::unique_ptr<DnnSsdFaceDetector> detector;
    std::vector<ModelSnapshot> models;
    std::string warning;  // 非致命警告（如日志打开失败）
    bool ok = false;      // false = 关键组件初始化失败
};

class RuntimeBootstrap {
public:
    // 纯装配：只构造对象，不启动线程，不写日志
    static BootstrapResult build(const AppConfig& cfg);
};

}  // namespace rk_win
```

- [ ] **Step 2: 实现 RuntimeBootstrap::build()**

```cpp
// src/win/src/RuntimeBootstrap.cpp
#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/FaceRecognizer.h"
#include "rk_win/ArcFaceWinRecognizer.h"
#include "rk_win/DnnSsdFaceDetector.h"
#include "rk_win/crypto_utils.h"  // calculateSHA256

namespace rk_win {

BootstrapResult RuntimeBootstrap::build(const AppConfig& cfg) {
    BootstrapResult result;
    result.ok = true;

    // 1. 识别器
    const std::string cascadeUtf8 = cfg.recognition.cascadePath.string();
    bool cascadeOk = false;

    if (cfg.model.recognition == "arcface") {
        auto arcRec = std::make_unique<ArcFaceWinRecognizer>();
        cascadeOk = arcRec->initialize(cascadeUtf8, cfg.recognition.databasePath,
            cfg.recognition.arcFaceModelPath.string(),
            cfg.recognition.minFaceSizePx, cfg.recognition.identifyThreshold);
        result.recognizer = std::move(arcRec);
    } else {
        auto lbpRec = std::make_unique<FaceRecognizer>();
        cascadeOk = lbpRec->initialize(cascadeUtf8, cfg.recognition.databasePath,
            cfg.recognition.minFaceSizePx, cfg.recognition.identifyThreshold);
        result.recognizer = std::move(lbpRec);
    }
    if (!cascadeOk) {
        result.warning = "识别模块初始化失败（请检查 cascade_path 与 database_path）";
        result.ok = false;
    }

    // 2. Cascade 模型快照
    {
        ModelSnapshot m;
        m.id = "cascade_frontalface";
        m.displayName = "Cascade Frontal Face (LBP)";
        m.taskType = "detect_recognize_pipeline";
        m.configuredPath = cascadeUtf8;
        m.resolvedPath = cascadeUtf8;
        m.backend = cfg.model.recognition == "arcface" ? "opencv_dnn_arcface" : "opencv_cascade";
        if (cfg.model.recognition == "arcface") {
            m.hash = rk_wcfr::calculateSHA256(cfg.recognition.arcFaceModelPath.string());
        } else {
            m.hash = rk_wcfr::calculateSHA256(cfg.recognition.cascadePath);
        }
        m.status = cascadeOk ? "loaded" : "failed";
        m.isInUse = !cfg.dnn.enable;
        if (!cascadeOk) m.lastError = "初始化失败 (cascade_path 或 database_path 有误)";
        result.models.push_back(std::move(m));
    }

    // 3. DNN 检测器
    result.detector = std::make_unique<DnnSsdFaceDetector>();
    if (cfg.dnn.enable) {
        DnnSsdConfig dc;
        dc.modelPath = cfg.dnn.modelPath;
        dc.configPath = cfg.dnn.configPath;
        dc.inputWidth = cfg.dnn.inputWidth;
        dc.inputHeight = cfg.dnn.inputHeight;
        dc.scale = cfg.dnn.scale;
        dc.meanB = cfg.dnn.meanB;
        dc.meanG = cfg.dnn.meanG;
        dc.meanR = cfg.dnn.meanR;
        dc.swapRB = cfg.dnn.swapRB;
        dc.confThreshold = cfg.dnn.confThreshold;
        dc.backend = cfg.dnn.backend;
        dc.target = cfg.dnn.target;

        std::string err;
        bool dnnOk = result.detector->initialize(dc, err);

        ModelSnapshot m;
        m.id = "dnn_face_detector";
        m.displayName = "OpenCV DNN Face Detector";
        m.taskType = "detect";
        m.configuredPath = cfg.dnn.modelPath.string();
        m.resolvedPath = cfg.dnn.modelPath.string();
        m.backend = "opencv_dnn";
        m.hash = rk_wcfr::calculateSHA256(cfg.dnn.modelPath);
        m.status = dnnOk ? "loaded"
            : (std::filesystem::exists(cfg.dnn.modelPath) ? "failed" : "missing");
        m.isInUse = true;
        m.lastError = err;
        result.models.push_back(std::move(m));

        if (!dnnOk) {
            result.warning += "; DNN 检测初始化失败: " + err;
            // DNN 失败不阻断整个 bootstrap（保留 cascade 降级）
        }
    } else {
        ModelSnapshot m;
        m.id = "dnn_face_detector";
        m.displayName = "OpenCV DNN Face Detector";
        m.taskType = "detect";
        m.configuredPath = cfg.dnn.modelPath.string();
        m.resolvedPath = cfg.dnn.modelPath.string();
        m.backend = "opencv_dnn";
        m.hash = rk_wcfr::calculateSHA256(cfg.dnn.modelPath);
        m.status = "disabled";
        m.isInUse = false;
        result.models.push_back(std::move(m));
    }

    return result;
}

}  // namespace rk_win
```

- [ ] **Step 3: 编写测试 — 验证 bootstrap 正常路径**

```cpp
// tests/win/test_runtime_bootstrap.cpp
#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/AppConfig.h"
#include <cassert>

static bool test_bootstrap_returns_models_on_valid_config() {
    // 使用仓库中的测试配置
    AppConfig cfg;
    cfg.recognition.cascadePath = "config/cascade_frontalface_default.xml";
    cfg.recognition.databasePath = "tests/fixtures/";
    cfg.recognition.minFaceSizePx = 80;
    cfg.recognition.identifyThreshold = 60.0;
    cfg.model.recognition = "lbph";
    cfg.dnn.enable = false;

    auto result = rk_win::RuntimeBootstrap::build(cfg);
    assert(result.models.size() >= 2);                    // cascade + dnn(disabled)
    assert(result.models[0].id == "cascade_frontalface");
    assert(result.models[1].status == "disabled");
    assert(result.recognizer != nullptr);                  // LBPH 识别器已构造
    assert(result.detector != nullptr);                    // DNN 检测器已构造（未初始化）
    return true;
}

static bool test_bootstrap_reports_failure_on_bad_cascade() {
    AppConfig cfg;
    cfg.recognition.cascadePath = "nonexistent/path.xml";
    cfg.recognition.databasePath = "tests/fixtures/";
    cfg.recognition.minFaceSizePx = 80;
    cfg.recognition.identifyThreshold = 60.0;
    cfg.model.recognition = "lbph";
    cfg.dnn.enable = false;

    auto result = rk_win::RuntimeBootstrap::build(cfg);
    assert(result.ok == false);                            // 关键组件失败
    assert(!result.warning.empty());                       // 包含错误信息
    assert(result.models[0].status == "failed");           // cascade 标记为 failed
    return true;
}
```

- [ ] **Step 4: 注册测试**

在 `tests/win/win_unit_tests_main.cpp` 的 TestCase 表中添加：

```cpp
{"test_bootstrap_returns_models_on_valid_config", test_bootstrap_returns_models_on_valid_config},
{"test_bootstrap_reports_failure_on_bad_cascade", test_bootstrap_reports_failure_on_bad_cascade},
```

- [ ] **Step 5: 编译 + 运行测试**

```powershell
cmake --build build_win --config Release --target win_unit_tests
ctest --test-dir build_win -C Release -R bootstrap
```

- [ ] **Step 6: 提交**

```bash
git add src/win/include/rk_win/RuntimeBootstrap.h \
        src/win/src/RuntimeBootstrap.cpp \
        tests/win/test_runtime_bootstrap.cpp \
        tests/win/win_unit_tests_main.cpp
git commit -m "feat: 提取 RuntimeBootstrap 装配对象，解耦识别器/DNN构建"
```

---

### Task 2: FramePipeline 改用 RuntimeBootstrap

**Files:**
- Modify: `src/win/src/FramePipeline.cpp:53-180`
- Modify: `src/win/include/rk_win/FramePipeline.h`
- Modify: `src/win/CMakeLists.txt`

- [ ] **Step 1: 在 FramePipeline.h 中添加 BootstrapResult 成员**

```cpp
// src/win/include/rk_win/FramePipeline.h — 在 private 区域添加:
#include "rk_win/RuntimeBootstrap.h"
// ...
private:
    std::vector<ModelSnapshot> activeModels_;
    // 其他成员不变...
```

- [ ] **Step 2: 改写 initialize() — 委托给 RuntimeBootstrap**

将 `src/win/src/FramePipeline.cpp:53-180` 替换为：

```cpp
bool FramePipeline::initialize(const AppConfig& cfg) {
    cfg_ = cfg;
    devices_ = MfCamera::enumerateDevices();

    // 日志初始化
    if (!logger_.open(cfg_.log.logDir, cfg_.log.maxFileBytes, cfg_.log.maxRollFiles)) {
        render_.status = "日志打开失败（将继续运行但不会落盘日志）";
    }

    // 装配运行时（委托 RuntimeBootstrap）
    auto bootstrap = RuntimeBootstrap::build(cfg_);
    {
        std::lock_guard<std::mutex> lock(modelsMu_);
        activeModels_ = std::move(bootstrap.models);
    }
    recognizer_ = std::move(bootstrap.recognizer);
    dnn_ = std::move(bootstrap.detector);

    if (!bootstrap.ok) {
        render_.status = bootstrap.warning;
    }

    // Manifest check
    {
        std::lock_guard<std::mutex> lock(modelsMu_);
        for (const auto& m : activeModels_) {
            const char* match = "unknown";
            if (m.id == "cascade_frontalface") {
                match = (m.hash == "529f217132809f287aaed5cd35dc00d9bc9b2afebe46dd1fe90ecb67f1daad0d")
                    ? "match" : "mismatch";
            }
            std::fprintf(stderr, "MODEL_MANIFEST_CHECK [id=%s] hash=%s manifest_match=%s\n",
                m.id.c_str(), m.hash.c_str(), match);
        }
    }

    running_ = true;
    processThread_ = std::thread(&FramePipeline::processLoop, this);
    return true;
}
```

- [ ] **Step 3: 更新 CMakeLists.txt**

```cmake
# src/win/CMakeLists.txt — 在 src 列表中添加:
src/RuntimeBootstrap.cpp
```

- [ ] **Step 4: 编译验证**

```powershell
cmake --build build_win --config Release --target win_local_service
```

- [ ] **Step 5: 提交**

```bash
git add src/win/src/FramePipeline.cpp \
        src/win/include/rk_win/FramePipeline.h \
        src/win/CMakeLists.txt
git commit -m "refactor: FramePipeline::initialize 委托 RuntimeBootstrap"
```

---

### Task 3: CameraSession — 相机切换回滚

**Files:**
- Create: `src/win/include/rk_win/CameraSession.h`
- Create: `src/win/src/CameraSession.cpp`

**Context:** FramePipeline.cpp:312-377 在"首帧超时"和"总耗时超标"两条分支里重复执行回滚逻辑。CameraSession 统一入口。

- [ ] **Step 1: 定义 CameraSession 接口**

```cpp
// src/win/include/rk_win/CameraSession.h
#pragma once
#include <string>
#include <vector>
#include "rk_win/MfCamera.h"

namespace rk_win {

struct CameraResult {
    bool ok = false;
    std::string code;       // "ok" | "open_failed" | "first_frame_timeout" | "total_timeout"
    std::string message;
    MfCamera::DeviceInfo device;  // 回滚到的设备
};

struct CameraOpenParams {
    int deviceIndex = 0;
    int width = 640;
    int height = 480;
    int fps = 30;
    int firstFrameTimeoutMs = 5000;
    int totalCollectTimeoutMs = 30000;
};

class CameraSession {
public:
    // 切换摄像头，失败自动回滚到 prevDevice
    // prevDevice.index < 0 表示首次打开（无需回滚）
    static CameraResult switchWithRollback(
        const CameraOpenParams& params,
        const MfCamera::DeviceInfo& prevDevice);
};

}  // namespace rk_win
```

- [ ] **Step 2: 实现 switchWithRollback**

```cpp
// src/win/src/CameraSession.cpp
#include "rk_win/CameraSession.h"
#include "rk_win/MfCamera.h"
#include <chrono>
#include <thread>

namespace rk_win {

CameraResult CameraSession::switchWithRollback(
    const CameraOpenParams& params,
    const MfCamera::DeviceInfo& prevDevice)
{
    CameraResult result;
    auto devices = MfCamera::enumerateDevices();
    if (devices.empty()) {
        result.ok = false;
        result.code = "no_devices";
        result.message = "未检测到摄像头设备";
        return result;
    }

    if (params.deviceIndex < 0 || params.deviceIndex >= static_cast<int>(devices.size())) {
        result.ok = false;
        result.code = "invalid_device_index";
        result.message = "摄像头索引无效: " + std::to_string(params.deviceIndex);
        return result;
    }

    // 尝试打开新摄像头
    auto& target = devices[params.deviceIndex];
    auto openRes = MfCamera::open(target, params.width, params.height, params.fps);
    if (!openRes.ok) {
        // 打开失败 → 回滚
        if (prevDevice.index >= 0) {
            auto rollbackRes = MfCamera::open(prevDevice, params.width, params.height, params.fps);
            if (rollbackRes.ok) {
                result.device = prevDevice;
                result.ok = true;
                result.code = "rollback_ok";
                result.message = "新摄像头打开失败，已回滚到上一个设备: " + prevDevice.name;
                return result;
            }
        }
        result.ok = false;
        result.code = "open_failed";
        result.message = "摄像头打开失败且回滚失败: " + openRes.message;
        return result;
    }

    // 等待首帧
    auto t0 = std::chrono::steady_clock::now();
    bool gotFirstFrame = false;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < params.firstFrameTimeoutMs) {
        cv::Mat testFrame;
        if (MfCamera::grabFrame(testFrame)) {
            gotFirstFrame = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!gotFirstFrame) {
        // 首帧超时 → 回滚
        MfCamera::close();
        if (prevDevice.index >= 0) {
            auto rollbackRes = MfCamera::open(prevDevice, params.width, params.height, params.fps);
            if (rollbackRes.ok) {
                result.device = prevDevice;
                result.ok = true;
                result.code = "rollback_ok";
                result.message = "首帧超时(" + std::to_string(params.firstFrameTimeoutMs)
                    + "ms)，已回滚到上一个设备";
                return result;
            }
        }
        result.ok = false;
        result.code = "first_frame_timeout";
        result.message = "首帧超时且回滚失败";
        return result;
    }

    result.ok = true;
    result.code = "ok";
    result.device = target;
    return result;
}

}  // namespace rk_win
```

- [ ] **Step 3: 提交**

```bash
git add src/win/include/rk_win/CameraSession.h \
        src/win/src/CameraSession.cpp \
        src/win/CMakeLists.txt
git commit -m "feat: 提取 CameraSession::switchWithRollback 统一相机切换回滚"
```

---

### Task 4: FrameProcessor + SideEffectSink — 热路径拆分

**Files:**
- Create: `src/win/include/rk_win/FrameProcessor.h`
- Create: `src/win/src/FrameProcessor.cpp`
- Create: `src/win/include/rk_win/SideEffectSink.h`
- Create: `src/win/src/SideEffectSink.cpp`
- Modify: `src/win/src/FramePipeline.cpp:594-746` (processLoop)

**Context:** processLoop 同时处理背压、自适应 stride、清库、注册、检测/识别、绘制、结构化日志和 UI 状态发布。FrameProcessor 只做纯计算（取帧→推理→返回结果），SideEffectSink 统一收口副作用。

- [ ] **Step 1: 定义数据结构和接口**

```cpp
// src/win/include/rk_win/FrameProcessor.h
#pragma once
#include <opencv2/core.hpp>
#include <vector>
#include <string>
#include <functional>
#include "rk_win/DnnSsdFaceDetector.h"
#include "rk_win/IRecognizer.h"

namespace rk_win {

struct DetectMatch {
    cv::Rect bbox;
    float confidence = 0.0f;
    std::string personId;
    bool isIdentified = false;
};

struct ControlCommand {
    bool clearDb = false;
    bool enrollRequested = false;
    std::string enrollPersonId;
    int detectStride = 1;
    int frameCounter = 0;
};

struct FrameResult {
    cv::Mat drawFrame;               // 原始帧（可能已画 overlay）
    std::vector<DetectMatch> matches;
    ControlCommand consumedCommand;  // 已消费的命令（调用方用于日志）
    bool hasMatch = false;
};

class FrameProcessor {
public:
    FrameProcessor(DnnSsdFaceDetector* dnn, IRecognizer* recognizer);

    // 纯计算：取帧 → 推理 → 返回结果，不写日志不更新 UI
    FrameResult run(const cv::Mat& bgr, const ControlCommand& cmd);

private:
    DnnSsdFaceDetector* dnn_;
    IRecognizer* recognizer_;
};

}  // namespace rk_win
```

```cpp
// src/win/include/rk_win/SideEffectSink.h
#pragma once
#include <opencv2/core.hpp>
#include <functional>
#include <string>
#include "rk_win/FrameProcessor.h"
#include "rk_win/StructuredLogger.h"
#include "rk_win/RenderState.h"

namespace rk_win {

class SideEffectSink {
public:
    SideEffectSink(StructuredLogger* logger, RenderState* render);

    // 统一发布所有副作用
    void publish(const FrameResult& result);

    // 设置外部回调（HTTP facesSeq 通知等）
    using FacesCallback = std::function<void(const std::vector<DetectMatch>&)>;
    void setFacesCallback(FacesCallback cb) { facesCb_ = std::move(cb); }

private:
    void drawFacesOverlay(cv::Mat& draw, const std::vector<DetectMatch>& matches);
    void appendLog(const FrameResult& result);
    void notifyFaces(const std::vector<DetectMatch>& matches);

    StructuredLogger* logger_;
    RenderState* render_;
    FacesCallback facesCb_;
};

}  // namespace rk_win
```

- [ ] **Step 2: 实现 FrameProcessor::run()**

```cpp
// src/win/src/FrameProcessor.cpp
#include "rk_win/FrameProcessor.h"

namespace rk_win {

FrameProcessor::FrameProcessor(DnnSsdFaceDetector* dnn, IRecognizer* recognizer)
    : dnn_(dnn), recognizer_(recognizer) {}

FrameResult FrameProcessor::run(const cv::Mat& bgr, const ControlCommand& cmd) {
    FrameResult result;
    ControlCommand consumed = cmd;

    // 背压：stride 跳跃帧
    if (consumed.frameCounter > 0) {
        consumed.frameCounter--;
        if (consumed.frameCounter % consumed.detectStride != 0) {
            result.consumedCommand = consumed;
            return result;  // 跳过此帧
        }
    }

    result.drawFrame = bgr.clone();

    // 清库
    if (consumed.clearDb) {
        recognizer_->clearDb();
        consumed.clearDb = false;
    }

    // 注册
    if (consumed.enrollRequested) {
        int remaining = recognizer_->enrollFromFrame(bgr, consumed.enrollPersonId);
        consumed.enrollPersonId.clear();
        if (remaining == 0) {
            consumed.enrollRequested = false;
        }
        result.consumedCommand = consumed;
        return result;  // 注册帧不检测
    }

    // 检测
    std::vector<DnnSsdFaceDetector::Detection> detections;
    if (dnn_ && dnn_->isInitialized()) {
        detections = dnn_->detect(bgr);
    }

    // 识别
    result.matches.reserve(detections.size());
    for (const auto& det : detections) {
        DetectMatch dm;
        dm.bbox = det.bbox;
        dm.confidence = det.confidence;
        dm.isIdentified = recognizer_->identify(bgr(dm.bbox), dm.personId);
        result.matches.push_back(dm);
    }

    result.hasMatch = !result.matches.empty();
    result.consumedCommand = consumed;
    return result;
}

}  // namespace rk_win
```

- [ ] **Step 3: 实现 SideEffectSink::publish()**

```cpp
// src/win/src/SideEffectSink.cpp
#include "rk_win/SideEffectSink.h"

namespace rk_win {

SideEffectSink::SideEffectSink(StructuredLogger* logger, RenderState* render)
    : logger_(logger), render_(render) {}

void SideEffectSink::publish(const FrameResult& result) {
    // 1. 绘制 overlay
    cv::Mat draw = result.drawFrame.clone();
    if (result.hasMatch) {
        drawFacesOverlay(draw, result.matches);
    }

    // 2. 发布渲染帧
    {
        std::lock_guard<std::mutex> lk(render_->mu);
        render_->bgr = std::move(draw);
        render_->seq++;
    }

    // 3. 结构化日志
    if (logger_ && logger_->isOpen()) {
        appendLog(result);
    }

    // 4. faces 通知（供 HTTP SSE 等订阅）
    notifyFaces(result.matches);
}

void SideEffectSink::drawFacesOverlay(cv::Mat& draw,
    const std::vector<DetectMatch>& matches)
{
    for (const auto& m : matches) {
        cv::Scalar color = m.isIdentified ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
        cv::rectangle(draw, m.bbox, color, 2);
        if (!m.personId.empty()) {
            cv::putText(draw, m.personId,
                cv::Point(m.bbox.x, std::max(0, m.bbox.y - 8)),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 1);
        }
    }
}

void SideEffectSink::appendLog(const FrameResult& result) {
    StructuredLogEntry entry;
    entry.faceCount = static_cast<int>(result.matches.size());
    for (const auto& m : result.matches) {
        if (m.isIdentified) {
            entry.identifiedIds.push_back(m.personId);
        }
    }
    logger_->append(entry);
}

void SideEffectSink::notifyFaces(const std::vector<DetectMatch>& matches) {
    if (facesCb_) {
        facesCb_(matches);
    }
}

}  // namespace rk_win
```

- [ ] **Step 4: 提交**

```bash
git add src/win/include/rk_win/FrameProcessor.h \
        src/win/src/FrameProcessor.cpp \
        src/win/include/rk_win/SideEffectSink.h \
        src/win/src/SideEffectSink.cpp \
        src/win/CMakeLists.txt
git commit -m "feat: 提取 FrameProcessor + SideEffectSink 解耦热路径计算与副作用"
```

---

### Task 5: FramePipeline processLoop 改写

**Files:**
- Modify: `src/win/src/FramePipeline.cpp:594-746`
- Modify: `src/win/include/rk_win/FramePipeline.h`

- [ ] **Step 1: 在 FramePipeline 中添加新成员**

```cpp
// src/win/include/rk_win/FramePipeline.h — private 区域添加:
    std::unique_ptr<FrameProcessor> processor_;
    std::unique_ptr<SideEffectSink> sink_;
```

- [ ] **Step 2: 在 initialize() 末尾构造**

```cpp
// 在 FramePipeline::initialize() 中 processThread_ 启动前:
processor_ = std::make_unique<FrameProcessor>(dnn_.get(), recognizer_.get());
sink_ = std::make_unique<SideEffectSink>(&logger_, &render_);
```

- [ ] **Step 3: 改写 processLoop()**

将 `processLoop` 从 153 行缩减为 ~25 行：

```cpp
void FramePipeline::processLoop() {
    while (running_) {
        cv::Mat bgr;
        if (!capture_.read(bgr)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // 构建控制命令
        ControlCommand cmd;
        cmd.clearDb = clearDbRequested_.exchange(false);
        cmd.enrollRequested = enrollRequested_.load();
        if (cmd.enrollRequested) {
            cmd.enrollPersonId = enrollPersonId_;
        }
        cmd.detectStride = detectStride_;
        cmd.frameCounter = frameCounter_++;

        // 纯计算
        auto result = processor_->run(bgr, cmd);

        // 更新注册状态
        if (result.consumedCommand.enrollRequested == false && enrollRequested_.load()) {
            enrollRequested_ = false;
            // 注册完成的 personId 保持在上层
        }

        // 副作用收口
        sink_->publish(result);
    }
}
```

- [ ] **Step 4: 编译验证**

```powershell
cmake --build build_win --config Release --target win_local_service
```

- [ ] **Step 5: 提交**

```bash
git add src/win/src/FramePipeline.cpp \
        src/win/include/rk_win/FramePipeline.h
git commit -m "refactor: processLoop 委托 FrameProcessor + SideEffectSink"
```

---

### Task 6: ReloadPolicy — 热更新粒度细化

**Files:**
- Modify: `src/win/app/win_local_service_main.cpp:145-164`

**Context:** 当前模型或加速配置变化时直接整管线重启。引入 ReloadPolicy 后，模型路径变化只重建 RuntimeBootstrap，预览布局变化只更新 SideEffectSink，只有相机参数变化才走 CameraSession。

- [ ] **Step 1: 在 win_local_service_main.cpp 中添加 ReloadPolicy 辅助函数**

```cpp
// 在 win_local_service_main.cpp 中添加:
namespace {

enum class ChangeKind { None, Camera, Model, Preview, FullRestart };

ChangeKind classifyChange(const AppConfig& prev, const AppConfig& next) {
    if (prev.camera.deviceIndex != next.camera.deviceIndex ||
        prev.camera.width != next.camera.width ||
        prev.camera.height != next.camera.height ||
        prev.camera.fps != next.camera.fps) {
        return ChangeKind::Camera;
    }
    if (prev.model.recognition != next.model.recognition ||
        prev.dnn.modelPath != next.dnn.modelPath ||
        prev.dnn.enable != next.dnn.enable) {
        return ChangeKind::Model;
    }
    if (prev.ui.previewScaleMode != next.ui.previewScaleMode ||
        prev.ui.renderWidth != next.ui.renderWidth ||
        prev.ui.renderHeight != next.ui.renderHeight) {
        return ChangeKind::Preview;
    }
    return ChangeKind::None;
}

}  // namespace
```

- [ ] **Step 2: 替换整管线重启**

```cpp
// 将原有 shutdown → initialize → setPreviewLayout → ensureCameraRunning
// 替换为:

auto kind = classifyChange(prevCfg, nextCfg);
switch (kind) {
case ChangeKind::Camera:
    // 仅重建相机
    pipe.switchCamera(nextCfg);
    break;
case ChangeKind::Model:
    // 仅重建运行时模型
    pipe.reloadRuntime(nextCfg);
    break;
case ChangeKind::Preview:
    // 仅更新副作用发布
    pipe.updatePreviewLayout(nextCfg.ui.renderWidth, nextCfg.ui.renderHeight,
                             nextCfg.ui.previewScaleMode);
    break;
case ChangeKind::FullRestart:
case ChangeKind::None:
default:
    pipe.shutdown();
    pipe.initialize(nextCfg);
    pipe.setPreviewLayout(nextCfg.ui.renderWidth, nextCfg.ui.renderHeight,
                          nextCfg.ui.previewScaleMode);
    rk_win::ensureCameraRunning(pipe, nextCfg, events);
    break;
}
```

注意：`switchCamera` 和 `reloadRuntime` 需要在 FramePipeline 中新增对应的薄方法。

- [ ] **Step 3: FramePipeline 新增方法**

```cpp
// FramePipeline.h — 新增公开方法:
    void switchCamera(const AppConfig& cfg);
    void reloadRuntime(const AppConfig& cfg);
    void updatePreviewLayout(int w, int h, const std::string& scaleMode);

// FramePipeline.cpp — 实现:
void FramePipeline::switchCamera(const AppConfig& cfg) {
    auto result = CameraSession::switchWithRollback(
        CameraOpenParams{cfg.camera.deviceIndex, cfg.camera.width,
                         cfg.camera.height, cfg.camera.fps},
        currentDevice_);
    if (result.ok) {
        currentDevice_ = result.device;
    }
    render_.status = result.code == "ok" ? "摄像头已切换" : result.message;
}

void FramePipeline::reloadRuntime(const AppConfig& cfg) {
    auto bootstrap = RuntimeBootstrap::build(cfg);
    {
        std::lock_guard<std::mutex> lock(modelsMu_);
        activeModels_ = std::move(bootstrap.models);
    }
    recognizer_ = std::move(bootstrap.recognizer);
    dnn_ = std::move(bootstrap.detector);
    // 重建 FrameProcessor（引用了 dnn_ 和 recognizer_）
    processor_ = std::make_unique<FrameProcessor>(dnn_.get(), recognizer_.get());
    render_.status = bootstrap.ok ? "模型运行时已重载" : bootstrap.warning;
}

void FramePipeline::updatePreviewLayout(int w, int h, const std::string& scaleMode) {
    render_.previewWidth = w;
    render_.previewHeight = h;
    render_.previewScaleMode = scaleMode;
}
```

- [ ] **Step 4: 编译验证 + 提交**

```bash
git add src/win/app/win_local_service_main.cpp \
        src/win/src/FramePipeline.cpp \
        src/win/include/rk_win/FramePipeline.h
git commit -m "feat: ReloadPolicy 替代整管线重启，热更新局部化"
```

---

## Phase 2: HttpFacesServer 拆分（文件 11-16）

### Task 7: EndpointRegistry — 路由表

**Files:**
- Create: `src/win/include/rk_win/EndpointRegistry.h`
- Create: `src/win/src/EndpointRegistry.cpp`
- Create: `tests/win/test_endpoint_registry.cpp`

- [ ] **Step 1: 定义 EndpointRegistry**

```cpp
// src/win/include/rk_win/EndpointRegistry.h
#pragma once
#include <string>
#include <vector>
#include <functional>
#include "rk_win/HttpFacesServer.h"  // HttpResponse, HttpRequest

namespace rk_win {

struct EndpointContext {
    FramePipeline* pipe = nullptr;
    WinJsonConfigStore* settings = nullptr;
};

struct EndpointDef {
    const char* method;   // "GET", "POST", "PUT", "*" (any)
    const char* path;     // e.g. "/api/v1/models"
    std::function<HttpFacesServer::HttpResponse(
        const HttpFacesServer::HttpRequest&, EndpointContext&)> handler;
};

class EndpointRegistry {
public:
    void add(EndpointDef def);

    // 返回: 匹配 → handler 结果; 路径不匹配 → 404; 方法不匹配 → 405
    HttpFacesServer::HttpResponse dispatch(
        const HttpFacesServer::HttpRequest& req,
        EndpointContext& ctx) const;

private:
    std::vector<EndpointDef> routes_;
};

// 统一响应工厂
namespace ResponseFactory {
    HttpFacesServer::HttpResponse ok(const std::string& jsonBody = "");
    HttpFacesServer::HttpResponse err(int code, const std::string& msg,
        const std::string& details = "");
}

}  // namespace rk_win
```

- [ ] **Step 2: 实现 dispatch + ResponseFactory**

```cpp
// src/win/src/EndpointRegistry.cpp
#include "rk_win/EndpointRegistry.h"

namespace rk_win {

void EndpointRegistry::add(EndpointDef def) {
    routes_.push_back(std::move(def));
}

HttpFacesServer::HttpResponse EndpointRegistry::dispatch(
    const HttpFacesServer::HttpRequest& req,
    EndpointContext& ctx) const
{
    for (const auto& r : routes_) {
        if (req.path == r.path) {
            if (r.method[0] != '*' && req.method != r.method) {
                return ResponseFactory::err(405, "method_not_allowed",
                    "此端点仅支持 " + std::string(r.method));
            }
            return r.handler(req, ctx);
        }
    }
    return ResponseFactory::err(404, "not_found", "未知的 API 端点");
}

namespace ResponseFactory {

HttpFacesServer::HttpResponse ok(const std::string& jsonBody) {
    HttpFacesServer::HttpResponse r;
    r.status = 200;
    r.contentType = "application/json; charset=utf-8";
    r.body = jsonBody.empty() ? "{}" : jsonBody;
    // 统一安全头
    r.headers["X-Content-Type-Options"] = "nosniff";
    return r;
}

HttpFacesServer::HttpResponse err(int code, const std::string& msg,
    const std::string& details)
{
    HttpFacesServer::HttpResponse r;
    r.status = code;
    r.contentType = "application/json; charset=utf-8";
    r.body = "{\"error\":\"" + msg + "\"";
    if (!details.empty()) {
        r.body += ",\"details\":\"" + details + "\"";
    }
    r.body += "}";
    r.headers["X-Content-Type-Options"] = "nosniff";
    return r;
}

}  // namespace ResponseFactory
}  // namespace rk_win
```

- [ ] **Step 3: 测试**

```cpp
// tests/win/test_endpoint_registry.cpp
static bool test_registry_dispatch_returns_200_for_matching_route() {
    rk_win::EndpointRegistry reg;
    reg.add({"GET", "/test", [](auto&, auto&) {
        return rk_win::ResponseFactory::ok("{\"ok\":true}");
    }});
    rk_win::EndpointContext ctx;

    HttpFacesServer::HttpRequest req;
    req.method = "GET";
    req.path = "/test";
    auto resp = reg.dispatch(req, ctx);
    assert(resp.status == 200);
    assert(resp.body == "{\"ok\":true}");
    return true;
}

static bool test_registry_returns_405_for_method_mismatch() {
    rk_win::EndpointRegistry reg;
    reg.add({"POST", "/test", [](auto&, auto&) {
        return rk_win::ResponseFactory::ok();
    }});
    rk_win::EndpointContext ctx;

    HttpFacesServer::HttpRequest req;
    req.method = "GET";
    req.path = "/test";
    auto resp = reg.dispatch(req, ctx);
    assert(resp.status == 405);
    return true;
}
```

注册到 `win_unit_tests_main.cpp`。

- [ ] **Step 4: 提交**

```bash
git commit -m "feat: 引入 EndpointRegistry + ResponseFactory 统一路由分发"
```

---

### Task 8: JsonEndpointHandlers — 端点迁移

**Files:**
- Create: `src/win/include/rk_win/JsonEndpointHandlers.h`
- Create: `src/win/src/JsonEndpointHandlers.cpp`
- Modify: `src/win/src/HttpFacesServer.cpp` (handleApi 缩减)

- [ ] **Step 1: 定义 handler 函数**

```cpp
// src/win/include/rk_win/JsonEndpointHandlers.h
#pragma once
#include "rk_win/EndpointRegistry.h"

namespace rk_win {
namespace handlers {

// models
HttpResponse handleGetModels(const HttpRequest& req, EndpointContext& ctx);
HttpResponse handleReloadModel(const HttpRequest& req, EndpointContext& ctx);

// settings
HttpResponse handleGetSettings(const HttpRequest& req, EndpointContext& ctx);
HttpResponse handlePutSettings(const HttpRequest& req, EndpointContext& ctx);

// cameras
HttpResponse handleGetCameras(const HttpRequest& req, EndpointContext& ctx);
HttpResponse handleFlipCamera(const HttpRequest& req, EndpointContext& ctx);

// actions
HttpResponse handleEnroll(const HttpRequest& req, EndpointContext& ctx);
HttpResponse handleClearDb(const HttpRequest& req, EndpointContext& ctx);

// 填充路由表
void registerAll(EndpointRegistry& reg);

}  // namespace handlers
}  // namespace rk_win
```

- [ ] **Step 2: 实现关键 handler（handleGetModels 示例）**

```cpp
// src/win/src/JsonEndpointHandlers.cpp (精简示例)
#include "rk_win/JsonEndpointHandlers.h"
#include "rk_win/json_utils.h"  // parseRequiredField, buildModelsJson 等

namespace rk_win { namespace handlers {

HttpResponse handleGetModels(const HttpRequest& req, EndpointContext& ctx) {
    auto snapshots = ctx.pipe->getModelSnapshots();
    return ResponseFactory::ok(buildModelsJson(snapshots));
}

HttpResponse handlePutSettings(const HttpRequest& req, EndpointContext& ctx) {
    auto body = parseRequiredObject(req.body);
    if (body.type == JsonValue::Null) {
        return ResponseFactory::err(400, "invalid_json", "请求体必须是合法 JSON");
    }
    // 委托给 WinJsonConfigStore 的原有逻辑
    std::string err;
    if (!ctx.settings->updateFromJson(body, err)) {
        return ResponseFactory::err(400, "invalid_settings", err);
    }
    return ResponseFactory::ok();
}

// ...其余 handler 类似迁移

void registerAll(EndpointRegistry& reg) {
    reg.add({"GET",  "/api/v1/models",        handleGetModels});
    reg.add({"POST", "/api/v1/models/reload", handleReloadModel});
    reg.add({"GET",  "/api/v1/settings",      handleGetSettings});
    reg.add({"PUT",  "/api/v1/settings",      handlePutSettings});
    reg.add({"GET",  "/api/v1/cameras",       handleGetCameras});
    reg.add({"PUT",  "/api/v1/camera/flip",   handleFlipCamera});
    reg.add({"POST", "/api/v1/actions/enroll", handleEnroll});
    reg.add({"POST", "/api/v1/actions/db/clear", handleClearDb});
}

}}  // namespace
```

- [ ] **Step 3: handleApi 缩减为 ~10 行**

```cpp
HttpFacesServer::HttpResponse HttpFacesServer::handleApi(const HttpRequest& req) {
    EndpointContext ctx;
    ctx.pipe = pipe_;
    ctx.settings = settings_;
    return registry_.dispatch(req, ctx);
}
```

- [ ] **Step 4: 提交**

```bash
git commit -m "refactor: JsonEndpointHandlers 替代 handleApi 长分支链"
```

---

### Task 9: StreamSessionRunner — 流式端点统一

**Files:**
- Create: `src/win/include/rk_win/StreamSessionRunner.h`
- Create: `src/win/src/StreamSessionRunner.cpp`
- Modify: `src/win/src/HttpFacesServer.cpp:857-945`

- [ ] **Step 1: 定义 StreamSessionRunner**

```cpp
// src/win/include/rk_win/StreamSessionRunner.h
#pragma once
#include <cstdint>
#include <atomic>
#include <string>
#include <functional>

namespace rk_win {

enum class StreamType { Sse, Mjpeg };

class StreamSessionRunner {
public:
    using FrameProvider = std::function<bool(cv::Mat&)>;

    StreamSessionRunner(std::atomic<bool>& running, FrameProvider provider);

    // 执行流式会话（阻塞直到客户端断开或服务停止）
    void run(std::uintptr_t sock, StreamType type);

private:
    void runSse(std::uintptr_t sock);
    void runMjpeg(std::uintptr_t sock);

    std::atomic<bool>& running_;
    FrameProvider provider_;
};

}  // namespace rk_win
```

- [ ] **Step 2: 实现**

```cpp
// src/win/src/StreamSessionRunner.cpp
#include "rk_win/StreamSessionRunner.h"
#include <chrono>
#include <thread>
#include <vector>

namespace rk_win {

StreamSessionRunner::StreamSessionRunner(std::atomic<bool>& running, FrameProvider provider)
    : running_(running), provider_(std::move(provider)) {}

void StreamSessionRunner::run(std::uintptr_t sock, StreamType type) {
    switch (type) {
    case StreamType::Sse:  runSse(sock);  break;
    case StreamType::Mjpeg: runMjpeg(sock); break;
    }
}

void StreamSessionRunner::runSse(std::uintptr_t sock) {
    const SOCKET s = static_cast<SOCKET>(sock);
    const char* header = "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";
    ::send(s, header, static_cast<int>(std::strlen(header)), 0);

    int seq = 0;
    while (running_) {
        cv::Mat frame;
        if (!provider_(frame)) {
            // 空帧 → keepalive 注释
            char keepalive[] = ":keepalive\n\n";
            ::send(s, keepalive, static_cast<int>(sizeof(keepalive) - 1), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        // SSE data 块...
        // (复用现有 buildFacesJson 逻辑)
        seq++;
    }
}

void StreamSessionRunner::runMjpeg(std::uintptr_t sock) {
    // 复用现有 MJPEG 推流逻辑，但不再硬编码在 handleClient 中
    const SOCKET s = static_cast<SOCKET>(sock);
    const char* header = "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n";
    ::send(s, header, static_cast<int>(std::strlen(header)), 0);

    while (running_) {
        cv::Mat frame;
        if (!provider_(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        std::vector<uchar> jpeg;
        cv::imencode(".jpg", frame, jpeg);
        char boundary[] = "--frame\r\nContent-Type: image/jpeg\r\n"
            "Content-Length: ";
        ::send(s, boundary, static_cast<int>(sizeof(boundary) - 1), 0);
        std::string len = std::to_string(jpeg.size()) + "\r\n\r\n";
        ::send(s, len.c_str(), static_cast<int>(len.size()), 0);
        ::send(s, reinterpret_cast<const char*>(jpeg.data()),
              static_cast<int>(jpeg.size()), 0);
        ::send(s, "\r\n", 2, 0);
    }
}

}  // namespace rk_win
```

- [ ] **Step 3: handleClient 缩减为 ~20 行**

```cpp
void HttpFacesServer::handleClient(std::uintptr_t sock) {
    // ...解析请求（不变）
    if (req.path == "/api/v1/preview.mjpeg") {
        streamRunner_.run(sock, StreamType::Mjpeg);
        return;
    }
    if (req.path == "/api/faces/stream" || req.path == "/api/v1/faces/stream") {
        streamRunner_.run(sock, StreamType::Sse);
        return;
    }
    // 非流式：写响应
    auto resp = registry_.dispatch(req, ctx);
    writeResponse(sock, resp);
}
```

- [ ] **Step 4: 提交**

```bash
git commit -m "refactor: StreamSessionRunner 统一 SSE/MJPEG 流式端点"
```

---

## Phase 3: Engine 值流管线（文件 17-27）

### Task 10: TrackCoordinator — 零依赖纯函数

**Files:**
- Create: `src/cpp/include/pipeline/TrackCoordinator.h`
- Create: `src/cpp/src/pipeline/TrackCoordinator.cpp`
- Create: `tests/cpp/test_track_coordinator.cpp`
- Modify: `tests/cpp/core_unit_tests_main.cpp`

- [ ] **Step 1: 定义 TrackCoordinator 数据结构**

```cpp
// src/cpp/include/pipeline/TrackCoordinator.h
#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace pipeline {

struct DetectedFace {
    cv::Rect bbox;
    std::string identityId;
    float confidence = 0.0f;
    bool isAuthenticated = false;
};

struct TrackView {
    int trackId = 0;
    cv::Rect bbox;
    std::string stableId;
    float stableConfidence = 0.0f;
    long long lastSeenMs = 0;
};

struct TrackConfig {
    long long ttlMs = 1200;
    float matchIouThreshold = 0.3f;
    int stableFrames = 3;
};

class TrackCoordinator {
public:
    TrackCoordinator(const TrackConfig& cfg = TrackConfig{});

    // 纯函数：不修改全局状态，不依赖 OpenCV（仅 cv::Rect）
    // 输入：当前检测结果 + 时间戳
    // 输出：跟踪视图列表
    std::vector<TrackView> update(
        const std::vector<DetectedFace>& faces,
        long long timestampMs);

    // 状态检查（调试用）
    int trackCount() const { return static_cast<int>(tracks_.size()); }

private:
    static float iou(const cv::Rect& a, const cv::Rect& b);

    TrackConfig config_;
    std::vector<TrackView> tracks_;
    int nextTrackId_ = 1;
};

}  // namespace pipeline
```

- [ ] **Step 2: 实现纯函数 update()**

从 Engine.cpp:793-965 提取 IoU 匹配 + stable ID 逻辑，改写为无副作用的纯函数。关键改动：
- `faceTracks` → 函数内局部变量 `tracks_`（public getter 用于测试）
- `onResultCallback` → 移除（ResultPublisher 负责）
- `drawTracks` → 移除（FrameAnnotator 负责）
- `shouldRecognize` → 参数传入（不需要，identity 已含在 DetectedFace 中）

```cpp
// src/cpp/src/pipeline/TrackCoordinator.cpp (核心逻辑，~80行)
#include "pipeline/TrackCoordinator.h"
#include <algorithm>

namespace pipeline {

TrackCoordinator::TrackCoordinator(const TrackConfig& cfg) : config_(cfg) {}

float TrackCoordinator::iou(const cv::Rect& a, const cv::Rect& b) {
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.width, b.x + b.width);
    int y2 = std::min(a.y + a.height, b.y + b.height);
    if (x2 <= x1 || y2 <= y1) return 0.0f;
    float inter = static_cast<float>((x2 - x1) * (y2 - y1));
    float uni = static_cast<float>(a.area() + b.area()) - inter;
    return uni > 0 ? inter / uni : 0.0f;
}

std::vector<TrackView> TrackCoordinator::update(
    const std::vector<DetectedFace>& faces,
    long long timestampMs)
{
    // 1. TTL 清理
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
        [&](const TrackView& t) {
            return timestampMs - t.lastSeenMs > config_.ttlMs;
        }), tracks_.end());

    if (faces.empty()) return tracks_;

    // 2. 按面积排序（大脸优先匹配）
    std::vector<int> order(faces.size());
    for (int i = 0; i < static_cast<int>(faces.size()); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return faces[a].bbox.area() > faces[b].bbox.area();
    });

    // 3. IoU 匹配
    std::vector<bool> trackUsed(tracks_.size(), false);
    std::vector<int> detToTrack(faces.size(), -1);
    for (int idx : order) {
        float best = 0.0f;
        int bestTrack = -1;
        for (int ti = 0; ti < static_cast<int>(tracks_.size()); ti++) {
            if (trackUsed[ti]) continue;
            float s = iou(tracks_[ti].bbox, faces[idx].bbox);
            if (s > best) { best = s; bestTrack = ti; }
        }
        if (bestTrack >= 0 && best >= config_.matchIouThreshold) {
            trackUsed[bestTrack] = true;
            detToTrack[idx] = bestTrack;
        }
    }

    // 4. 新 track
    for (int i = 0; i < static_cast<int>(faces.size()); i++) {
        if (detToTrack[i] >= 0) continue;
        TrackView t;
        t.trackId = nextTrackId_++;
        t.bbox = faces[i].bbox;
        t.stableId = faces[i].isAuthenticated ? faces[i].identityId : "Unknown";
        t.stableConfidence = faces[i].confidence;
        t.lastSeenMs = timestampMs;
        tracks_.push_back(t);
        detToTrack[i] = static_cast<int>(tracks_.size()) - 1;
    }

    // 5. 更新已有 track
    for (int i = 0; i < static_cast<int>(faces.size()); i++) {
        int ti = detToTrack[i];
        if (ti < 0) continue;
        auto& t = tracks_[ti];
        t.bbox = faces[i].bbox;
        t.lastSeenMs = timestampMs;
        // 已认证的身份不被非认证覆盖
        if (faces[i].isAuthenticated &&
            faces[i].identityId == tracks_[ti].stableId) {
            // streak 逻辑简化：直接采纳
        } else if (faces[i].isAuthenticated) {
            t.stableId = faces[i].identityId;
            t.stableConfidence = faces[i].confidence;
        }
        // 非认证帧不覆盖已认证身份
    }

    return tracks_;
}

}  // namespace pipeline
```

- [ ] **Step 3: 编写纯函数测试（零 OpenCV 依赖）**

```cpp
// tests/cpp/test_track_coordinator.cpp
static bool test_track_coordinator_creates_new_track_for_first_face() {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(100, 100, 80, 80);
    f.isAuthenticated = false;
    faces.push_back(f);

    auto tracks = tc.update(faces, 1000);
    assert(tracks.size() == 1);
    assert(tracks[0].trackId == 1);
    assert(tracks[0].stableId == "Unknown");
    return true;
}

static bool test_track_coordinator_removes_stale_tracks() {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 10, 50, 50);
    f.isAuthenticated = false;
    faces.push_back(f);

    auto t1 = tc.update(faces, 1000);
    assert(t1.size() == 1);

    // 2 秒后无匹配 → TTL 清理
    auto t2 = tc.update({}, 3000);
    assert(t2.empty());
    return true;
}

static bool test_track_coordinator_iou_matches_consecutive_frames() {
    pipeline::TrackCoordinator tc;
    // Frame 1
    pipeline::DetectedFace f1;
    f1.bbox = cv::Rect(100, 100, 80, 80);
    f1.isAuthenticated = true;
    f1.identityId = "Alice";
    f1.confidence = 0.95f;
    auto t1 = tc.update({f1}, 1000);
    assert(t1.size() == 1);

    // Frame 2 — 重叠 bbox 应保持同一 track
    pipeline::DetectedFace f2;
    f2.bbox = cv::Rect(105, 105, 78, 78);  // 高度重叠
    f2.isAuthenticated = true;
    f2.identityId = "Alice";
    f2.confidence = 0.93f;
    auto t2 = tc.update({f2}, 1100);
    assert(t2.size() == 1);
    assert(t2[0].trackId == 1);  // 同一 track
    return true;
}
```

- [ ] **Step 4: 编译运行测试 + 提交**

```powershell
cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON
cmake --build build_ci --target core_unit_tests
ctest --test-dir build_ci -R track_coordinator
```

---

### Task 11: ResultPublisher + PerfReporter — 副作用收口

**Files:**
- Create: `src/cpp/include/pipeline/ResultPublisher.h`
- Create: `src/cpp/src/pipeline/ResultPublisher.cpp`
- Create: `src/cpp/include/pipeline/PerfReporter.h`
- Create: `src/cpp/src/pipeline/PerfReporter.cpp`
- Create: `tests/cpp/test_result_publisher.cpp`

- [ ] **Step 1: ResultPublisher 定义**

```cpp
// src/cpp/include/pipeline/ResultPublisher.h
#pragma once
#include <functional>
#include <string>
#include <mutex>
#include <vector>
#include <opencv2/core.hpp>
#include "pipeline/TrackCoordinator.h"

namespace pipeline {

struct DomainEvent {
    std::string type;       // "VERIFIED" | "AUTH_FAIL" | "NO_FACE" | "FACES"
    std::string message;
    long long timestampMs = 0;
};

struct FrameOutcome {
    cv::Mat renderFrame;
    std::vector<TrackView> tracks;
    std::vector<DomainEvent> events;
    struct {
        double decodeMs = 0.0, preMs = 0.0, inferMs = 0.0,
               postMs = 0.0, renderMs = 0.0;
        long long rssBytes = 0;
    } stats;
};

class ResultPublisher {
public:
    using Callback = std::function<void(const std::string&)>;
    using RenderSink = std::function<void(const cv::Mat&, uint64_t)>;

    ResultPublisher(Callback onResult, RenderSink onRender);

    // 副作用：回调节流 + 渲染帧发布 + 异常事件
    void publish(const FrameOutcome& outcome);

private:
    Callback onResult_;
    RenderSink onRender_;
    // 节流计时器
    long long lastNoFaceMs_ = 0, lastUnknownMs_ = 0,
              lastVerifiedMs_ = 0, lastMultiMs_ = 0;
};

}  // namespace pipeline
```

- [ ] **Step 2: PerfReporter 定义**

```cpp
// src/cpp/include/pipeline/PerfReporter.h
#pragma once
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <deque>

namespace pipeline {

struct PerfStats {
    double decodeMs = 0.0, preMs = 0.0, inferMs = 0.0,
           postMs = 0.0, renderMs = 0.0;
    long long rssBytes = 0;
};

class PerfReporter {
public:
    PerfReporter(const std::string& csvPath, const std::string& envOutDir = "tests/metrics");
    ~PerfReporter();

    // 无锁入队（热路径安全）
    void submit(const PerfStats& s);

private:
    void workerLoop();  // 后台线程：聚合 + 写 CSV

    std::deque<PerfStats> queue_;
    std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> running_{true};
    std::string csvPath_;
};

}  // namespace pipeline
```

- [ ] **Step 3: 实现 PerfReporter 异步写入**

```cpp
// src/cpp/src/pipeline/PerfReporter.cpp
#include "pipeline/PerfReporter.h"
#include <cstdio>
#include <algorithm>
#include <numeric>

namespace pipeline {

PerfReporter::PerfReporter(const std::string& csvPath, const std::string& envOutDir)
    : csvPath_(csvPath)
{
    worker_ = std::thread(&PerfReporter::workerLoop, this);
}

PerfReporter::~PerfReporter() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void PerfReporter::submit(const PerfStats& s) {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(s);
    if (queue_.size() > 1024) queue_.pop_front();  // 防止无限积累
}

void PerfReporter::workerLoop() {
    while (running_) {
        std::deque<PerfStats> batch;
        {
            std::lock_guard<std::mutex> lk(mu_);
            batch.swap(queue_);
        }
        if (batch.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // 聚合 P50/P95
        std::vector<double> totals;
        totals.reserve(batch.size());
        for (const auto& s : batch) {
            totals.push_back(s.decodeMs + s.preMs + s.inferMs + s.postMs + s.renderMs);
        }
        std::sort(totals.begin(), totals.end());
        double p50 = totals[totals.size() / 2];
        double p95 = totals[static_cast<size_t>(totals.size() * 0.95)];
        double mean = std::accumulate(totals.begin(), totals.end(), 0.0) / totals.size();

        // 写 CSV（追加模式）
        FILE* f = fopen(csvPath_.c_str(), "a");
        if (f) {
            for (const auto& s : batch) {
                fprintf(f, "%g,%g,%g,%g,%g,%lld\n",
                    s.decodeMs, s.preMs, s.inferMs, s.postMs, s.renderMs, (long long)s.rssBytes);
            }
            fclose(f);
        }
    }
}

}  // namespace pipeline
```

- [ ] **Step 4: 提交**

```bash
git add src/cpp/include/pipeline/ResultPublisher.h \
        src/cpp/src/pipeline/ResultPublisher.cpp \
        src/cpp/include/pipeline/PerfReporter.h \
        src/cpp/src/pipeline/PerfReporter.cpp \
        tests/cpp/test_result_publisher.cpp
git commit -m "feat: 提取 ResultPublisher + PerfReporter 管线副作用收口"
```

---

### Task 12: EngineRuntime 装配 + JNI 适配

**Files:**
- Modify: `src/cpp/src/Engine.cpp` (1294→~150 行)
- Modify: `src/cpp/include/Engine.h` (213→~40 行)
- Modify: `src/cpp/CMakeLists.txt`

- [ ] **Step 1: 改写 Engine.h — 外部接口保留，内部替换**

```cpp
// src/cpp/include/Engine.h — 公开接口不变，private 替换为管线组件
#include "pipeline/FrameSource.h"
#include "pipeline/TrackCoordinator.h"
#include "pipeline/ResultPublisher.h"
#include "pipeline/PerfReporter.h"

class Engine {
public:
    // === 公开接口（不变）===
    bool initialize(...);
    void run();
    void stop();
    void setMode(MonitoringMode mode);
    void setFlip(bool flipX, bool flipY);
    bool getRenderFrame(cv::Mat& outFrame);
    bool getRenderFrame(cv::Mat& outFrame, uint64_t& seq);
    void setOnResultCallback(std::function<void(std::string)> cb);
    void setExternalInputEnabled(bool enabled);
    void configureExternalInput(FrameBackpressureMode mode, std::size_t capacity);
    void setMaxFrames(int max) { maxFrames_ = max; }

private:
    // === 管线组件 ===
    std::unique_ptr<pipeline::FrameSource> source_;
    std::unique_ptr<BioAuth> bioAuth_;
    std::unique_ptr<MotionDetector> motionDetector_;
    std::unique_ptr<pipeline::TrackCoordinator> trackCoordinator_;
    std::unique_ptr<pipeline::ResultPublisher> publisher_;
    std::unique_ptr<pipeline::PerfReporter> perfReporter_;

    // === 运行时状态（最小化）===
    std::atomic<bool> isRunning{false};
    std::atomic<bool> initialized_{false};
    std::atomic<MonitoringMode> currentMode{MonitoringMode::CONTINUOUS};
    int maxFrames_ = 0;

    // Preprocess 参数
    std::atomic<bool> flipXEnabled{false}, flipYEnabled{false};
};
```

- [ ] **Step 2: 改写 run() — ~15 行**

```cpp
void Engine::run() {
    isRunning = true;
    while (isRunning) {
        auto packet = source_->next(30);
        if (!packet) continue;

        // Motion gate
        if (currentMode == MonitoringMode::MOTION_TRIGGERED &&
            !motionDetector_->detect(packet->bgr)) {
            cv::Mat annotated = packet->bgr.clone();
            cv::putText(annotated, "WAITING", cv::Point(20, 40),
                cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);
            pipeline::FrameOutcome outcome;
            outcome.renderFrame = annotated;
            publisher_->publish(outcome);
            continue;
        }

        // 预处理（resize + flip）
        long long preStart = nowMs();
        cv::Mat processed;
        preprocess(packet->bgr, processed);  // 私有 helper
        double preMs = static_cast<double>(nowMs() - preStart);

        // 检测 + 识别（BioAuth）
        long long inferStart = nowMs();
        std::vector<BioAuth::FaceAuthResult> results;
        bioAuth_->verifyMulti(processed, results, 4, true);
        double inferMs = static_cast<double>(nowMs() - inferStart);

        // 转化为 DetectedFace 格式
        std::vector<pipeline::DetectedFace> faces;
        for (auto& r : results) {
            pipeline::DetectedFace df;
            df.bbox = r.face;
            df.identityId = r.identity.id;
            df.confidence = r.identity.confidence;
            df.isAuthenticated = r.identity.isAuthenticated;
            faces.push_back(df);
        }

        // 跟踪（纯函数）
        auto tracks = trackCoordinator_->update(faces, packet->timestampMs);

        // 标注
        cv::Mat annotated = processed.clone();
        annotate(annotated, tracks);

        // 组装结果
        pipeline::FrameOutcome outcome;
        outcome.renderFrame = annotated;
        outcome.tracks = tracks;
        outcome.stats.decodeMs = packet->decodeMs;
        outcome.stats.preMs = preMs;
        outcome.stats.inferMs = inferMs;

        // 副作用
        publisher_->publish(outcome);
        perfReporter_->submit(outcome.stats);
    }
}
```

- [ ] **Step 3: 编译 + JNI 验证**

```powershell
cmake --build build_win --config Release --target core_unit_tests
ctest --test-dir build_win -C Release
```

- [ ] **Step 4: 提交**

```bash
git add src/cpp/src/Engine.cpp \
        src/cpp/include/Engine.h \
        src/cpp/CMakeLists.txt
git commit -m "refactor: Engine 值流管线化 — 317行 processFrame 消亡"
```

---

## 回归总验

全部 12 个 Task 完成后：

```powershell
# 全量构建 + 全量测试
cmake --build build_win --config Release --target win_local_service win_unit_tests core_unit_tests
ctest --test-dir build_win -C Release --output-on-failure

# E2E
pnpm -C web build
pnpm -C web e2e:run
```

- [ ] 确认 HTTP 端点全部正常（GET/PUT/POST 返回格式不变）
- [ ] 确认 MJPEG 推流无中断
- [ ] 确认相机切换后仍可识别
- [ ] 确认模型热重载结果正确
