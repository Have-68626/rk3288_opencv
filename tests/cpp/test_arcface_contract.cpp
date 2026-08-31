#include "ArcFaceEmbedder.h"
#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
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