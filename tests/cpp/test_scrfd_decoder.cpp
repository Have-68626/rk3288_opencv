#include "pipeline/ScrfdDecoder.h"
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace pipeline;

namespace {

// 组装一个有效 layer（stride 8 / 16 / 32 之一），score=2 通道 [bg,fg]
struct Layer {
    std::vector<float> score;  // anchors*2
    std::vector<float> bbox;   // anchors*4
    std::vector<float> kps;    // anchors*10
};

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
    o.scale = 2.0f;  // 640 / 320
    return o;
}

// distance 语义测试：scale=1 + 足大画布（100×100），避免裁剪干扰网格中心与距离断言。
// 保持 mode=默认 kDelta；distance 用例显式设 kDistance。
ScrfdDecodeOptions opt100() {
    ScrfdDecodeOptions o;
    o.origWidth = 100;
    o.origHeight = 100;
    o.padX = 0.0f;
    o.padY = 0.0f;
    o.scale = 1.0f;
    o.stride[0] = 8;
    return o;
}

float near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

}  // namespace

// 单脸：stride8 单 anchor → 原图坐标（含 letterbox scale）
TEST(ScrfdDecoderTest, SingleFaceLetterboxMappedToOrig) {
    Layer l;
    l.score = {0.0f, 0.90f};                // [bg, fg]
    l.bbox = {0, 0, 0, 0};                  // dx,dy,dw,dh
    l.kps.assign(10, 0.0f);
    l.kps[0] = 100.0f; l.kps[1] = 100.0f;   // 左眼 (letterbox)

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) {
        score[i] = bbox[i] = kps[i] = ScrfdTensor{};  // invalid
    }
    score[0] = t(l.score, 2, 1, 1);
    bbox[0] = t(l.bbox, 4, 1, 1);
    kps[0] = t(l.kps, 10, 1, 1);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_TRUE(err.empty());
    // anchor=(4,4) → bbox letterbox(0,0,8,8) → 原图(0,0,4,4)
    EXPECT_EQ(dets[0].bbox.x, 0);
    EXPECT_EQ(dets[0].bbox.y, 0);
    EXPECT_EQ(dets[0].bbox.width, 4);
    EXPECT_EQ(dets[0].bbox.height, 4);
    ASSERT_EQ(dets[0].keypoints.size(), 5u);
    EXPECT_TRUE(near(dets[0].keypoints[0].x, 50.0f));  // (100-0)/2
    EXPECT_TRUE(near(dets[0].keypoints[0].y, 50.0f));
}

// 多脸：同一层两个高分 anchor（相邻但不重叠）→ 两检测
TEST(ScrfdDecoderTest, MultiFaceAcrossAnchors) {
    Layer l;
    int n = 4;  // featW=featH=2
    l.score.assign(static_cast<size_t>(n) * 2, 0.0f);
    l.bbox.assign(static_cast<size_t>(n) * 4, 0.0f);
    l.kps.assign(static_cast<size_t>(n) * 10, 0.0f);
    l.score[1 * 2 + 1] = 0.95f;  // anchor(1,0)
    l.score[3 * 2 + 1] = 0.80f;  // anchor(1,1)
    // keypoints 落在各自 anchor 中心（letterbox）
    l.kps[1 * 10 + 0] = 12.0f;  l.kps[1 * 10 + 1] = 4.0f;
    l.kps[3 * 10 + 0] = 12.0f;  l.kps[3 * 10 + 1] = 12.0f;

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 2, 2);
    bbox[0] = t(l.bbox, 4, 2, 2);
    kps[0] = t(l.kps, 10, 2, 2);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    ASSERT_EQ(dets.size(), 2u);
    EXPECT_TRUE(near(dets[0].keypoints[0].x, 6.0f));  // 高分 anchor(1,0) → (12/2)
    EXPECT_TRUE(near(dets[1].keypoints[0].x, 6.0f));
    EXPECT_TRUE(near(dets[1].keypoints[0].y, 6.0f));
}

// 越界：解码框与原图无交集 → 后处理丢弃（非形状错误）
TEST(ScrfdDecoderTest, OutOfBoundsBoxDroppedByPostProcessor) {
    Layer l;
    l.score = {0.0f, 0.95f};
    l.bbox = {1000.0f, 0.0f, 0.0f, 0.0f};  // dx 巨大 → 框完全出帧
    l.kps.assign(10, 0.0f);

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 1, 1);
    bbox[0] = t(l.bbox, 4, 1, 1);
    kps[0] = t(l.kps, 10, 1, 1);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    EXPECT_TRUE(dets.empty());  // 越界无有效面积被丢弃
    EXPECT_TRUE(err.empty());   // 非形状错误
}

// 形状校验：bbox 通道数非法 → 拒绝 + 错误信息
TEST(ScrfdDecoderTest, InvalidBboxShapeRejected) {
    Layer l;
    l.score = {0.0f, 0.9f};
    l.bbox = {0, 0, 0};      // channels=3（非法）
    l.kps.assign(10, 0.0f);

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 1, 1);
    bbox[0] = t(l.bbox, 3, 1, 1);
    kps[0] = t(l.kps, 10, 1, 1);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    EXPECT_TRUE(dets.empty());
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("bbox"), std::string::npos);
}

// 层缺失：同 stride 三层未同时提供 → 拒绝猜测解码
TEST(ScrfdDecoderTest, MissingLayerRejected) {
    Layer l;
    l.score = {0.0f, 0.9f};
    l.bbox = {0, 0, 0, 0};
    // kps 不提供（invalid）

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 1, 1);
    bbox[0] = t(l.bbox, 4, 1, 1);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    EXPECT_TRUE(dets.empty());
    EXPECT_FALSE(err.empty());
}

// 重叠框：同一层两个高重叠高分 anchor → NMS 保留最高分
TEST(ScrfdDecoderTest, OverlappingNmsKeepsHighest) {
    Layer l;
    int n = 4;
    l.score.assign(static_cast<size_t>(n) * 2, 0.0f);
    l.bbox.assign(static_cast<size_t>(n) * 4, 0.0f);
    l.kps.assign(static_cast<size_t>(n) * 10, 0.0f);
    // 两个相邻 anchor 均用 dw=2 放大 → 框体大范围重叠
    for (int a : {0, 1}) {
        l.score[a * 2 + 1] = (a == 0) ? 0.90f : 0.95f;
        l.bbox[a * 4 + 2] = 2.0f;  // dw
    }

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 2, 2);
    bbox[0] = t(l.bbox, 4, 2, 2);
    kps[0] = t(l.kps, 10, 2, 2);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_TRUE(near(dets[0].score, 0.95f));
}

// 关键点随后处理同步：保留检测的关键点对应其原始检测
TEST(ScrfdDecoderTest, KeypointsFollowKeptDetections) {
    Layer l;
    l.score = {0.0f, 0.90f};   // fg=0.90，anchor(0,0) center (4,4)
    l.bbox = {0, 0, 1.0f, 0};  // dw → 放大，避免被丢弃
    l.kps.assign(10, 0.0f);
    l.kps[0] = 4.0f;  l.kps[1] = 4.0f;  // 左眼在 letterbox 中心

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 1, 1);
    bbox[0] = t(l.bbox, 4, 1, 1);
    kps[0] = t(l.kps, 10, 1, 1);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    ASSERT_EQ(dets.size(), 1u);
    ASSERT_EQ(dets[0].keypoints.size(), 5u);
    EXPECT_TRUE(near(dets[0].keypoints[0].x, 2.0f));  // (4-0)/2
    EXPECT_TRUE(near(dets[0].keypoints[0].y, 2.0f));
}

// 所有层均缺失 → 明确错误而非静默返回空
TEST(ScrfdDecoderTest, AllLayersMissingReturnsError) {
    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    EXPECT_TRUE(dets.empty());
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("无有效输出层"), std::string::npos);
}

// score 单通道（channels==1）分支解码
TEST(ScrfdDecoderTest, SingleChannelScoreDecodes) {
    Layer l;
    l.score = {0.93f};  // channels=1，直接置信度
    l.bbox = {0, 0, 0, 0};
    l.kps.assign(10, 0.0f);
    l.kps[0] = 8.0f; l.kps[1] = 8.0f;

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 1, 1, 1);
    bbox[0] = t(l.bbox, 4, 1, 1);
    kps[0] = t(l.kps, 10, 1, 1);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_TRUE(near(dets[0].score, 0.93f));
}

// anchorScale 影响检测框尺寸（任务7 接线真实 SCRFD anchor 的钩子）
TEST(ScrfdDecoderTest, AnchorScaleAffectsBoxSize) {
    Layer l;
    l.score = {0.0f, 0.9f};
    l.bbox = {0.5f, 0.5f, 0.0f, 0.0f};  // dx/dy 使 anchor 中心移到 8,8，框完全落原图内
    l.kps.assign(10, 0.0f);

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 1, 1);
    bbox[0] = t(l.bbox, 4, 1, 1);
    kps[0] = t(l.kps, 10, 1, 1);

    ScrfdDecodeOptions o = opt320();
    o.anchorScale[0] = 2.0f;  // stride8 × 2 → 基线 16
    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, o, err);
    ASSERT_EQ(dets.size(), 1u);
    // letterbox(0,0,16,16) → 原图(0,0,8,8)（默认 scale1 则为 4）
    EXPECT_EQ(dets[0].bbox.width, 8);
    EXPECT_EQ(dets[0].bbox.height, 8);
}

// dataLen 与形状不符 → 拒绝越界读
TEST(ScrfdDecoderTest, DataLenShortRejected) {
    Layer l;
    l.score = {0.0f, 0.9f};
    l.bbox = {0, 0, 0, 0};
    l.kps.assign(10, 0.0f);

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 1, 1);
    bbox[0] = t({0.0f, 0.0f}, 4, 1, 1);  // 声明 4 通道但只有 2 个值
    kps[0] = t(l.kps, 10, 1, 1);

    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, opt320(), err);
    EXPECT_TRUE(dets.empty());
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("数据长度不足"), std::string::npos);
}

// distance 语义（golden 查证 2026-09-03：官方 scrfd.py / nihui → 距离须乘 stride、中心=网格·stride 无 +0.5）。
// 用 featW=featH=5、stride=8，居中 pos(fx=2,fy=2) 锚中心=(16,16)，避免边缘裁剪干扰断言。
TEST(ScrfdDecoderTest, DistanceSingleAnchor) {
    // grid 5×5，N=1，score c=1
    Layer l;
    l.score.assign(25, 0.0f);
    l.bbox.assign(25 * 4, 0.0f);
    l.kps.assign(25 * 10, 0.0f);
    l.score[2 * 5 + 2] = 0.9f;
    l.bbox[(2 * 5 + 2) * 4 + 0] = 2.0f;  // d=(2,2,2,2)
    l.bbox[(2 * 5 + 2) * 4 + 1] = 2.0f;
    l.bbox[(2 * 5 + 2) * 4 + 2] = 2.0f;
    l.bbox[(2 * 5 + 2) * 4 + 3] = 2.0f;
    l.kps[(2 * 5 + 2) * 10 + 0] = 2.0f;  // 左眼偏移 (2,2) → 中心+2*stride
    l.kps[(2 * 5 + 2) * 10 + 1] = 2.0f;

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 1, 5, 5);
    bbox[0] = t(l.bbox, 4, 5, 5);
    kps[0] = t(l.kps, 10, 5, 5);

    ScrfdDecodeOptions o = opt100();
    o.mode = DecodeMode::kDistance;
    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, o, err);
    ASSERT_EQ(dets.size(), 1u) << err;
    // 中心 (16,16) 无 +0.5；距离乘 stride=8 → 框 x1=16-16=0, x2=32 → (0,0,32,32)
    EXPECT_EQ(dets[0].bbox.x, 0);
    EXPECT_EQ(dets[0].bbox.y, 0);
    EXPECT_EQ(dets[0].bbox.width, 32);
    EXPECT_EQ(dets[0].bbox.height, 32);
    // 左眼 = 中心16 + 偏移2*stride=16 → 32
    ASSERT_EQ(dets[0].keypoints.size(), 5u);
    EXPECT_TRUE(near(dets[0].keypoints[0].x, 32.0f));
    EXPECT_TRUE(near(dets[0].keypoints[0].y, 32.0f));
}

// distance + 每位置 2 anchor：canonical（score=2、bbox=8、kps=20）。
// 两 anchor 均过阈，构造框同中心不同大小 → NMS 保留高分 a0。
TEST(ScrfdDecoderTest, DistanceTwoAnchorsPerPos) {
    // grid 5×5，N=2：score c=2、bbox c=8、kps c=20
    Layer l;
    l.score.assign(25 * 2, 0.0f);
    l.bbox.assign(25 * 8, 0.0f);
    l.kps.assign(25 * 20, 0.0f);
    const int pos = 2 * 5 + 2;  // 中心 (16,16)
    const int a0 = pos * 2 + 0;  // rowBase
    const int a1 = pos * 2 + 1;
    l.score[a0] = 0.9f;
    l.score[a1] = 0.8f;
    for (int i = 0; i < 4; i++) {
        l.bbox[a0 * 4 + i] = 2.0f;  // a0 距离 (2,2,2,2)
        l.bbox[a1 * 4 + i] = 1.0f;  // a1 距离 (1,1,1,1)
    }
    l.kps[a0 * 10 + 0] = 2.0f;  l.kps[a0 * 10 + 1] = 2.0f;
    l.kps[a1 * 10 + 0] = 2.0f;  l.kps[a1 * 10 + 1] = 2.0f;

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(l.score, 2, 5, 5);
    bbox[0] = t(l.bbox, 8, 5, 5);
    kps[0] = t(l.kps, 20, 5, 5);

    ScrfdDecodeOptions o = opt100();
    o.mode = DecodeMode::kDistance;
    o.numAnchors[0] = 2;
    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, o, err);
    ASSERT_EQ(dets.size(), 1u) << err;  // a1 框 (16±8)=(8,8,16,16) 被 a0 (0,0,32,32) NMS 抑制
    EXPECT_TRUE(near(dets[0].score, 0.9f));
    EXPECT_EQ(dets[0].bbox.x, 0);
    EXPECT_EQ(dets[0].bbox.y, 0);
    EXPECT_EQ(dets[0].bbox.width, 32);
    EXPECT_EQ(dets[0].bbox.height, 32);
    ASSERT_EQ(dets[0].keypoints.size(), 5u);
    EXPECT_TRUE(near(dets[0].keypoints[0].x, 32.0f));
    EXPECT_TRUE(near(dets[0].keypoints[0].y, 32.0f));
}

// 模式切换回归：同一 bbox 张量用 kDelta vs kDistance 几何不同（distance 距离乘 stride，delta 走 exp 基线）。
TEST(ScrfdDecoderTest, DistanceVsDeltaDiffer) {
    // 居中 grid 5×5、stride8、pos(2,2)：distance 锚中心=(16,16)、delta=(20,20)（无 vs +0.5）
    const int pos = 2 * 5 + 2;
    const int grid5 = 5 * 5;

    // distance：score c=1（单 sigmoid）、bbox c=4、kps c=10，全网格填满
    std::vector<float> s1(grid5, 0.0f);
    std::vector<float> b1(grid5 * 4, 0.0f);
    std::vector<float> k1(grid5 * 10, 0.0f);
    s1[pos] = 0.9f;
    b1[pos * 4 + 0] = 1.0f;  b1[pos * 4 + 1] = 1.0f;  b1[pos * 4 + 2] = 1.0f;  b1[pos * 4 + 3] = 1.0f;
    k1[pos * 10 + 0] = 1.0f;  k1[pos * 10 + 1] = 1.0f;

    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t(s1, 1, 5, 5);
    bbox[0] = t(b1, 4, 5, 5);
    kps[0] = t(k1, 10, 5, 5);
    ScrfdDecodeOptions od = opt100();
    od.mode = DecodeMode::kDistance;
    std::string err;
    auto dist = ScrfdDecoder::decode(score, bbox, kps, od, err);
    ASSERT_EQ(dist.size(), 1u) << err;
    EXPECT_EQ(dist[0].bbox.width, 16);  // x1=16-8=8, x2=16+8=24
    EXPECT_EQ(dist[0].bbox.x, 8);

    // delta：[bg,fg] c=2、bbox c=4、kps c=10；cx=20+1*8=28，bw=8*exp(1)≈21.7 → width 不同
    std::vector<float> s2(grid5 * 2, 0.0f);
    s2[pos * 2 + 1] = 0.9f;
    score[0] = t(s2, 2, 5, 5);
    bbox[0] = t(b1, 4, 5, 5);
    kps[0] = t(k1, 10, 5, 5);
    auto delta = ScrfdDecoder::decode(score, bbox, kps, opt100(), err);  // 默认 kDelta
    ASSERT_FALSE(delta.empty()) << err;
    EXPECT_NE(delta[0].bbox.width, dist[0].bbox.width);
}

// distance + dataLen 越界：声明 N*4N*10N 但数据不足 → 拒绝
TEST(ScrfdDecoderTest, DistanceDataLenShortRejected) {
    ScrfdTensor score[3], bbox[3], kps[3];
    for (int i = 0; i < 3; i++) score[i] = bbox[i] = kps[i] = ScrfdTensor{};
    score[0] = t({0.4f, 0.4f, 0.4f, 0.4f}, 2, 2, 1);        // N=2 需 4 个 score
    bbox[0] = t({0, 0, 0, 0, 0, 0, 0, 0}, 8, 2, 1);           // 声明 8 但需 2×1×8=16
    kps[0] = t(std::vector<float>(20, 0.0f), 20, 2, 1);       // 声明 40 但需 40

    ScrfdDecodeOptions o = opt100();
    o.mode = DecodeMode::kDistance;
    o.numAnchors[0] = 2;
    std::string err;
    auto dets = ScrfdDecoder::decode(score, bbox, kps, o, err);
    EXPECT_TRUE(dets.empty());
    EXPECT_FALSE(err.empty());
}