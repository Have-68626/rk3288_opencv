#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include "FrameTransform.h"

using namespace rk_core;

namespace {

// 非对称图案：左半蓝(B)、右半红(R)，恒无边对称性，可检出镜像回归
cv::Mat makeAsymMat(int w, int h) {
    cv::Mat m(h, w, CV_8UC3, cv::Scalar(0, 0, 255));  // 右半红
    m(cv::Rect(0, 0, w / 2, h)).setTo(cv::Scalar(255, 0, 0));  // 左半蓝
    return m;
}

// ① 变换不修改输入（沿用 applyFlip 回归语义，覆盖 XOR 配置路径）
TEST(FrameTransformTest, TransformDoesNotMutateInput) {
    cv::Mat in = makeAsymMat(320, 200);
    cv::Mat before = in.clone();
    FrameTransformConfig cfg;
    cfg.target_mirrored_x = true;  // input=false, target=true -> 水平翻转
    cv::Mat out = FrameTransform::applyImage(in, cfg);
    const cv::Mat diffMask = before != in;
    EXPECT_EQ(cv::countNonZero(diffMask.reshape(1)), 0);
    // 输出确为水平镜像：左上像素 == 输入右上像素
    const cv::Vec3b left = out.at<cv::Vec3b>(0, 0);
    const cv::Vec3b right = in.at<cv::Vec3b>(0, in.cols - 1);
    EXPECT_EQ(left[0], right[0]);
    EXPECT_EQ(left[1], right[1]);
    EXPECT_EQ(left[2], right[2]);
}

// ② 前摄默认镜像：input=false(源未镜像) 且 target=true(用户开镜像) -> 实际翻转
TEST(FrameTransformTest, FrontCameraDefaultsToMirror) {
    FrameTransformConfig cfg;
    cfg.input_mirrored_x = false;
    cfg.target_mirrored_x = true;   // 前置默认 target=true
    EXPECT_TRUE(FrameTransform::effectiveFlipX(cfg));
}

// ③ XOR 抵消：源已镜像 + 目标镜像 -> 不再额外翻转
TEST(FrameTransformTest, XorCancelWhenSourceAlreadyMirrored) {
    FrameTransformConfig cfg;
    cfg.input_mirrored_x = true;
    cfg.target_mirrored_x = true;
    EXPECT_FALSE(FrameTransform::effectiveFlipX(cfg));
    cv::Mat in = makeAsymMat(320, 200);
    cv::Mat out = FrameTransform::applyImage(in, cfg);
    EXPECT_TRUE(out.data == in.data) << " 无实际翻转时应返回输入引用（零拷贝）";
}

// ④ 无翻转时返回输入引用（保持零拷贝路径）
TEST(FrameTransformTest, NoFlipReturnsInputAlias) {
    cv::Mat in = makeAsymMat(320, 200);
    FrameTransformConfig cfg;
    cfg.rotation_degrees = 0;
    cv::Mat out = FrameTransform::applyImage(in, cfg);
    EXPECT_TRUE(out.data == in.data);
}

// ⑤ generation 仅配置变化递增：与检测结果无关，同类配置视为未变化
TEST(FrameTransformTest, GenerationChangesOnlyOnConfiguration) {
    FrameTransformConfig a;
    a.input_mirrored_x = true;
    a.target_mirrored_x = true;
    a.flip_y = false;
    a.rotation_degrees = 0;
    FrameTransformConfig b = a;
    b.generation = 42;   // 仅 generation 不同
    EXPECT_TRUE(FrameTransform::configUnchanged(a, b));
    // 任一变换字段变化都视为配置变化
    b = a; b.target_mirrored_x = false;
    EXPECT_FALSE(FrameTransform::configUnchanged(a, b));
    b = a; b.input_mirrored_x = false;
    EXPECT_FALSE(FrameTransform::configUnchanged(a, b));
    b = a; b.flip_y = true;
    EXPECT_FALSE(FrameTransform::configUnchanged(a, b));
    b = a; b.rotation_degrees = 90;
    EXPECT_FALSE(FrameTransform::configUnchanged(a, b));
}

// ⑥ 像素/关键点/框使用同一变换：图像变换后的某像素 == 原坐标经 mapPoint 得到的输出坐标
TEST(FrameTransformTest, PixelsBoxesAndLandmarksUseSameTransform) {
    const int w = 320, h = 200;
    cv::Mat in = makeAsymMat(w, h);
    FrameTransformConfig cfg;
    cfg.target_mirrored_x = true;   // 水平翻转
    cv::Mat out = FrameTransform::applyImage(in, cfg);
    // 原(0,0) 左上(蓝) 经 mapPoint 应落在输出 (w-1,0)
    cv::Point2f pt = FrameTransform::mapPoint({0.f, 0.f}, cfg, w, h);
    ASSERT_EQ(pt.x, static_cast<float>(w - 1));
    ASSERT_EQ(pt.y, 0.f);
    const cv::Vec3b p0 = out.at<cv::Vec3b>(static_cast<int>(pt.y), static_cast<int>(pt.x));
    const cv::Vec3b src = in.at<cv::Vec3b>(0, 0);
    EXPECT_EQ(p0[0], src[0]);
    EXPECT_EQ(p0[1], src[1]);
    EXPECT_EQ(p0[2], src[2]);
}

// ⑦ 旋转也在同一变换内：rot=90 时 applyImage 输出尺寸互换并把 (0,0) 映射到 (inH-1,0)
TEST(FrameTransformTest, RotationUsesSameTransform) {
    const int w = 3, h = 2;
    cv::Mat in(h, w, CV_8UC3, cv::Scalar(0, 0, 0));
    in.at<cv::Vec3b>(0, 0) = cv::Vec3b(255, 255, 255);  // 左上标记
    in.at<cv::Vec3b>(1, w - 1) = cv::Vec3b(0, 255, 0);  // 右下标记
    FrameTransformConfig cfg;
    cfg.rotation_degrees = 90;
    cv::Mat out = FrameTransform::applyImage(in, cfg);
    ASSERT_EQ(out.cols, h);
    ASSERT_EQ(out.rows, w);
    cv::Point2f topLeft = FrameTransform::mapPoint({0.f, 0.f}, cfg, w, h);
    ASSERT_EQ(topLeft.x, static_cast<float>(h - 1));
    ASSERT_EQ(topLeft.y, 0.f);
    const cv::Vec3b atMark = out.at<cv::Vec3b>(static_cast<int>(topLeft.y), static_cast<int>(topLeft.x));
    EXPECT_EQ(atMark[0], 255);
    EXPECT_EQ(atMark[1], 255);
    EXPECT_EQ(atMark[2], 255);
}

// ⑧ XOR 对称组合：源已镜像但目标不镜像 -> 仍需翻转
TEST(FrameTransformTest, MirrorWhenSourceMirroredButTargetNot) {
    FrameTransformConfig cfg;
    cfg.input_mirrored_x = true;
    cfg.target_mirrored_x = false;
    EXPECT_TRUE(FrameTransform::effectiveFlipX(cfg));
    cv::Mat in = makeAsymMat(320, 200);
    cv::Mat out = FrameTransform::applyImage(in, cfg);
    const cv::Vec3b left = out.at<cv::Vec3b>(0, 0);
    const cv::Vec3b srcRight = in.at<cv::Vec3b>(0, in.cols - 1);
    EXPECT_EQ(left[0], srcRight[0]);
    EXPECT_EQ(left[2], srcRight[2]);
}

// ⑨ 180° 旋转同一变换
TEST(FrameTransformTest, Rotation180UsesSameTransform) {
    const int w = 3, h = 2;
    cv::Mat in(h, w, CV_8UC3, cv::Scalar(0, 0, 0));
    in.at<cv::Vec3b>(0, 0) = cv::Vec3b(255, 255, 255);  // 左上标记
    FrameTransformConfig cfg;
    cfg.rotation_degrees = 180;
    cv::Mat out = FrameTransform::applyImage(in, cfg);
    ASSERT_EQ(out.cols, w);
    ASSERT_EQ(out.rows, h);
    cv::Point2f pt = FrameTransform::mapPoint({0.f, 0.f}, cfg, w, h);
    ASSERT_EQ(pt.x, static_cast<float>(w - 1));
    ASSERT_EQ(pt.y, static_cast<float>(h - 1));
    const cv::Vec3b at = out.at<cv::Vec3b>(static_cast<int>(pt.y), static_cast<int>(pt.x));
    EXPECT_EQ(at[0], 255);
}

// ⑩ 270° + 水平翻转组合同一变换
TEST(FrameTransformTest, Rotation270CombinedWithFlipUsesSameTransform) {
    const int w = 3, h = 2;
    cv::Mat in(h, w, CV_8UC3, cv::Scalar(0, 0, 0));
    in.at<cv::Vec3b>(0, 0) = cv::Vec3b(255, 255, 255);  // 左上标记
    FrameTransformConfig cfg;
    cfg.rotation_degrees = 270;
    cfg.target_mirrored_x = true;
    cv::Mat out = FrameTransform::applyImage(in, cfg);
    ASSERT_EQ(out.cols, h);
    ASSERT_EQ(out.rows, w);
    cv::Point2f pt = FrameTransform::mapPoint({0.f, 0.f}, cfg, w, h);
    ASSERT_EQ(pt.x, static_cast<float>(h - 1));
    ASSERT_EQ(pt.y, static_cast<float>(w - 1));
    const cv::Vec3b at = out.at<cv::Vec3b>(static_cast<int>(pt.y), static_cast<int>(pt.x));
    EXPECT_EQ(at[0], 255);
    EXPECT_EQ(at[1], 255);
    EXPECT_EQ(at[2], 255);
}

// ⑪ mapRect 与水平翻转后的外接框一致
// cv::Rect 为半开区间，实际像素范围为 [r.x, r.x+r.width-1]；水平翻转后
// 最左输出像素 = (w-1) - (r.x+r.width-1) = w - (r.x+r.width)，故外接框 x 取该值。
TEST(FrameTransformTest, MapRectMatchesHorizontalMirror) {
    FrameTransformConfig cfg;
    cfg.target_mirrored_x = true;
    const int w = 320, h = 200;
    cv::Rect r(10, 20, 30, 40);
    cv::Rect mapped = FrameTransform::mapRect(r, cfg, w, h);
    EXPECT_EQ(mapped.x, w - (r.x + r.width));
    EXPECT_EQ(mapped.y, r.y);
    EXPECT_EQ(mapped.width, r.width);
    EXPECT_EQ(mapped.height, r.height);
}

// ⑫ XOR 真值表（A1 镜像契约）：input=帧实际已镜像，target=最终希望镜像，
// 实际水平翻转 = input XOR target。四格：00→不翻、01→翻、10→翻、11→不翻。
// 既有用例已分散覆盖各格，此处把四格**显式穷尽**在一处，作为契约的可执行定义。
TEST(FrameTransformTest, EffectiveFlipXIsInputXorTarget) {
    struct Case { bool in; bool tgt; bool expect; };
    const Case cases[] = {
        {false, false, false},  // 00：都不镜像 → 不翻
        {false, true,  true },  // 01：仅目标镜像 → 翻
        {true,  false, true },  // 10：源已镜像、目标不镜像 → 翻
        {true,  true,  false},  // 11：源已镜像 + 目标镜像 → XOR 抵消
    };
    for (const auto& c : cases) {
        FrameTransformConfig cfg;
        cfg.input_mirrored_x = c.in;
        cfg.target_mirrored_x = c.tgt;
        EXPECT_EQ(FrameTransform::effectiveFlipX(cfg), c.expect)
            << "input=" << c.in << " target=" << c.tgt;
    }
}

// ⑬ 四格 XOR 下「像素 / 框 / 关键点」共用同一变换（A1 的真实缺口）：
//    把非对称标记放在非边界位置，applyImage 后 mapRect/mapPoint 必须把它映射到**同一位置**
//    （映射框内的每个像素都是标记；标记中心经 mapPoint 落在框内且仍是标记）。跨全部四格验证。
TEST(FrameTransformTest, MappedBoxAndKeypointsAgreeWithImageUnderAllXorCombos) {
    const int w = 40, h = 30;
    const cv::Rect marker(6, 5, 8, 9);  // 非对称位置（靠左上但非贴边）
    struct Case { bool in; bool tgt; };
    const Case cases[] = {{false, false}, {false, true}, {true, false}, {true, true}};
    for (const auto& c : cases) {
        FrameTransformConfig cfg;
        cfg.input_mirrored_x = c.in;
        cfg.target_mirrored_x = c.tgt;
        cv::Mat in(h, w, CV_8UC3, cv::Scalar(0, 0, 0));
        in(marker).setTo(cv::Scalar(255, 255, 255));
        cv::Mat out = FrameTransform::applyImage(in, cfg);

        const cv::Rect mapped = FrameTransform::mapRect(marker, cfg, w, h);
        ASSERT_EQ(mapped.width, marker.width);
        ASSERT_EQ(mapped.height, marker.height);
        ASSERT_GE(mapped.x, 0) << "in=" << c.in << " tgt=" << c.tgt;
        ASSERT_GE(mapped.y, 0);
        ASSERT_LE(mapped.x + mapped.width, out.cols);
        ASSERT_LE(mapped.y + mapped.height, out.rows);

        // 映射框内每个像素都应是标记（白）
        for (int y = mapped.y; y < mapped.y + mapped.height; ++y) {
            for (int x = mapped.x; x < mapped.x + mapped.width; ++x) {
                EXPECT_EQ(out.at<cv::Vec3b>(y, x)[0], 255)
                    << "标记未落在 mapRect 映射框内 in=" << c.in << " tgt=" << c.tgt
                    << " @(" << x << "," << y << ")";
            }
        }
        // 关键点：标记中心经 mapPoint 应落在映射框内且仍是标记
        const cv::Point2f pc = FrameTransform::mapPoint(
            cv::Point2f(marker.x + marker.width / 2.f, marker.y + marker.height / 2.f),
            cfg, w, h);
        EXPECT_GE(pc.x, mapped.x);
        EXPECT_LE(pc.x, mapped.x + mapped.width - 1);
        EXPECT_GE(pc.y, mapped.y);
        EXPECT_LE(pc.y, mapped.y + mapped.height - 1);
        EXPECT_EQ(out.at<cv::Vec3b>(static_cast<int>(pc.y), static_cast<int>(pc.x))[0], 255)
            << "关键点 mapPoint 未落在标记上 in=" << c.in << " tgt=" << c.tgt;
    }
}

}  // namespace