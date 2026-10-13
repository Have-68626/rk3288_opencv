#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/WinConfig.h"
#include "FileHash.h"  // rk_wcfr::calculateSHA256：与 RuntimeBootstrap 同源哈希实现

#include <filesystem>
#include <string>

// 测试：合法配置至少返回 cascade + dnn 两个 ModelSnapshot
bool test_bootstrap_returns_models_on_valid_config() {
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
bool test_bootstrap_reports_failure_on_bad_cascade() {
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

// 测试：arcface 模式下 cascade 快照的 hash 必须取 cascadePath（RW2 修复回归钉住）。
// 判别点：arcFaceModelPath 指向不存在的文件。修复前 hash 取 arcFaceModelPath
// （不存在 → "(unavailable)"），修复后 hash 取 cascadePath → 与真实文件 SHA256 一致。
// 只断言 cascade 快照的 hash，不引入对真实 arcface 模型文件的依赖。
bool test_bootstrap_arcface_snapshot_hash_uses_cascade_path() {
    // ctest 以 build 根为 cwd，且 CI 不构建 win_local_service（assets 拷贝挂在其
    // POST_BUILD 上），相对 "assets/" 无法保证可解析。按优先级探测候选路径，
    // 取首个存在者；全部缺失则判失败，明确暴露测试前置缺失而非静默假绿。
    const std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path cascadePath;
    for (const auto& cand : {
             cwd / "assets" / "lbpcascade_frontalface.xml",
             cwd.parent_path() / "app" / "src" / "main" / "assets" / "lbpcascade_frontalface.xml",
             cwd / "app" / "src" / "main" / "assets" / "lbpcascade_frontalface.xml"}) {
        if (std::filesystem::exists(cand)) {
            cascadePath = cand;
            break;
        }
    }
    if (cascadePath.empty()) return false;

    rk_win::AppConfig cfg;
    cfg.model.recognition = "arcface";
    cfg.recognition.cascadePath = cascadePath;
    cfg.recognition.databasePath = ".";
    // 不存在的模型路径（判别性关键）：修复前 hash 取它 → "(unavailable)"，
    // 与 cascadePath 真实哈希不等，使本用例在修复回退时必然失败。
    cfg.recognition.arcFaceModelPath = "nonexistent/arcface.onnx";
    cfg.dnn.enable = false;

    auto result = rk_win::RuntimeBootstrap::build(cfg);

    for (const auto& m : result.models) {
        if (m.id != "cascade_frontalface") continue;
        // 修复前 hash 取 arcFaceModelPath（不存在）→ "(unavailable)"；
        // 断言 hash == cascadePath 实际 SHA256 钉住 RW2 修复。
        // 注：arcface 模式 cascadeOk 同时依赖 embedder（onnx）初始化，本配置下
        // status 为 "failed" 属预期；hash 才是本用例的判别点。
        return m.hash == rk_wcfr::calculateSHA256(cfg.recognition.cascadePath);
    }
    return false;  // 未找到 cascade 快照
}
