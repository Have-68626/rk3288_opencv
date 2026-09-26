// FACE-ARCWIN-002：`ArcFaceWinRecognizer::initialize()` 的**初始化自检块**判别用例。
//
// 背景（为什么此前被列为「收回项」）：`initialize()` 在 `embedder_.initialize()` 成功之后，会
// **合成一张 112×112 灰图真跑一次前向**，失败即写 stderr 并 `return false`（fail loud）。该块此前
// **全仓零用例引用** —— 删掉它不会有任何测试变红，等于「防静默的最后一道闸门」自身没有护栏。
//
// 为什么曾被判「不好测」（现在怎么绕开）：
//   ① 前置需要真实的 ArcFace 权重（生产默认 `models/arcface_w600k_r50.onnx` 不在库内，`models/*` 被
//      gitignore）⇒ 改用**自造的极小合成 ONNX**（`tests/fixtures/onnx/`，生成脚本与期望 sha256 见
//      该目录的 `gen_tiny_onnx.py`）。`cv::dnn::readNet` 接受它，已实测（2026-09-26）。
//   ② `dbPath` 指向**不存在**的路径即可（`FaceDatabase::load()` 对该情况返回 true —— 这是**有意语义**，
//      见该函数 javadoc）⇒ `return db_.load(...)` 不会失败，判据被孤立在自检块上。
//
// 三个用例**互相对照，缺一不可**：
//   A **阳性对照**：`Flatten`+`Slice` 件（输出**恰好** 512）⇒ `initialize` 必须返回 true。
//     ★ 没有它，B/C 的「返回 false」无法区分「自检拦住了」与「前置本身不可用」——
//       `:29` 的 cascade 加载失败出口**无本类自带诊断**（OpenCV 的 FileStorage 会自己打一行
//       `Can't open file`），会制造归因困难。⚠️ 它的准确角色是「**前置可用性哨兵**」，
//       **不是**自检块的守护者（自检被短路时它仍绿）：守护自检块的是 B/C 两条。
//       ⚠️ 另：它把 `dbPath` **不存在 ⇒ `FaceDatabase::load()` 返回 true** 这一语义硬耦合了进来
//       （见文件头 ②）⇒ 该语义若被改，本文件**三条一起红**；届时请先看 `FaceDatabase::load` 的契约
//       （那条语义是**有意**的，`tests/win/test_face_database_encryption.cpp` 有它的钉住用例）。
//   B 维度不符：`Identity` 件（输入 `1×3×112×112` 被原样透传 ⇒ 输出 37632 ≠ 512）⇒ 必须 false。
//   C 输入 blob 名不存在：A 的模型 + `setNodeNames("…不存在的名字…", "")` ⇒ 必须 false。
//   ⇒ B/C 的判别点：**删掉自检块后两者都会变成 true**（2026-09-26 实测），故本文件能钉住该块。
//
// ⚠️ 前置缺失时的取向：两个固定件与 cascade 都在库内/仓内，缺失即**真错** ⇒ 报错并返回 false，
//    **不静默跳过**（与「模型缺失可跳过」那类不同）。

#include "rk_win/ArcFaceWinRecognizer.h"

#include <filesystem>
#include <iostream>
#include <string>

#ifndef RK_TEST_SOURCE_ROOT
#error "本用例依赖 RK_TEST_SOURCE_ROOT 定位仓内固定件；缺失它会让定位静默失效（见 cmake/windows.cmake）"
#endif

namespace {

std::filesystem::path repoFile(const std::string& rel) {
    return std::filesystem::path(RK_TEST_SOURCE_ROOT) / rel;
}

struct Probe {
    std::filesystem::path cascade;
    std::filesystem::path dbPath;            // 不存在 ⇒ FaceDatabase::load 返回 true（有意语义）
    std::filesystem::path okModel;           // 输出恰 512 ⇒ 自检应通过
    std::filesystem::path dimMismatchModel;  // 输出 37632 ⇒ 自检应拦住
    bool ready = false;
    std::string why;
};

Probe makeProbe() {
    Probe p;
    p.cascade = repoFile("app/src/main/assets/lbpcascade_frontalface.xml");
    p.okModel = repoFile("tests/fixtures/onnx/tiny_flatten_slice_512.onnx");
    p.dimMismatchModel = repoFile("tests/fixtures/onnx/tiny_identity_1x512.onnx");
    p.dbPath = std::filesystem::temp_directory_path() / "rk_arcwin_selfcheck" / "db_does_not_exist.bin";
    std::error_code ec;
    std::filesystem::remove(p.dbPath, ec);  // 确保它不存在
    for (const auto& f : {p.cascade, p.okModel, p.dimMismatchModel}) {
        if (!std::filesystem::exists(f)) {
            p.why = "前置缺失（固定件应在库内）：" + f.string();
            return p;
        }
    }
    p.ready = true;
    return p;
}

}  // namespace

// A 阳性对照：可用模型 + 不存在的 dbPath ⇒ initialize 成功。
// 判别力：若 cascade 缺失/合成件被 readNet 拒绝，本用例红 —— 正是它把 B/C 的假绿堵住。
bool test_arcwin_selfcheck_positive_control() {
    const Probe p = makeProbe();
    if (!p.ready) {
        std::cerr << "[arcwin_selfcheck] " << p.why << std::endl;
        return false;
    }
    rk_win::ArcFaceWinRecognizer rec;
    if (!rec.initialize(p.cascade.string(), p.dbPath, p.okModel.string(), 60, 0.45)) {
        std::cerr << "[arcwin_selfcheck] 阳性对照失败：initialize 未成功。`initialize` 有四个失败出口，"
                     "本用例无法区分是哪一个 —— ① cascade 加载失败（该出口**无本类自带诊断**，"
                     "但 OpenCV 的 FileStorage 会自己打一行 `Can't open file`）；② 嵌入器 initialize 失败"
                     "（会打印 `…initialize failed:`）；③ **本文件要守护的自检块**失败（打印「初始化自检失败」）；"
                     "④ `db_.load()` 返回 false（本例 dbPath 不存在 ⇒ 返回 true，见文件头 ②）。"
                     "⇒ 请按 stderr 的具体前缀归因，不要一律当成「前置坏了」" << std::endl;
        return false;
    }
    return true;
}

// B 输出维度不符 ⇒ 自检必须拦住（删掉自检块后此处返回 true ⇒ 本用例变红）。
bool test_arcwin_selfcheck_rejects_dim_mismatch() {
    const Probe p = makeProbe();
    if (!p.ready) {
        std::cerr << "[arcwin_selfcheck] " << p.why << std::endl;
        return false;
    }
    rk_win::ArcFaceWinRecognizer rec;
    if (rec.initialize(p.cascade.string(), p.dbPath, p.dimMismatchModel.string(), 60, 0.45)) {
        std::cerr << "[arcwin_selfcheck] 自检未拦住「输出维度不是 512」的模型 —— "
                     "删掉 initialize() 的自检块会让本用例变红，故此处红即该块缺失或失效" << std::endl;
        return false;
    }
    return true;
}

// C 输入 blob 名不存在 ⇒ 自检必须拦住（同 B 的判别点，覆盖 setNodeNames 那条分支）。
bool test_arcwin_selfcheck_rejects_unknown_input_blob() {
    const Probe p = makeProbe();
    if (!p.ready) {
        std::cerr << "[arcwin_selfcheck] " << p.why << std::endl;
        return false;
    }
    rk_win::ArcFaceWinRecognizer rec;
    rec.setNodeNames("nonexistent_blob_xyz", "");
    if (rec.initialize(p.cascade.string(), p.dbPath, p.okModel.string(), 60, 0.45)) {
        std::cerr << "[arcwin_selfcheck] 自检未拦住不存在的输入 blob 名 —— 同上，红即自检块缺失或失效"
                  << std::endl;
        return false;
    }
    return true;
}
