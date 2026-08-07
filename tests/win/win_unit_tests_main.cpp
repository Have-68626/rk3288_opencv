#include <gtest/gtest.h>
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
bool test_registry_dispatch_returns_200_for_matching_route();
bool test_registry_returns_405_for_method_mismatch();
bool test_pipeline_concurrent_reload_no_uaf();
bool test_pipeline_switch_camera_switches_source();
bool test_pipeline_switch_camera_failure_keeps_old();
bool test_pipeline_render_state_concurrent_access();
bool test_config_put_bumps_generation();
bool test_config_external_write_bumps_generation_once();
bool test_config_put_and_external_each_consumed_once();
// test_http_faces_server_path_validation 已在 core_unit_tests 注册,
// 此处不重复注册, 避免跨测试套件重复执行。

// 接通两个 runner：自定义 bool 测试（runAll）+ GoogleTest 测试（RUN_ALL_TESTS）。
// 任一失败均返回非零退出码，使 CTest 失败；全过返回 0。
int main(int argc, char** argv) {
    using namespace rk_win_test;
    const TestCase cases[] = {
        {"lbph_embedder_dim_and_distance", test_lbph_embedder_dim_and_distance},
        {"face_metrics_confusion_matrix", test_face_metrics_confusion_matrix},
        {"registry_dispatch_returns_200", test_registry_dispatch_returns_200_for_matching_route},
        {"registry_returns_405_for_method_mismatch", test_registry_returns_405_for_method_mismatch},
        {"test_bootstrap_returns_models_on_valid_config", test_bootstrap_returns_models_on_valid_config},
        {"test_bootstrap_reports_failure_on_bad_cascade", test_bootstrap_reports_failure_on_bad_cascade},
        {"pipeline_concurrent_reload_no_uaf", test_pipeline_concurrent_reload_no_uaf},
        {"pipeline_switch_camera_switches_source", test_pipeline_switch_camera_switches_source},
        {"pipeline_switch_camera_failure_keeps_old", test_pipeline_switch_camera_failure_keeps_old},
        {"pipeline_render_state_concurrent_access", test_pipeline_render_state_concurrent_access},
        {"config_put_bumps_generation", test_config_put_bumps_generation},
        {"config_external_write_bumps_generation_once", test_config_external_write_bumps_generation_once},
        {"config_put_and_external_each_consumed_once", test_config_put_and_external_each_consumed_once},
    };
    const bool boolOk = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    ::testing::InitGoogleTest(&argc, argv);
    const int gtestOk = RUN_ALL_TESTS();
    return (boolOk && gtestOk == 0) ? 0 : 1;
}
