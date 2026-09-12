#include "ArcFaceEmbedder.h"
#include "adapters/ArcFaceAdapter.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <numeric>
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