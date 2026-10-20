#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/IRecognizer.h"  // threshold()：BootstrapResult::recognizer 是 unique_ptr<IRecognizer>，需完整类型
#include "rk_win/WinConfig.h"
#include "FileHash.h"  // rk_wcfr::calculateSHA256：与 RuntimeBootstrap 同源哈希实现

#include <filesystem>
#include <iostream>
#include <string>

namespace {

// ctest 以 build 根为 cwd，且 CI 不构建 win_local_service（assets 拷贝挂在其 POST_BUILD 上），
// 相对 "assets/" 无法保证可解析。按优先级探测候选路径，取首个存在者；全部缺失则返回 false，
// 由调用方判失败——明确暴露「测试前置缺失」而非静默假绿。
bool probeCascadePath(std::filesystem::path& out) {
    const std::filesystem::path cwd = std::filesystem::current_path();
    for (const auto& cand : {
             std::filesystem::path(RK_TEST_SOURCE_ROOT) / "app" / "src" / "main" / "assets" / "lbpcascade_frontalface.xml",
             cwd / "assets" / "lbpcascade_frontalface.xml",
             cwd.parent_path() / "app" / "src" / "main" / "assets" / "lbpcascade_frontalface.xml",
             cwd / "app" / "src" / "main" / "assets" / "lbpcascade_frontalface.xml"}) {
        if (std::filesystem::exists(cand)) {
            out = cand;
            return true;
        }
    }
    return false;
}

}  // namespace

// 测试：合法配置至少返回 cascade + dnn 两个 ModelSnapshot
bool test_bootstrap_returns_models_on_valid_config() {
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = "assets/lbpcascade_frontalface.xml";
    cfg.recognition.databasePath = ".";
    cfg.dnn.enable = false;  // 关闭 DNN 以避免依赖真实模型文件

    auto result = rk_win::RuntimeBootstrap::build(cfg);

    // 至少应返回 cascade + dnn（disabled）两个快照。
    // 注：本用例**不**断言 result.ok，故 cascadePath 沿用相对 "assets/..."（可能不存在）也不影响
    //   判据——两个快照无论装配成败都会生成。需要真实 cascade 的用例（哈希 / 阈值注入）另走
    //   probeCascadePath，理由见其函数上方注释。
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
    std::filesystem::path cascadePath;
    if (!probeCascadePath(cascadePath)) return false;

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
        // 注：arcface 模式 recognizerOk（快照 status 的来源）同时依赖 embedder（onnx）初始化，
        // 本配置下 status 为 "failed" 属预期；hash 才是本用例的判别点。
        return m.hash == rk_wcfr::calculateSHA256(cfg.recognition.cascadePath);
    }
    return false;  // 未找到 cascade 快照
}

// 守卫：RecognitionConfig 的 ArcFace 节点名字段**默认必须为空串**（FACE-ARCWIN-001 回归钉住）。
// 语义：空串 = OpenCV-DNN 走「自动默认输入 / 首个输出」兜底，这正是 FACE-ARCWIN-001 的修复本身；
// 若有人把默认值改回 `data`/`output`（即把 ncnn 字段默认误当 OpenCV-DNN 真实节点名，正是本缺陷
// 的形态），本用例会立即变红——无需任何模型或 OpenCV 推理，CI 恒可跑。
// 与 RuntimeBootstrap::build() 调用 setNodeNames(cfg.recognition.arcFaceInputName, ...) 配套：
// 配置键缺省即空串 ⇒ 自动兜底；此处钉住这两个字段的结构体默认值不被改回硬编码。
// ⚠️ 该路径上的 `ArcFaceWinRecognizer::setNodeNames` 是「**空串 = 显式置空**」（逐字写入空串，
//   见 ArcFaceWinRecognizer.cpp），与 `ArcFaceAdapter::setNodeNames` 的「空串忽略、保留既有值」
//   **语义不同**——勿把两者的空串语义混读（后者在 C++ 核心引擎侧，见 FaceInferStages.cpp）。
bool test_recognition_config_node_names_default_empty() {
    const rk_win::RecognitionConfig cfg{};
    return cfg.arcFaceInputName.empty() && cfg.arcFaceOutputName.empty();
}

// 守卫：RecognitionConfig 的 ArcFace 阈值必须落在**余弦相似度值域**内（FACE-ARCWIN-006/007 回归钉住）。
// 语义：ArcFaceWinRecognizer::identify 用 `bestSim >= 阈值`（bestSim ∈ [-1,1]）判定；若有人把它改回
// LBPH 专用的 55.0 这类卡方距离值（值域约 [0,64]），`bestSim >= 55.0` 将恒假 ⇒ identify() 恒返回
// UNKNOWN（正是 FACE-ARCWIN-006 的形态）。断言「落在 [-1,1] 余弦值域内」即可：55.0 等 LBPH 量纲值
// 因远超上界 1.0 已被拒绝；不再额外拒绝恰好等于 1.0 者（那是 schema 允许的合法余弦阈值，拒绝属过度严格）。
// 模型无关、无需 OpenCV 推理，CI 恒可跑。
bool test_recognition_config_arcface_threshold_in_cosine_range() {
    const rk_win::RecognitionConfig cfg{};
    return cfg.identifyThresholdArcFace >= -1.0 && cfg.identifyThresholdArcFace <= 1.0;
}

// 护栏（机械）：两个阈值字段必须是**可区分的值**。
// 为什么需要：ArcFaceWinRecognizer 的成员默认值（0.45）与 RecognitionConfig::identifyThresholdArcFace
// 的默认值（0.45）相同 ⇒ 若有人用**默认值**写「ArcFace 装配是否注入了正确阈值」的断言，则「正确装配」
// 「把 LBPH 的 55.0 传错」「根本没注入（读成员默认）」三种情形给出同一读数，断言恒真、零判别力。
// 本用例钉住二者不相等，使上面那条装配断言（test_bootstrap_arcface_dir_injects_arcface_threshold）
// 注入 0.37 时仍然有效。改默认值若把两者改成相等，本用例立即红。
bool test_recognition_config_thresholds_are_distinguishable() {
    const rk_win::RecognitionConfig cfg{};
    return cfg.identifyThresholdArcFace != cfg.identifyThreshold;
}

// FACE-ARCWIN-008：装配级断言——arcface 分支必须把 `identifyThresholdArcFace` 注入识别器。
// 判别点：RuntimeBootstrap 调 ArcFaceWinRecognizer::initialize 的第 5 实参。若被写成
// `identifyThreshold`（55.0，即 FACE-ARCWIN-006 的成形形态）或与 LBPH 分支共用同一实参，本用例红。
// 不需要任何模型文件：生产权重 models/arcface_w600k_r50.onnx 不在仓 ⇒ initialize() 稳定停在 embedder
// 失败出口（在 db 加载之前）；该出口此前读不到注入值，故 ArcFaceWinRecognizer::initialize 已把
// identifyThreshold_ 的赋值前置到函数最前（Step 1，见该文件注释）。
bool test_bootstrap_arcface_dir_injects_arcface_threshold() {
    std::filesystem::path cascadePath;
    if (!probeCascadePath(cascadePath)) {
        std::cerr << "[test_bootstrap_arcface_dir_injects_arcface_threshold] 前置缺失："
                     "未找到 lbpcascade_frontalface.xml（探测候选见 probeCascadePath）"
                  << std::endl;
        return false;
    }

    rk_win::AppConfig cfg;
    cfg.model.recognition = "arcface";
    cfg.recognition.cascadePath = cascadePath;
    // 两个路径都取**不存在**的值：arcFaceModelPath 缺失 ⇒ initialize() 停在 embedder 失败出口，
    // 本用例正是要在该出口上观测注入的阈值（无需任何模型文件）；databasePath 同样取不存在值，
    // 避免任何「意外读到仓内某个真实人脸库」的耦合。
    cfg.recognition.databasePath = "nonexistent/arcface_db.yml";
    cfg.recognition.arcFaceModelPath = "nonexistent/arcface.onnx";
    // 注入 0.37（≠ 任何默认值）——用默认 0.45 断言会恒真，见上方护栏用例。
    cfg.recognition.identifyThresholdArcFace = 0.37;
    // 同时把 LBPH 阈值设成 55.0（默认值也一样）：若实现误传 LBPH 阈值，读数会是 55.0 而非 0.37。
    cfg.recognition.identifyThreshold = 55.0;
    cfg.dnn.enable = false;

    auto result = rk_win::RuntimeBootstrap::build(cfg);
    if (!result.recognizer) {
        std::cerr << "[test_bootstrap_arcface_dir_injects_arcface_threshold] 未构造识别器（fail-open 契约破损）"
                  << std::endl;
        return false;
    }
    if (result.recognizer->threshold() != 0.37) {
        std::cerr << "[test_bootstrap_arcface_dir_injects_arcface_threshold] 读到阈值 "
                  << result.recognizer->threshold()
                  << "（期望 0.37）——可能把 identifyThreshold（LBPH 量纲）传给了第 5 实参，"
                     "或未注入/未前置赋值（见 ArcFaceWinRecognizer::initialize 开头注释）"
                  << std::endl;
        return false;
    }
    // 顺带钉住本配置确实走的是**识别器初始化失败**出口（否则上面的读数来自另一条路径，判别力不同）。
    return !result.ok;
}

// FACE-ARCWIN-008 的 LBPH 对称面：lbph 分支必须把 `identifyThreshold` 注入识别器。
// 判别点：RuntimeBootstrap 调 FaceRecognizer::initialize 的第 4 实参。
// ⚠️ databasePath 必须是**不存在**的路径：传 "." 会命中 FaceDatabase::load 的明文分支
// （cv::FileStorage 打不开目录 ⇒ 返回 false）⇒ result.ok 变 false，本用例会误红。
// 不存在的路径走 FaceDatabase::load 的 `!exists ⇒ 返回 true`（空库）⇒ 初始化成功。
bool test_bootstrap_lbph_dir_injects_lbph_threshold() {
    std::filesystem::path cascadePath;
    if (!probeCascadePath(cascadePath)) {
        std::cerr << "[test_bootstrap_lbph_dir_injects_lbph_threshold] 前置缺失："
                     "未找到 lbpcascade_frontalface.xml（探测候选见 probeCascadePath）"
                  << std::endl;
        return false;
    }

    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = cascadePath;
    cfg.recognition.databasePath = "nonexistent/lbph_db.yml";  // 不存在 ⇒ 空库、load 成功（见上方说明）
    cfg.recognition.identifyThreshold = 42.0;  // 注入值（≠ 默认 55.0）
    cfg.dnn.enable = false;

    auto result = rk_win::RuntimeBootstrap::build(cfg);
    if (!result.recognizer) {
        std::cerr << "[test_bootstrap_lbph_dir_injects_lbph_threshold] 未构造识别器" << std::endl;
        return false;
    }
    if (result.recognizer->threshold() != 42.0) {
        std::cerr << "[test_bootstrap_lbph_dir_injects_lbph_threshold] 读到阈值 "
                  << result.recognizer->threshold() << "（期望 42.0）——可能传错第 4 实参" << std::endl;
        return false;
    }
    return result.ok;
}
