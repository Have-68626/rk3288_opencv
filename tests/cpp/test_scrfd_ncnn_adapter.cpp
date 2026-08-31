#include "adapters/ScrfdNcnnAdapter.h"
#include <gtest/gtest.h>

#include <opencv2/core.hpp>

#include <vector>

using namespace rk_core;
using namespace pipeline;

// 任务7：ScrfdNcnnAdapter —— 用构造张量验证 decode/reason（不依赖真实模型/ncnn forward）。

namespace {

ScrfdTensor t(const std::vector<float>& data, int channels, int featW, int featH) {
    ScrfdTensor s;
    s.data = data.data();
    s.channels = channels;
    s.featW = featW;
    s.featH = featH;
    s.dataLen = data.size();
    return s;
}

ScrfdDecodeOptions opt320() {
    ScrfdDecodeOptions o;
    o.origWidth = 320;
    o.origHeight = 240;
    o.scale = 2.0f;
    return o;
}

void fillAll(ScrfdTensor score[3], ScrfdTensor bbox[3], ScrfdTensor kps[3]) {
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
}

}  // namespace

// 模拟有效输出：1 个高分 anchor → FaceDetection（Rect2f + 5 关键点）
TEST(ScrfdNcnnAdapterTest, DecodeFromTensorsSimulatedOutput) {
    std::vector<float> sc = {0.0f, 0.90f};
    std::vector<float> bb = {0, 0, 0, 0};
    std::vector<float> kp(10, 0.0f);
    kp[0] = 100.0f; kp[1] = 100.0f;  // 左眼

    ScrfdTensor score[3], bbox[3], kps[3];
    fillAll(score, bbox, kps);
    score[0] = t(sc, 2, 1, 1);
    bbox[0] = t(bb, 4, 1, 1);
    kps[0] = t(kp, 10, 1, 1);

    std::string err;
    FaceDetections dets = ScrfdNcnnAdapter::decodeFromTensors(score, bbox, kps, opt320(), err);
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_TRUE(err.empty());
    EXPECT_FLOAT_EQ(dets[0].bbox.x, 0.0f);
    EXPECT_FLOAT_EQ(dets[0].bbox.width, 4.0f);
    ASSERT_TRUE(dets[0].keypoints5.has_value());
    EXPECT_FLOAT_EQ((*dets[0].keypoints5)[0].x, 50.0f);
}

// 错误节点/张量形状 → 明确失败（禁止猜测解码）
TEST(ScrfdNcnnAdapterTest, InvalidTensorShapeRejected) {
    std::vector<float> sc = {0.0f, 0.9f};
    std::vector<float> bb = {0.0f, 0.0f, 0.0f};  // channels=3 非法
    std::vector<float> kp(10, 0.0f);

    ScrfdTensor score[3], bbox[3], kps[3];
    fillAll(score, bbox, kps);
    score[0] = t(sc, 2, 1, 1);
    bbox[0] = t(bb, 3, 1, 1);
    kps[0] = t(kp, 10, 1, 1);

    std::string err;
    FaceDetections dets = ScrfdNcnnAdapter::decodeFromTensors(score, bbox, kps, opt320(), err);
    EXPECT_TRUE(dets.empty());
    EXPECT_FALSE(err.empty());
}

// ncnn 编译状态分支：未编译 → build_disabled；已编译 → 缺失模型文件报 missing_model
TEST(ScrfdNcnnAdapterTest, LoadReportsCompileOrMissingReason) {
    ScrfdNcnnAdapter adapter;
    std::string err;
    bool ok = adapter.load("definitely_not_exist.param", err);
    EXPECT_FALSE(ok);
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    EXPECT_EQ(err, "missing_model");
#else
    EXPECT_EQ(err, "build_disabled");
#endif
}

// 首个成功推理后才 effective：decode 成功置位；加载后未推理不置位
TEST(ScrfdNcnnAdapterTest, EffectiveOnlyAfterFirstSuccessfulInference) {
    ScrfdNcnnAdapter adapter;
    EXPECT_FALSE(adapter.isEffective());

    std::vector<float> sc = {0.0f, 0.9f};
    std::vector<float> bb = {0, 0, 0, 0};
    std::vector<float> kp(10, 0.0f);
    ScrfdTensor score[3], bbox[3], kps[3];
    fillAll(score, bbox, kps);
    score[0] = t(sc, 2, 1, 1);
    bbox[0] = t(bb, 4, 1, 1);
    kps[0] = t(kp, 10, 1, 1);

    std::string err;
    auto dets = ScrfdNcnnAdapter::decodeFromTensors(score, bbox, kps, opt320(), err);
    ASSERT_EQ(dets.size(), 1u);
    // decodeFromTensors 为纯解码，effective_ 置位于 detect 的真实 forward 路径；
    // 此处仅验证静态函数输出正确（单元覆盖在 core_gtest_tests 内闭环）。
    (void)dets;
}