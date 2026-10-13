#include <gtest/gtest.h>
#include "ModelRegistry.h"
#include "adapters/MobileFaceNetAdapter.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace rk_core;

TEST(AdapterTest, ModelRegistryInstance) {
    auto& registry = ModelRegistry::instance();
    // 单例必须始终有效
    EXPECT_NE(&registry, nullptr);
}

TEST(AdapterTest, ModelRegistryListAllEmpty) {
    auto& registry = ModelRegistry::instance();
    // 未注册任何模型时，listAll 应返回空列表
    auto all = registry.listAll();
    EXPECT_TRUE(all.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// §2.5-A：MobileFaceNetAdapter 的节点名与输出维度必须与本仓模型一致
// ═══════════════════════════════════════════════════════════════════════════════
//
// 缺陷原貌：适配器把 `ex.input("data")` / `ex.extract("output")` 与「输出必须恰 **128** 维」
// **硬编码**在实现里，而本仓 `models/mobilefacenet_ncnn/mobilefacenet.param` 与
// `models/mobilefacenet_int8_ncnn/mobilefacenet_int8.param` 实为 `Input in0`、末层输出 `out0`、
// **512** 维 ⇒ 该适配器对这两个模型**必然在 ex.input 处即失败**，且没有任何调用方能改。
//
// 下方用例分两层：
//   ① 配置面（**始终**运行，**CI 也跑**）：默认值、注入、空串忽略、维度可设
//      —— 它把默认值钉死为 `in0`/`out0`/512，故**适配器常量漂移在 CI 可被拦**；
//   ② 与真实模型对齐（模型缺失则 skip）：直接解析 `.param` 的**实际**节点名与输出维度，与适配器默认值
//      逐项比对。模型二进制不入库（gitignored）⇒ **本用例在 CI 上恒 SKIP**，只在「有模型的本机」有效
//      （子代理 4 S1 指出原注释未写明这点，等于给人「CI 已守住」的错觉）。

namespace {

// 解析 ncnn .param 的「输入 blob 名 / 末层输出 blob 名 / 末层 num_output」。
// 行格式：<layer_type> <layer_name> <bottom_count> <top_count> [bottoms…] [tops…] [k=v…]
// ⇒ top 的首个元素在下标 `4 + bottom_count`；num_output 取末层的 `0=<n>` 参数。
struct NcnnParamInfo {
    bool ok = false;
    std::string inputBlob;
    std::string outputBlob;
    int numOutput = -1;
};

NcnnParamInfo parseNcnnParam(const std::string& path) {
    NcnnParamInfo info;
    std::ifstream in(path);
    if (!in) return info;

    std::string line;
    if (!std::getline(in, line)) return info;  // 魔数 7767517
    if (!std::getline(in, line)) return info;  // 层数 / Blob 数

    std::vector<std::vector<std::string>> layers;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::vector<std::string> tok;
        std::string t;
        while (ss >> t) tok.push_back(t);
        if (!tok.empty()) layers.push_back(std::move(tok));
    }
    if (layers.empty()) return info;

    for (const auto& tok : layers) {
        if (tok.size() >= 5 && tok[0] == "Input") {
            info.inputBlob = tok[4];
            break;
        }
    }

    const auto& last = layers.back();
    if (last.size() >= 4) {
        const int bottomCount = std::atoi(last[2].c_str());
        const std::size_t topIdx = 4 + static_cast<std::size_t>(bottomCount);
        if (topIdx < last.size()) {
            info.outputBlob = last[topIdx];
            for (std::size_t i = topIdx + 1; i < last.size(); ++i) {
                if (last[i].rfind("0=", 0) == 0) {
                    info.numOutput = std::atoi(last[i].c_str() + 2);
                    break;
                }
            }
        }
    }

    info.ok = !info.inputBlob.empty() && !info.outputBlob.empty() && info.numOutput > 0;
    return info;
}

}  // namespace

// ① 配置面（始终运行）
TEST(AdapterTest, MobileFaceNetNodeNamesAndDimAreConfigurable) {
    rk_core::MobileFaceNetAdapter adapter;

    // 默认值必须与本仓模型一致（in0/out0、512），否则「开箱即用」是假的。
    EXPECT_EQ(adapter.inputNodeName(), "in0");
    EXPECT_EQ(adapter.outputNodeName(), "out0");
    EXPECT_EQ(adapter.expectedEmbeddingDim(), 512);
    EXPECT_EQ(adapter.embeddingDim(), 512) << "embeddingDim 必须反映期望维度，而非硬编码常量";

    adapter.setNodeNames("node_in", "node_out");
    EXPECT_EQ(adapter.inputNodeName(), "node_in");
    EXPECT_EQ(adapter.outputNodeName(), "node_out");

    // 空串忽略（与 ArcFaceAdapter::setNodeNames 同语义）：只给一个时不清空另一个。
    adapter.setNodeNames("only_in", "");
    EXPECT_EQ(adapter.inputNodeName(), "only_in");
    EXPECT_EQ(adapter.outputNodeName(), "node_out");

    adapter.setExpectedEmbeddingDim(256);
    EXPECT_EQ(adapter.expectedEmbeddingDim(), 256);
    EXPECT_EQ(adapter.embeddingDim(), 256);
}

// ② 与真实模型文件对齐（缺模型时跳过）。
// 这条是「适配器常量 ↔ 模型文件」耦合的直接守卫：任何一侧漂移都会在此报红，
// 而不是等到设备上 `ex.input` 静默失败。
TEST(AdapterTest, MobileFaceNetDefaultsMatchShippedModelFiles) {
    const char* params[] = {
        "models/mobilefacenet_ncnn/mobilefacenet.param",
        "models/mobilefacenet_int8_ncnn/mobilefacenet_int8.param",
    };

    int checked = 0;
    for (const char* p : params) {
        if (!std::filesystem::exists(p)) continue;
        const NcnnParamInfo info = parseNcnnParam(p);
        ASSERT_TRUE(info.ok) << "无法解析 ncnn param: " << p;

        rk_core::MobileFaceNetAdapter adapter;  // 每个模型都用默认值比对
        EXPECT_EQ(adapter.inputNodeName(), info.inputBlob)
            << p << "：适配器默认输入节点名与模型实际不符";
        EXPECT_EQ(adapter.outputNodeName(), info.outputBlob)
            << p << "：适配器默认输出节点名与模型实际不符";
        EXPECT_EQ(adapter.expectedEmbeddingDim(), info.numOutput)
            << p << "：适配器默认期望维度与模型末层 num_output 不符";
        ++checked;
    }

    if (checked == 0) {
        // ⚠️ 如实标注**生效范围**，别让 skip 被读成 pass（子代理 4 S1；子代理 3 独立指出同一问题）：
        //   · `models/*` 是 gitignored 的二进制 ⇒ **CI 上不存在** ⇒ 本用例在 CI **恒 SKIP**；
        //   · 本机有模型时，cwd 还必须是仓库根（直接跑 `bin/Release/` 里的 exe 扫不到）——
        //     ctest 路径已由 `CMakeLists.txt` 给 `add_test` 加 `WORKING_DIRECTORY` 解决。
        //   ⇒ 这是**本机专用**的对齐校验；CI 侧真正守默认值的是同文件里无跳过条件的用例 ①。
        GTEST_SKIP() << "本机无 mobilefacenet 模型（gitignored，CI 上恒无）⇒ 跳过与真实模型的对齐比对。"
                        "注意：这不是「通过」——CI 中本用例不提供任何保护；"
                        "默认值漂移由 MobileFaceNetNodeNamesAndDimAreConfigurable 在 CI 拦截。";
    }
}
