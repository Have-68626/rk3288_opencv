/**
 * @file EventManager.h
 * @brief Manages system events and reporting.
 * 
 * Handles the creation of structured reports for abnormal events
 * and interfaces with the Storage module for persistence.
 */
#pragma once

#include "Types.h"
#include <string>

namespace rk_core {

// ── FACE-CLEAN-021：测试接缝**不得**把 googletest 拖进生产编译单元 ──
// `tests/cpp/test_event_manager.cpp` 的 `TEST(EventManager, UniqueId)` 需要访问 private
// `generateUniqueId()`。此处**刻意不使用 googletest 的 `FRIEND_TEST` 宏**——该宏定义在
// `<gtest/gtest_prod.h>`，而本头文件经 `Engine.h` 被**所有编译 Engine 的 target**包含，
// 于是每个生产 target 都被迫具备 googletest 包含路径；而生产 CLI `rk3288_cli` 并没有这条路径：
//   · 缺陷证据：2026-09-18 一次针对 `rk3288_cli` 的本地构建报
//     `EventManager.h(14): error C1083: 无法打开 "gtest/gtest_prod.h"`；本次又以**等价探针**
//     （同一 include 集合、不含 googletest 路径）独立复现出**逐字相同**的 `C1083`。
//   · 为何长期静默：CI 的 windows job 目标清单（core_unit_tests / win_unit_tests /
//     face_infer_unit_tests / core_gtest_tests / win_replay_export_cli）**不含 rk3288_cli**
//     ⇒ 没有任何一步真的编译它（见 FACE-CLEAN-021 台账）。
//   · 修复后已实测：`rk3288_cli` 构建成功（`-> bin\Release\rk3288_cli.exe`，退出码 0）。
//
// 改为等价的前向声明 + `friend`。等价性**有据、非推测**：
//   · `FRIEND_TEST(a, b)` 在 gtest 1.12.1（本仓 FetchContent 钉版）中**逐字**展开为
//     `friend class a##_##b##_Test`（`googletest/include/gtest/gtest_prod.h`）；
//   · `TEST(a, b)` 生成的类名同为 `a##_##b##_Test`，且两者必须在**同一命名空间**
//     （gtest_prod.h 自注：「The test class must be in the same namespace as the class
//     being tested」）——本仓的 `TEST` 正写在 `namespace rk_core` 内。
// ⇒ 下方那行 `friend class EventManager_UniqueId_Test;` 与旧宏的展开结果**完全相同**。
//   若将来 gtest 改了生成类名，测试会**编译失败**（私有成员访问被拒）——响亮失败，不静默。
class EventManager_UniqueId_Test;

class EventManager {
public:
    EventManager();
    
    /**
     * @brief Logs an event to the system.
     * @param type Event category (e.g. "AUTH_FAIL").
     * @param description Detail message.
     * @param snapshotPath Optional path to image evidence.
     */
    void logEvent(const std::string& type, const std::string& description, const std::string& snapshotPath = "");

    /**
     * @brief Generates a JSON formatted report of the event.
     * @param event The event object.
     * @return JSON string.
     */
    std::string formatEventJson(const AppEvent& event);

private:
    // 等价于 `FRIEND_TEST(EventManager, UniqueId)`，但不引入 googletest 依赖（理由见文件头）。
    friend class EventManager_UniqueId_Test;

    std::string generateUniqueId();
};

} // namespace rk_core
