#include <gtest/gtest.h>
#include "BioAuth.h"

TEST(BioAuthTest, CreateInstance) {
    BioAuth auth;
    SUCCEED();
}

TEST(BioAuthTest, SetFaceSelectMode) {
    BioAuth auth;
    // 验证 setFaceSelectMode 可正常调用且不崩溃
    auth.setFaceSelectMode(BioAuth::FaceSelectMode::MAIN_FACE);
    auth.setFaceSelectMode(BioAuth::FaceSelectMode::MULTI_FACES);
    SUCCEED();
}
