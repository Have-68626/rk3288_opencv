#include <gtest/gtest.h>
#include "MotionDetector.h"

TEST(MotionDetectorTest, DetectEmptyFrameReturnsFalse) {
    MotionDetector detector;
    // 空帧不应检测到运动
    EXPECT_FALSE(detector.detect(cv::Mat()));
}
