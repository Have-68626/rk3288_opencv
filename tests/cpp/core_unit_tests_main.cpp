#include <iostream>

namespace rk_core_test {

using TestFn = bool (*)();

struct TestCase {
    const char* name;
    TestFn fn;
};

bool runAll(const TestCase* cases, int n) {
    int pass = 0;
    int fail = 0;
    for (int i = 0; i < n; i++) {
        const bool ok = cases[i].fn();
        if (ok) {
            pass++;
            std::cout << "TEST_PASS name=" << cases[i].name << std::endl;
        } else {
            fail++;
            std::cout << "TEST_FAIL name=" << cases[i].name << std::endl;
        }
    }
    std::cout << "TEST_SUMMARY pass=" << pass << " fail=" << fail << " total=" << (pass + fail) << std::endl;
    return fail == 0;
}

}  // namespace rk_core_test

bool test_face_search_stable_topk();
bool test_frame_input_latest_only_keeps_newest();
bool test_frame_input_bounded_queue_drops_oldest();
bool test_threshold_policy_version_and_consecutive();
bool test_threshold_policy_rollback_empty_history();
bool test_event_manager_format_json();
bool test_event_manager_unique_id();

int main() {
    using namespace rk_core_test;
    const TestCase cases[] = {
        {"face_search_stable_topk", test_face_search_stable_topk},
        {"frame_input_latest_only_keeps_newest", test_frame_input_latest_only_keeps_newest},
        {"frame_input_bounded_queue_drops_oldest", test_frame_input_bounded_queue_drops_oldest},
        {"threshold_policy_version_and_consecutive", test_threshold_policy_version_and_consecutive},
        {"threshold_policy_rollback_empty_history", test_threshold_policy_rollback_empty_history},
        {"event_manager_format_json", test_event_manager_format_json},
        {"event_manager_unique_id", test_event_manager_unique_id},
    };
    const bool ok = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    return ok ? 0 : 1;
}

