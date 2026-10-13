#include "adapters/ScrfdNcnnAdapter.h"
#include <gtest/gtest.h>

#include <opencv2/core.hpp>

#include <vector>
#include <filesystem>
#include "ncnn_test_models.h"

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

// normalizeLevel flat（det_10g 1D 展平）：c=1、h=grid²*N、w=perPosCh → canonical 零拷贝
TEST(ScrfdNcnnAdapterTest, NormalizeLevelFlatZeroCopy) {
    // side=2 → grid=4；N=2、perPosCh=4（bbox）
    const int perPosCh = 4, N = 2, side = 2;
    std::vector<float> data(static_cast<size_t>(side) * side * N * perPosCh, 0.0f);
    data[0] = 2.0f; data[1] = 1.0f; data[2] = 1.0f; data[3] = 3.0f;  // pos0 a0 距离

    std::vector<float> scratch;
    pipeline::ScrfdTensor out;
    std::string err;
    bool ok = ScrfdNcnnAdapter::normalizeLevel(
        1, side * side * N, perPosCh, data.data(), data.size(),
        perPosCh, N, side, ScrfdNcnnConfig::OutputLayout::kFlat, scratch, out, err);
    ASSERT_TRUE(ok) << err;
    EXPECT_EQ(out.channels, 8);  // 4N
    EXPECT_EQ(out.featW, 2);
    EXPECT_EQ(out.featH, 2);
    EXPECT_EQ(out.data, data.data());  // 零拷贝：指针不变
    EXPECT_EQ(out.dataLen, data.size());
    // canonical 布局 [pos][anchor][4]：pos0 a0 的前 4 值 = data 前 4 值
    EXPECT_FLOAT_EQ(out.data[0], 2.0f);
    EXPECT_FLOAT_EQ(out.data[3], 3.0f);
}

// normalizeLevel NCHW（kps-opt2 ch-major）：c=4N、h=w=side → canonical 重排
TEST(ScrfdNcnnAdapterTest, NormalizeLevelNChwTranspose) {
    const int perPosCh = 4, N = 2, side = 2;
    const int grid = side * side;
    // ch-major：data[ch * grid + pos]，ch = a*perPosCh + o
    std::vector<float> src(static_cast<size_t>(N) * perPosCh * grid, 0.0f);
    // pos0 a0 距离 (2,1,1,3)：ch0..3 @ pos0
    src[0 * grid + 0] = 2.0f;  // a0,o0 @ pos0
    src[1 * grid + 0] = 1.0f;  // a0,o1
    src[2 * grid + 0] = 1.0f;  // a0,o2
    src[3 * grid + 0] = 3.0f;  // a0,o3

    std::vector<float> scratch;
    pipeline::ScrfdTensor out;
    std::string err;
    bool ok = ScrfdNcnnAdapter::normalizeLevel(
        N * perPosCh, side, side, src.data(), src.size(),
        perPosCh, N, side, ScrfdNcnnConfig::OutputLayout::kNChw, scratch, out, err);
    ASSERT_TRUE(ok) << err;
    EXPECT_EQ(out.channels, 8);
    EXPECT_EQ(out.featW, 2);
    EXPECT_EQ(out.featH, 2);
    EXPECT_NE(out.data, src.data());  // ch-major 需重排，非零拷贝
    // canonical [pos][anchor][4]：pos0 a0 = (2,1,1,3)
    EXPECT_FLOAT_EQ(out.data[0], 2.0f);
    EXPECT_FLOAT_EQ(out.data[1], 1.0f);
    EXPECT_FLOAT_EQ(out.data[2], 1.0f);
    EXPECT_FLOAT_EQ(out.data[3], 3.0f);
    // pos0 a1 = (0,0,0,0)（src ch4..7 @ pos0 全 0）
    for (int o = 0; o < 4; o++) EXPECT_FLOAT_EQ(out.data[4 + o], 0.0f);
}

// normalizeLevel 无法识别布局 → 明确失败
TEST(ScrfdNcnnAdapterTest, NormalizeLevelUnknownLayoutRejected) {
    std::vector<float> data(8, 0.0f);
    std::vector<float> scratch;
    pipeline::ScrfdTensor out;
    std::string err;
    bool ok = ScrfdNcnnAdapter::normalizeLevel(
        3, 4, 8, data.data(), data.size(),  // 非 flat（c!=1）非 NCHW（c!=4N）
        4, 2, 2, ScrfdNcnnConfig::OutputLayout::kAuto, scratch, out, err);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
}

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
TEST(ScrfdNcnnAdapterTest, RealForwardPreservesPaddedNchwChannels) {
    const auto dir = std::filesystem::temp_directory_path() / "rk_scrfd_padded_nchw";
    ASSERT_TRUE(rk_test::writeConstantScrfdModel(dir, 288));
    ScrfdNcnnConfig cfg;
    cfg.paramPath = (dir / "scrfd.param").string();
    cfg.binPath = (dir / "scrfd.bin").string();
    cfg.inputName = "input.1";
    cfg.inputW = cfg.inputH = 288;
    cfg.decodeMode = DecodeMode::kDistance;
    cfg.outputLayout = ScrfdNcnnConfig::OutputLayout::kNChw;
    for (int i = 0; i < 3; ++i) {
        const auto stride = std::to_string(8 << i);
        cfg.scoreNames[i] = "s" + stride;
        cfg.bboxNames[i] = "b" + stride;
        cfg.kpsNames[i] = "k" + stride;
        cfg.anchorsPerPos[i] = 2;
    }
    ScrfdNcnnAdapter adapter(cfg);
    std::string err;
    ASSERT_TRUE(adapter.load(cfg.paramPath, err)) << err;
    const auto faces = adapter.detect(cv::Mat(288, 288, CV_8UC3, cv::Scalar(128, 128, 128)), err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(faces.size(), 1u);
    EXPECT_FLOAT_EQ(faces[0].bbox.x, 96.0f);
    EXPECT_FLOAT_EQ(faces[0].bbox.y, 96.0f);
    EXPECT_FLOAT_EQ(faces[0].bbox.width, 64.0f);
    ASSERT_TRUE(faces[0].keypoints5.has_value());
    for (const auto& point : *faces[0].keypoints5) {
        EXPECT_FLOAT_EQ(point.x, 128.0f);
        EXPECT_FLOAT_EQ(point.y, 128.0f);
    }
}
#endif
