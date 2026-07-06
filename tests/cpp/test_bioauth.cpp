#include <gtest/gtest.h>
#include "BioAuth.h"

TEST(BioAuthTest, InitializeWithBadPathReturnsFalse) {
    BioAuth auth;
    // 空路径无法加载 cascade 文件 → 应返回 false
    EXPECT_FALSE(auth.initialize("", ""));
}

TEST(BioAuthTest, SetFaceSelectModeDoesNotCrash) {
    BioAuth auth;
    // 验证 setFaceSelectMode 可正常调用且不崩溃
    auth.setFaceSelectMode(BioAuth::FaceSelectMode::MAIN_FACE);
    auth.setFaceSelectMode(BioAuth::FaceSelectMode::MULTI_FACES);
    SUCCEED();
}
