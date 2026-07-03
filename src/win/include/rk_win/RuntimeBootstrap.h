#pragma once
#include <memory>
#include <string>
#include <vector>

namespace rk_win {

// 前向声明
class IRecognizer;
class DnnSsdFaceDetector;
struct AppConfig;

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
    std::string warning;  // 非致命警告（如 DNN 初始化失败）
    bool ok = false;      // false = 关键组件初始化失败
};

class RuntimeBootstrap {
public:
    // 纯装配：只构造对象，不启动线程，不写日志，不写 render_.status
    static BootstrapResult build(const AppConfig& cfg);
};

}  // namespace rk_win
