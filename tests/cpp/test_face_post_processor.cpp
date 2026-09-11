#include <gtest/gtest.h>

#include <limits>

#include "pipeline/FacePostProcessor.h"

using namespace pipeline;

namespace {

DetectedFace makeFace(int x, int y, int w, int h, float conf) {
    DetectedFace f;
    f.bbox = cv::Rect(x, y, w, h);
    f.confidence = conf;
    return f;
}

}  // namespace

// 同一脸两个高重叠框只保留一个（最高分）
TEST(FacePostProcessorTest, OverlappingDetectionKeepsHighestConfidence) {
    std::vector<DetectedFace> in = {
        makeFace(10, 10, 200, 200, 0.80f),
        makeFace(14, 12, 200, 200, 0.90f),  // 高重叠 + 更高分
    };
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].confidence, 0.90f);
}

// 主脸内部的局部框（分数不更高）被包含框抑制删除
TEST(FacePostProcessorTest, InnerLocalBoxRemovedWhenNotHigherScore) {
    std::vector<DetectedFace> in = {
        makeFace(100, 120, 200, 240, 0.92f),  // 大脸主框
        makeFace(150, 160, 60, 70, 0.80f),    // 内部局部框，面积更小
    };
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].bbox.x, 100);
    EXPECT_EQ(out[0].bbox.width, 200);
}

// 相邻两张真实人脸（不重叠）不会被合并
TEST(FacePostProcessorTest, AdjacentRealFacesNotMerged) {
    std::vector<DetectedFace> in = {
        makeFace(10, 10, 100, 100, 0.90f),
        makeFace(150, 10, 100, 100, 0.88f),
    };
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480);
    ASSERT_EQ(out.size(), 2u);
}

// NaN 置信度/负尺寸/完全越界框被稳定丢弃
TEST(FacePostProcessorTest, InvalidFramesDroppedStably) {
    DetectedFace nanFace = makeFace(0, 0, 50, 50, 0.9f);
    nanFace.confidence = std::numeric_limits<float>::quiet_NaN();
    std::vector<DetectedFace> in = {
        nanFace,
        makeFace(0, 0, -10, 50, 0.8f),   // 负宽
        makeFace(-50, -50, 20, 20, 0.7f), // 完全在帧外（与 640x480 无交集）
        makeFace(100, 100, 40, 40, 0.6f), // 正常
    };
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].confidence, 0.6f);
}

// 相同分数按原始索引顺序稳定输出
TEST(FacePostProcessorTest, EqualScoresKeepStableOrder) {
    std::vector<DetectedFace> in = {
        makeFace(10, 10, 50, 50, 0.80f),
        makeFace(200, 10, 50, 50, 0.80f),
    };
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].bbox.x, 10);
    EXPECT_EQ(out[1].bbox.x, 200);
}

// maxFaces 限制输出数量（保留最高分者）
TEST(FacePostProcessorTest, MaxFacesTruncates) {
    FacePostProcessOptions opt;
    opt.maxFaces = 1;
    std::vector<DetectedFace> in = {
        makeFace(10, 10, 50, 50, 0.6f),
        makeFace(90, 10, 50, 50, 0.95f),
        makeFace(170, 10, 50, 50, 0.75f),
    };
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480, opt);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].confidence, 0.95f);
}

// 越界部分裁剪到画面边界；完全在外则丢弃
TEST(FacePostProcessorTest, ClipToFrameBounds) {
    std::vector<DetectedFace> in = {
        makeFace(600, 460, 200, 200, 0.9f),  // 部分越界（右下）
        makeFace(0, 0, 30, 80, 0.5f),        // 正常左上
    };
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480);
    ASSERT_EQ(out.size(), 2u);
    // 部分越界框被裁剪到画面边界：600..799 → 640、460..659 → 480
    EXPECT_EQ(out[0].bbox.x, 600);
    EXPECT_EQ(out[0].bbox.y, 460);
    EXPECT_EQ(out[0].bbox.width, 40);
    EXPECT_EQ(out[0].bbox.height, 20);
}

// 内部小框分数更高时应幸存（包含抑制只在"分数不更高"时删小框）
TEST(FacePostProcessorTest, InnerHighScoreBoxSurvives) {
    std::vector<DetectedFace> in = {
        makeFace(0, 0, 200, 200, 0.70f),  // 大脸主框（低分）
        makeFace(50, 50, 60, 60, 0.95f),  // 内部小框（更高分）
    };
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].confidence, 0.95f);
}

// 回归：中间夹无效框会拉开原始索引与紧凑下标，若用原始索引去索引紧凑数组即越界读。
// 用例 [有效, 无效(NaN), 有效, 有效]：修复前 order={0,2,3} 而 clippedFaces 仅 3 项，
// 排序/NMS/包含抑制中 clippedFaces[order[...]] 会读到下标 3（越界，Debug 下断言/Release 下 UB）。
TEST(FacePostProcessorTest, InvalidFaceInMiddlePreservesOriginalIndexMapping) {
    DetectedFace invalid = makeFace(0, 0, 40, 40, 0.99f);
    invalid.confidence = std::numeric_limits<float>::quiet_NaN();  // 无效：被丢弃
    std::vector<DetectedFace> in = {
        makeFace(10, 10, 100, 100, 0.60f),   // 原始索引 0，有效
        invalid,                              // 原始索引 1，无效（NaN）
        makeFace(200, 10, 100, 100, 0.90f),  // 原始索引 2，有效
        makeFace(400, 10, 100, 100, 0.80f),  // 原始索引 3，有效
    };

    // keptIndices 必须返回原始 faces 索引：全部落在 [0, faces.size()) 内且映射到正确检测
    std::vector<std::size_t> idx = FacePostProcessor::keptIndices(in, 640, 480);
    ASSERT_EQ(idx.size(), 3u);
    for (std::size_t k : idx) {
        ASSERT_LT(k, in.size());
    }
    EXPECT_EQ(in[idx[0]].confidence, 0.90f);
    EXPECT_EQ(in[idx[1]].confidence, 0.80f);
    EXPECT_EQ(in[idx[2]].confidence, 0.60f);

    // process 输出：无效框被丢弃，三个有效框按置信度降序稳定输出，且能正确回映到原始 bbox
    std::vector<DetectedFace> out = FacePostProcessor::process(in, 640, 480);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_FLOAT_EQ(out[0].confidence, 0.90f);
    EXPECT_FLOAT_EQ(out[1].confidence, 0.80f);
    EXPECT_FLOAT_EQ(out[2].confidence, 0.60f);
    EXPECT_EQ(out[0].bbox.x, 200);
    EXPECT_EQ(out[1].bbox.x, 400);
    EXPECT_EQ(out[2].bbox.x, 10);
}