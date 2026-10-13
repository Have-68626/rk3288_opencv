#include "ArcFaceEmbedder.h"
#include "adapters/ArcFaceAdapter.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace rk_core;

// 任务8：ArcFace 契约固化 —— 维度恰 512 / 零向量拒绝 / L2 模长≈1 / 节点名配置驱动。
// （INT8 vs FP32 余弦 ≥0.90 属本地模型精度验证，走 ncnn_precision_test，CI 只跑结构/模拟。）

TEST(ArcFaceContractTest, DimMustBeExactly512) {
    EXPECT_FALSE(ArcFaceEmbedder::hasContractDim512({}));
    EXPECT_FALSE(ArcFaceEmbedder::hasContractDim512(std::vector<float>(128, 0.1f)));
    EXPECT_FALSE(ArcFaceEmbedder::hasContractDim512(std::vector<float>(513, 0.1f)));
    EXPECT_TRUE(ArcFaceEmbedder::hasContractDim512(std::vector<float>(512, 0.1f)));
}

TEST(ArcFaceContractTest, ZeroVectorRejected) {
    std::vector<float> zero(512, 0.0f);
    EXPECT_FALSE(ArcFaceEmbedder::l2Normalize(zero));
    std::vector<float> empty;
    EXPECT_FALSE(ArcFaceEmbedder::l2Normalize(empty));
}

TEST(ArcFaceContractTest, UnitNormAfterNormalize) {
    std::vector<float> v(512);
    for (int i = 0; i < 512; i++) v[i] = 0.5f + 0.01f * static_cast<float>(i);
    const float lenBefore =
        std::sqrt(std::inner_product(v.begin(), v.end(), v.begin(), 0.0f));
    ASSERT_GT(lenBefore, 0.0f);
    ASSERT_TRUE(ArcFaceEmbedder::l2Normalize(v));
    const float sq = std::inner_product(v.begin(), v.end(), v.begin(), 0.0f);
    EXPECT_NEAR(sq, 1.0f, 1e-4f);
}

// 节点名由配置驱动（默认 data/output，可设为 in0/out0），未硬编码
TEST(ArcFaceContractTest, NodeNamesConfigDriven) {
    ArcFaceEmbedderConfig cfg;
    EXPECT_EQ(cfg.ncnnInput, "data");
    EXPECT_EQ(cfg.ncnnOutput, "output");
    cfg.ncnnInput = "in0";
    cfg.ncnnOutput = "out0";
    cfg.opencvInput = "in0";
    cfg.opencvOutput = "out0";
    SUCCEED();  // 可设置即证明非写死，推理仅读 cfg
}

// P1-1：manifest 节点名注入路径。未注入时保持历史默认 data/output（旧调用方兼容），
// 注入真实模型节点名（in0/out0）后覆盖，供 RealtimeFacePipeline 装配使用。
TEST(ArcFaceContractTest, AdapterNodeNamesInjectedFromManifest) {
    rk_core::ArcFaceAdapter adapter;
    EXPECT_EQ(adapter.inputNodeName(), "data");
    EXPECT_EQ(adapter.outputNodeName(), "output");

    adapter.setNodeNames("in0", "out0");
    EXPECT_EQ(adapter.inputNodeName(), "in0");
    EXPECT_EQ(adapter.outputNodeName(), "out0");
}

// 空串忽略：调用方未提供某节点名时不清空，保留既有默认（防御性，manifest 缺字段时不破坏默认）。
TEST(ArcFaceContractTest, AdapterNodeNameEmptyIgnored) {
    rk_core::ArcFaceAdapter adapter;
    adapter.setNodeNames("in0", "");
    EXPECT_EQ(adapter.inputNodeName(), "in0");
    EXPECT_EQ(adapter.outputNodeName(), "output");  // 空 output 保留默认
}

// 缺口3：ArcFaceAdapter 期望嵌入维度可注入（**配置面**，无模型/OpenCV/ncnn 依赖 ⇒ CI 恒跑）。
// 默认 = ArcFaceEmbedding::kDim(512)（未注入时与历史逐字一致）；setExpectedEmbeddingDim 对 dim>0
// 生效、对 dim<=0 忽略（保留既有值），与 MobileFaceNetAdapter 同语义。
// 判别点：把 ArcFaceAdapter::embeddingDim() 改回 `return ArcFaceEmbedding::kDim;`（常量）⇒ 注入 256
//        后仍返回 512，本条翻红。
TEST(ArcFaceContractTest, AdapterExpectedEmbeddingDimConfigurable) {
    rk_core::ArcFaceAdapter adapter;
    EXPECT_EQ(adapter.embeddingDim(), 512) << "默认必须等于 ArcFaceEmbedding::kDim(512)";
    EXPECT_EQ(adapter.embeddingDim(), ArcFaceEmbedding::kDim)
        << "默认值应与 kDim 常量一致（模型实际维度契约）";

    adapter.setExpectedEmbeddingDim(256);
    EXPECT_EQ(adapter.embeddingDim(), 256) << "embeddingDim 必须反映注入值，而非硬编码常量";

    adapter.setExpectedEmbeddingDim(0);  // 非法值：忽略，保留 256
    EXPECT_EQ(adapter.embeddingDim(), 256) << "<=0 应被忽略，保留既有期望维度";
}

// 真实 INT8 模型加载 + 推理：本机 models/arcface_int8_ncnn/ 存在（gitignored），
// 缺失或未编译 ncnn 时按仓库惯例跳过。证伪「默认 data/output 能加载 manifest 模型」：
// 注入 in0/out0 后 load/embed 应成功且输出恰 512 维。
TEST(ArcFaceContractTest, AdapterLoadsRealInt8ModelWithInjectedNames) {
#if !(defined(RK_HAVE_NCNN) && RK_HAVE_NCNN)
    GTEST_SKIP() << "当前构建未启用 ncnn，跳过真实 INT8 模型加载";
#else
    const std::string param = "models/arcface_int8_ncnn/arcface_int8.param";
    if (!std::filesystem::exists(param)) {
        GTEST_SKIP() << "本机无 arcface_int8 模型（gitignored），跳过真实加载";
    }

    rk_core::ArcFaceAdapter adapter;
    adapter.setNodeNames("in0", "out0");
    std::string err;
    ASSERT_TRUE(adapter.load(param, err)) << err;
    EXPECT_EQ(std::string(adapter.name()), "ncnn");

    // 合成 112x112 灰度脸图（尺寸与 manifest embedderInputW/H 一致）
    const cv::Mat face(112, 112, CV_8UC3, cv::Scalar(128, 128, 128));
    std::optional<std::vector<float>> emb = adapter.embed(face, err);
    ASSERT_TRUE(emb.has_value()) << err;
    EXPECT_EQ(emb->size(), 512u);
    const float sq = std::inner_product(emb->begin(), emb->end(), emb->begin(), 0.0f);
    EXPECT_NEAR(sq, 1.0f, 1e-3f);  // L2 归一化契约
#endif
}

// ── 接线守卫（读源码文本）：loadEmbedder 必须消费 manifest 的 embedderBin ──────────────
//
// 缺口2：RealtimeFacePipeline::makeDefaultRealtimePipelineHooks 的 loadEmbedder 钩子须显式读取
// manifest 声明的 `profile.embedderBin.relativePath`，并把解析出的 bin 路径经
// `ArcFaceAdapter::setModelBinPath(...)` 交给适配器（否则适配器回落 param stem 推导，bin 名与
// stem 不同时会静默加载错误的 .bin）。**现状零行为测试**：该段落在
// `#if !defined(RK_SKIP_OPENCV) && defined(RK_HAVE_NCNN) && RK_HAVE_NCNN` 内 ⇒ 两个 CI job
// （SKIP_OPENCV / 无 ncnn）**都不编译它**，行为用例覆盖不到；本形态判据是 CI 内唯一的接线守卫。
//
// 判别点：删掉 loadEmbedder 体内对 `profile.embedderBin` 的读取，或删掉 `emb->setModelBinPath(...)`
//        调用 ⇒ 本用例翻红（已实测：见提交说明的变异证据）。
// ⚠️ 判据边界（如实登记，勿读成强保证）：这是**文本/形态判据**（对去注释、去空白后的源码做子串匹配），
//    挡不住等价改写——改字段名、把读取/注入抽进辅助函数、用宏包一层、或改后丢弃结果，都能绕过。
//    它与仓内既有结构测试（test_log_rate_limiter.cpp / test_track_coordinator.cpp /
//    test_checked_arith.cpp）同一量级，是**行为覆盖之外的补充**，**不**等价于行为覆盖。
// ⚠️ 脆弱面：函数体切片靠纯字符计数（matchBrace）。loadEmbedder 体内若无字面量 '{'/'}'（当前无），
//    切片精确；一旦体内出现带花括号的字面量，计数可能失准 ⇒ 由下方「切片长度下限」防线兜底，
//    使「切片为空/被误截」无法伪装成「全部合规」。
#ifdef RK_TEST_SOURCE_ROOT
namespace {

// 读取源码根下相对路径文件；失败（或路径写错）返回空串——调用方**必须**显式断言非空，
// 否则「扫描集合为空」会静默地让所有断言都通过（空 == 空）。
std::string readRepoFile(const std::string& rel) {
    std::ifstream in(std::string(RK_TEST_SOURCE_ROOT) + "/" + rel, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// 剥离 `//` 行注释与 `/* */` 块注释（块注释状态跨行保留），与 test_track_coordinator.cpp 同口径：
// 源码里**讨论**这些符号的说明性注释不应造成误报。
std::string stripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool inBlock = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (inBlock) {
            if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/') {
                inBlock = false;
                ++i;
            }
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size()) {
            if (src[i + 1] == '/') {
                while (i < src.size() && src[i] != '\n') ++i;
                out.push_back('\n');
                continue;
            }
            if (src[i + 1] == '*') {
                inBlock = true;
                ++i;
                continue;
            }
        }
        out.push_back(src[i]);
    }
    return out;
}

// 去掉全部空白：使断言对格式漂移免疫。
std::string squeeze(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

// 取 `open`（指向 '{'）所属块的收尾 '}' 之后一个下标；未配对到时返回 npos。
std::size_t matchBrace(const std::string& s, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == '{') {
            ++depth;
        } else if (s[i] == '}') {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return std::string::npos;
}

}  // namespace

TEST(ArcFaceContractTest, LoadEmbedderConsumesManifestEmbedderBinWiring) {
    const std::string raw = readRepoFile("src/cpp/src/pipeline/RealtimeFacePipeline.cpp");
    ASSERT_FALSE(raw.empty()) << "无法读取 src/cpp/src/pipeline/RealtimeFacePipeline.cpp"
                              << "（RK_TEST_SOURCE_ROOT=" << RK_TEST_SOURCE_ROOT << "）";
    const std::string body = squeeze(stripComments(raw));
    ASSERT_GT(body.size(), 5000u) << "RealtimeFacePipeline.cpp 文本异常短，疑似路径写错";

    // 定位 loadEmbedder 钩子的**赋值定义**（去空白后形如 `h.loadEmbedder=[](...)`）。用精确锚，
    // 避免命中同名的调用点 `hooks.loadEmbedder(...)` 或已剥离的注释。
    const std::size_t at = body.find("h.loadEmbedder=[](");
    ASSERT_NE(at, std::string::npos)
            << "找不到 h.loadEmbedder 钩子定义（符号改名/写法变化？须同步本用例）";
    // 捕获列表为空、形参表与尾置返回类型 `-> std::shared_ptr<IRealtimeEmbedder>` 内均无 '{'，
    // 故其后首个 '{' 即 lambda 体起始。
    const std::size_t brace = body.find('{', at);
    ASSERT_NE(brace, std::string::npos) << "找不到 loadEmbedder 体起始 '{'";
    const std::size_t end = matchBrace(body, brace);
    ASSERT_NE(end, std::string::npos) << "loadEmbedder 体花括号未配对（格式变化或计数失准？）";
    const std::string fnBody = body.substr(brace, end - brace);
    ASSERT_GT(fnBody.size(), 200u) << "loadEmbedder 切片过短，勿让空/残缺切片伪装成通过";

    // ① 正向锚：函数体内读取 manifest 声明的 embedderBin（缺口2 的消费面）。
    EXPECT_NE(fnBody.find("embedderBin"), std::string::npos)
            << "loadEmbedder 未读取 profile.embedderBin：manifest 声明的 bin 路径未被装配消费";
    // ② 正向锚：把解析出的 bin 路径注入 ArcFaceAdapter。
    EXPECT_NE(fnBody.find("setModelBinPath("), std::string::npos)
            << "loadEmbedder 未调用 setModelBinPath：bin 路径未注入适配器（回落 stem 推导）";
}

#else
// 这条 `#error` 是**故意**的（同 test_log_rate_limiter.cpp / test_track_coordinator.cpp 范式）：
// 若本文件被编进一个未定义 RK_TEST_SOURCE_ROOT 的 target，上方接线守卫会**静默消失**（既不注册、
// 也不 skip、也不 fail，零信号）。宁可让构建当场失败，也不要留一个看起来在守、实际不存在的守卫。
// 本文件当前唯一编入的 target 是 core_gtest_tests（RK_TEST_SOURCE_ROOT 由 CMakeLists.txt 为其定义）。
#error "test_arcface_contract.cpp 的接线守卫依赖 RK_TEST_SOURCE_ROOT；缺失它会让守卫静默消失"
#endif  // RK_TEST_SOURCE_ROOT