#include <gtest/gtest.h>
#include "ModelRegistry.h"

TEST(AdapterTest, ModelRegistryInstance) {
    auto& registry = ModelRegistry::instance();
    SUCCEED();
}

TEST(AdapterTest, ModelRegistryListAllEmpty) {
    auto& registry = ModelRegistry::instance();
    // 未注册任何模型时，listAll 应返回空列表
    auto all = registry.listAll();
    EXPECT_TRUE(all.empty());
}
