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
    std::string modelVersion;
    std::string hash;
    std::string status;       // "loaded" | "failed" | "missing" | "disabled"
    bool isInUse = false;
    std::string lastError;
};

struct BootstrapResult {
    BootstrapResult() = default;
    // 自定义析构：成员里 `IRecognizer` 与 `DnnSsdFaceDetector` **两者**都只被前向声明（见文件头），
    // 而 unique_ptr 的析构需要完整类型 ⇒ 析构必须在能看到这两个完整类型的 .cpp 中定义
    // （见 RuntimeBootstrap.cpp 的 `BootstrapResult::~BootstrapResult() = default;`）。
    ~BootstrapResult();
    BootstrapResult(BootstrapResult&&) noexcept = default;
    BootstrapResult& operator=(BootstrapResult&&) noexcept = default;
    std::unique_ptr<IRecognizer> recognizer;
    std::unique_ptr<DnnSsdFaceDetector> detector;
    std::vector<ModelSnapshot> models;
    // 告警文本（**可含致命项**：识别器初始化失败时这里也会被写入，并同时把 ok 置 false；
    // DNN 初始化失败等非致命项亦写在此处，需结合 ok 判断是否致命）。
    std::string warning;
    // false = **识别器**初始化失败（DNN 初始化失败不置 false，只追加 warning）
    bool ok = false;
};

class RuntimeBootstrap {
public:
    // 纯装配：只构造对象，不启动线程，不写 render_.status；但**会**经识别器构造链向 stderr/stdout
    // 打自检输出（ArcFace / LBPH 识别器初始化时的失败 `std::cerr`、LBPH 成功时的 `std::cout` + SHA256）。
    static BootstrapResult build(const AppConfig& cfg);
};

}  // namespace rk_win
