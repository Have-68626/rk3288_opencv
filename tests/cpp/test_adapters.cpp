#include <gtest/gtest.h>
#include "ModelRegistry.h"

using namespace rk_core;

TEST(AdapterTest, ModelRegistryInstance) {
    auto& registry = ModelRegistry::instance();
    // 单例必须始终有效
    EXPECT_NE(&registry, nullptr);
}

TEST(AdapterTest, ModelRegistryListAllEmpty) {
    auto& registry = ModelRegistry::instance();
    // 未注册任何模型时，listAll 应返回空列表
    auto all = registry.listAll();
    EXPECT_TRUE(all.empty());
}
