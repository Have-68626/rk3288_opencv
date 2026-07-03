#include <iostream>
#include <string>

namespace rk_win_test {

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

}  // namespace rk_win_test

bool test_registry_dispatch_returns_200_for_matching_route();
bool test_registry_returns_405_for_method_mismatch();
bool test_lbph_embedder_dim_and_distance();
bool test_face_metrics_confusion_matrix();
bool test_bootstrap_returns_models_on_valid_config();
bool test_bootstrap_reports_failure_on_bad_cascade();
// test_http_faces_server_path_validation 已在 core_unit_tests 注册,
// 此处不重复注册, 避免跨测试套件重复执行。

int main() {
    using namespace rk_win_test;
    const TestCase cases[] = {
        {"lbph_embedder_dim_and_distance", test_lbph_embedder_dim_and_distance},
        {"face_metrics_confusion_matrix", test_face_metrics_confusion_matrix},
        {"registry_dispatch_returns_200", test_registry_dispatch_returns_200_for_matching_route},
        {"registry_returns_405_for_method_mismatch", test_registry_returns_405_for_method_mismatch},
        {"test_bootstrap_returns_models_on_valid_config", test_bootstrap_returns_models_on_valid_config},
        {"test_bootstrap_reports_failure_on_bad_cascade", test_bootstrap_reports_failure_on_bad_cascade},
    };
    const bool ok = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    return ok ? 0 : 1;
}
