#include "ModelRegistry.h"

using namespace rk_core;

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

// 并发查询/注册压力测试：
// - writer 线程交替覆盖同一 key 的 entry（内容不同、长 notes 增大 string 撕裂窗口）；
// - reader 线程反复 getEntry 后在锁外读取 entry 字段，断言读到完整一致的快照。
// 修复前 getEntry 返回锁外裸指针（&map 内元素），同 key 覆盖会并发改写 entry 的
// std::string 字段（data race：读到撕裂内容或已释放缓冲），快照不一致；
// 修复后返回 optional<ModelEntry> 拷贝，锁外读取必然完整一致。
// 用 auto 声明以同时兼容两种返回类型，测试代码在修复前后无需改动。
bool test_model_registry_concurrent_get_and_register() {
    ModelRegistry::ensureBuiltinRegistered();
    auto& reg = ModelRegistry::instance();

    std::atomic<bool> stop{false};
    std::atomic<int> badReads{0};
    std::atomic<int> totalReads{0};

    std::thread writer([&] {
        const ModelEntry a{"race_key", "Display A", "detect", std::string(2048, 'A'), "high_accuracy", 1};
        const ModelEntry b{"race_key", "Display B", "recognize", std::string(2048, 'B'), "high_speed", 2};
        int i = 0;
        while (!stop.load()) {
            reg.registerDetector("race_key", [] { return std::unique_ptr<FaceDetector>(); },
                                 (i++ & 1) ? b : a);
            if ((i % 64) == 0) {
                const ModelEntry fresh{"fresh_" + std::to_string(i), "Fresh", "detect",
                                       std::string(64, 'F'), "high_speed", 3};
                reg.registerDetector(fresh.id, [] { return std::unique_ptr<FaceDetector>(); }, fresh);
            }
        }
    });

    std::thread reader([&] {
        while (!stop.load()) {
            auto e = reg.getEntry("race_key");
            if (e) {
                totalReads.fetch_add(1);
                // 放大小竞态窗口：模拟调用方在锁外做其他事后再解引用
                std::this_thread::yield();
                const bool consistent =
                    (e->taskType == "detect" && e->displayName == "Display A" &&
                     e->notes.size() == 2048 && e->notes[0] == 'A' && e->notes[2047] == 'A') ||
                    (e->taskType == "recognize" && e->displayName == "Display B" &&
                     e->notes.size() == 2048 && e->notes[0] == 'B' && e->notes[2047] == 'B');
                if (!consistent) badReads.fetch_add(1);
            }
        }
    });

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(600)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop = true;
    writer.join();
    reader.join();

    return totalReads.load() > 0 && badReads.load() == 0;
}

// FACE-SCRFD-004：`retinaface_scrfd` 注册条目**已被删除**（不是改指）—— 本用例锁住这个决定。
//
// 为什么是删除而不是改指 ScrfdNcnnAdapter（三条独立理由，详见 ModelRegistry.cpp 该处注释）：
//   1) 该 ID **零消费者**（无 profile / manifest / Java / Web / JNI / CLI 引用）；
//   2) ★ 决定性：注册表以 `make_unique<ScrfdNcnnAdapter>()` **默认构造**后只调 `load(path)`
//      ⇒ 用的是默认 `cfg_`（`kDelta` + `anchorsPerPos={1,1,1}`），**本路径不消费 profile**；
//      真实 SCRFD det_10g 需 `kDistance` + 2 anchor ⇒ 经注册表取得的实例**必然解错框位**。
//      故改指只能修好加载器、解码照样错，等于换一个坑；
//   3) 原指向的 RetinaFaceAdapter 用 delta 解码（SCRFD 是距离编码），且加载器只吃 ONNX，
//      与已记录的文档意图（CREDITS.md「NCNN SCRFD」记为 BIN/PARAM）矛盾。
//
// 判别力（已实测）：把该条目加回注册表（无论指向 RetinaFaceAdapter 还是 ScrfdNcnnAdapter）
// ⇒ `getEntry` 有值 ⇒ 本用例**红**。
// 反向护栏：同族的 `scrfd_0.5gf` 必须仍在，防止「删多了」。
bool test_model_registry_has_no_retinaface_scrfd_entry() {
    ModelRegistry::ensureBuiltinRegistered();
    auto& reg = ModelRegistry::instance();

    if (reg.getEntry("retinaface_scrfd").has_value()) {
        std::fprintf(stderr, "retinaface_scrfd 条目应已被删除，实际仍存在于注册表\n");
        return false;
    }
    if (reg.createDetector("retinaface_scrfd", nullptr) != nullptr) {
        std::fprintf(stderr, "retinaface_scrfd 应无法创建实例，实际创建成功\n");
        return false;
    }

    // 反向护栏：0.5g 档位不得被误删（本批只删 retinaface_scrfd 一条）。
    auto det05 = reg.createDetector("scrfd_0.5gf", nullptr);
    if (!det05) {
        std::fprintf(stderr, "scrfd_0.5gf 条目被误删\n");
        return false;
    }
    return true;
}
