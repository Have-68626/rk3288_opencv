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
bool test_bootstrap_arcface_snapshot_hash_uses_cascade_path();
bool test_recognition_config_node_names_default_empty();
bool test_recognition_config_arcface_threshold_in_cosine_range();
bool test_registry_dispatch_returns_200_for_matching_route();
bool test_registry_returns_405_for_method_mismatch();
bool test_pipeline_concurrent_reload_no_uaf();
bool test_pipeline_switch_camera_switches_source();
bool test_pipeline_switch_camera_failure_keeps_old();
bool test_pipeline_render_state_concurrent_access();
bool test_pipeline_first_frame_delayed();
bool test_pipeline_first_frame_timeout();
bool test_pipeline_open_failure();
bool test_pipeline_enroll_writeback_does_not_overwrite_new_request();
bool test_pipeline_reload_reruns_manifest_check();
bool test_config_put_bumps_generation();
bool test_config_external_write_bumps_generation_once();
bool test_config_put_and_external_each_consumed_once();
bool test_config_default_serialization_passes_schema();
bool test_config_each_single_field_put_passes_schema();
bool test_config_roundtrip_and_schema_alignment();
bool test_config_masked_echo_preserves_real_value();
bool test_config_missing_arcface_threshold_key_migrates();
bool test_config_arcface_threshold_out_of_range_rejected();
bool test_config_rejected_load_quarantines_and_degrades();
bool test_config_parse_failure_quarantines_and_degrades();
bool test_config_healthy_load_not_quarantined();
bool test_config_quarantine_failure_preserves_original();
bool test_config_quarantine_move_and_name();
bool test_config_quarantine_timestamp_collision_preserves_original();
bool test_config_quarantine_retry_succeeds_after_collisions();
bool test_config_reload_rejected_no_bak_rewrites_from_memory();
bool test_config_reload_rejected_rewrites_memory_not_stale_bak();
bool test_config_reload_rejected_quarantines_and_not_applied();
bool test_config_reload_rejected_key_unavailable_bak_unusable();
bool test_config_reload_rejected_key_unavailable_no_bak_no_selfheal();
bool test_config_cascade_migration_write_failure_degrades();
bool test_config_missing_key_rejects_default_write();
bool test_config_second_initialize_resets_diagnostics();
bool test_config_unknown_key_rejected();
bool test_config_load_failure_writes_stderr();
bool test_config_healthy_load_silent_stderr();
bool test_reload_endpoint_bumps_generation();
bool test_openapi_endpoint_returns_spec_json();
bool test_face_db_plaintext_vuln_documented();
bool test_face_db_encrypted_no_plaintext();
bool test_face_db_wrong_key_controlled_failure();
bool test_face_db_migration_encrypts_legacy();
bool test_face_db_migration_failure_preserves_legacy();
bool test_face_db_encrypted_roundtrip();
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
        {"test_bootstrap_arcface_snapshot_hash_uses_cascade_path", test_bootstrap_arcface_snapshot_hash_uses_cascade_path},
        {"test_recognition_config_node_names_default_empty", test_recognition_config_node_names_default_empty},
        {"test_recognition_config_arcface_threshold_in_cosine_range", test_recognition_config_arcface_threshold_in_cosine_range},
        {"pipeline_concurrent_reload_no_uaf", test_pipeline_concurrent_reload_no_uaf},
        {"pipeline_switch_camera_switches_source", test_pipeline_switch_camera_switches_source},
        {"pipeline_switch_camera_failure_keeps_old", test_pipeline_switch_camera_failure_keeps_old},
        {"pipeline_render_state_concurrent_access", test_pipeline_render_state_concurrent_access},
        {"pipeline_first_frame_delayed", test_pipeline_first_frame_delayed},
        {"pipeline_first_frame_timeout", test_pipeline_first_frame_timeout},
        {"pipeline_open_failure", test_pipeline_open_failure},
        {"pipeline_enroll_writeback_does_not_overwrite_new_request", test_pipeline_enroll_writeback_does_not_overwrite_new_request},
        {"pipeline_reload_reruns_manifest_check", test_pipeline_reload_reruns_manifest_check},
        {"config_put_bumps_generation", test_config_put_bumps_generation},
        {"config_external_write_bumps_generation_once", test_config_external_write_bumps_generation_once},
        {"config_put_and_external_each_consumed_once", test_config_put_and_external_each_consumed_once},
        {"config_default_serialization_passes_schema", test_config_default_serialization_passes_schema},
        {"config_each_single_field_put_passes_schema", test_config_each_single_field_put_passes_schema},
        {"config_roundtrip_and_schema_alignment", test_config_roundtrip_and_schema_alignment},
        {"config_masked_echo_preserves_real_value", test_config_masked_echo_preserves_real_value},
        {"config_missing_arcface_threshold_key_migrates", test_config_missing_arcface_threshold_key_migrates},
        {"config_arcface_threshold_out_of_range_rejected", test_config_arcface_threshold_out_of_range_rejected},
        {"config_rejected_load_quarantines_and_degrades", test_config_rejected_load_quarantines_and_degrades},
        {"config_parse_failure_quarantines_and_degrades", test_config_parse_failure_quarantines_and_degrades},
        {"config_healthy_load_not_quarantined", test_config_healthy_load_not_quarantined},
        {"config_quarantine_failure_preserves_original", test_config_quarantine_failure_preserves_original},
        {"config_quarantine_move_and_name", test_config_quarantine_move_and_name},
        {"config_quarantine_timestamp_collision_preserves_original", test_config_quarantine_timestamp_collision_preserves_original},
        {"config_quarantine_retry_succeeds_after_collisions", test_config_quarantine_retry_succeeds_after_collisions},
        {"config_reload_rejected_no_bak_rewrites_from_memory", test_config_reload_rejected_no_bak_rewrites_from_memory},
        {"config_reload_rejected_rewrites_memory_not_stale_bak", test_config_reload_rejected_rewrites_memory_not_stale_bak},
        {"config_reload_rejected_quarantines_and_not_applied", test_config_reload_rejected_quarantines_and_not_applied},
        {"config_reload_rejected_key_unavailable_bak_unusable", test_config_reload_rejected_key_unavailable_bak_unusable},
        {"config_reload_rejected_key_unavailable_no_bak_no_selfheal", test_config_reload_rejected_key_unavailable_no_bak_no_selfheal},
        {"config_cascade_migration_write_failure_degrades", test_config_cascade_migration_write_failure_degrades},
        {"config_missing_key_rejects_default_write", test_config_missing_key_rejects_default_write},
        {"config_second_initialize_resets_diagnostics", test_config_second_initialize_resets_diagnostics},
        {"config_unknown_key_rejected", test_config_unknown_key_rejected},
        {"config_load_failure_writes_stderr", test_config_load_failure_writes_stderr},
        {"config_healthy_load_silent_stderr", test_config_healthy_load_silent_stderr},
        {"reload_endpoint_bumps_generation", test_reload_endpoint_bumps_generation},
        {"openapi_endpoint_returns_spec_json", test_openapi_endpoint_returns_spec_json},
        {"face_db_plaintext_vuln_documented", test_face_db_plaintext_vuln_documented},
        {"face_db_encrypted_no_plaintext", test_face_db_encrypted_no_plaintext},
        {"face_db_wrong_key_controlled_failure", test_face_db_wrong_key_controlled_failure},
        {"face_db_migration_encrypts_legacy", test_face_db_migration_encrypts_legacy},
        {"face_db_migration_failure_preserves_legacy", test_face_db_migration_failure_preserves_legacy},
        {"face_db_encrypted_roundtrip", test_face_db_encrypted_roundtrip},
    };
    const bool boolOk = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    ::testing::InitGoogleTest(&argc, argv);
    const int gtestOk = RUN_ALL_TESTS();
    return (boolOk && gtestOk == 0) ? 0 : 1;
}
