#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/WinConfig.h"

#include <string>

// 测试：合法配置至少返回 cascade + dnn 两个 ModelSnapshot
static bool test_bootstrap_returns_models_on_valid_config() {
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = "assets/lbpcascade_frontalface.xml";
    cfg.recognition.databasePath = ".";
    cfg.dnn.enable = false;  // 关闭 DNN 以避免依赖真实模型文件

    auto result = rk_win::RuntimeBootstrap::build(cfg);

    // 至少应返回 cascade + dnn（disabled）两个快照
    if (result.models.size() < 2) {
        return false;
    }

    bool hasCascade = false;
    bool hasDnn = false;
    for (const auto& m : result.models) {
        if (m.id == "cascade_frontalface") hasCascade = true;
        if (m.id == "dnn_face_detector") hasDnn = true;
    }

    return hasCascade && hasDnn;
}

// 测试：无效 cascade 路径应返回 ok=false
static bool test_bootstrap_reports_failure_on_bad_cascade() {
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = "/nonexistent/path/to/cascade.xml";
    cfg.recognition.databasePath = ".";
    cfg.dnn.enable = false;

    auto result = rk_win::RuntimeBootstrap::build(cfg);

    // 关键组件应标记为失败
    if (result.ok) return false;
    if (result.warning.empty()) return false;

    // cascade 快照应标记为 failed
    for (const auto& m : result.models) {
        if (m.id == "cascade_frontalface" && m.status != "failed") {
            return false;
        }
    }

    return true;
}
