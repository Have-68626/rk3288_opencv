#include <gtest/gtest.h>
#include <iostream>
#include <string>

#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/WinConfig.h"

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
bool test_recognition_config_thresholds_are_distinguishable();
bool test_bootstrap_arcface_dir_injects_arcface_threshold();
bool test_bootstrap_lbph_dir_injects_lbph_threshold();
bool test_event_logger_rotates_keeps_generations_capped();
bool test_event_logger_rotation_disabled_when_max_bytes_zero();
bool test_event_logger_prunes_shrunk_generations();
// 日志轮转三件套（tests/win/test_log_rotation.cpp）：A=代数收缩原语、B=StructuredLogger、C=RenderMetricsLogger。
bool test_prune_shrunk_generations_removes_excess();
bool test_prune_shrunk_generations_spares_current();
bool test_prune_shrunk_generations_spares_foreign_suffixes();
bool test_prune_shrunk_generations_noop_when_disabled();
bool test_prune_shrunk_generations_noop_on_missing_dir();
bool test_prune_shrunk_generations_count_is_exact();
bool test_prune_shrunk_generations_huge_suffix_no_throw();
bool test_structured_logger_rotates_and_caps_generations();
bool test_structured_logger_no_rotation_when_max_bytes_zero();
bool test_structured_logger_write_handle_follows_current();
bool test_structured_logger_open_rolls_oversized_cold_start();
bool test_structured_logger_open_is_idempotent();
bool test_structured_logger_header_preserved_across_open_rotation();
bool test_structured_logger_header_preserved_across_append_rotation();
bool test_structured_logger_open_prunes_shrunk_generations();
bool test_structured_logger_rename_failure_does_not_reset_header();
// 重开失败后**自动恢复** + 两路独立（2026-09-28 新增）。
bool test_structured_logger_reopen_recovers_and_paths_independent();
// ★ 2026-09-29 新增：「每路独立重试窗口」的判别用例（S4：此前零覆盖）。
bool test_structured_logger_reopen_windows_are_per_path();
bool test_render_metrics_logger_rotates_and_caps_generations();
bool test_render_metrics_logger_no_rotation_when_max_bytes_zero();
bool test_render_metrics_logger_open_default_arguments();
bool test_render_metrics_logger_open_rolls_oversized_cold_start();
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
// 读取失败（超上限）后**不**标记 mtime ⇒ 可读性恢复且 mtime 未变时仍会重试（2026-09-28 新增）。
bool test_config_reload_retries_after_read_failure_without_mtime_change();
bool test_config_put_and_external_each_consumed_once();
bool test_config_default_serialization_passes_schema();
bool test_config_each_single_field_put_passes_schema();
bool test_config_roundtrip_and_schema_alignment();
bool test_config_masked_echo_preserves_real_value();
bool test_config_empty_post_url_round_trips_and_survives_reload();
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
bool test_config_initialize_cleans_stale_rollback_tmp();
// FACE-ARCWIN-010（S-6）/ FACE-CONFIG-017（S-3）/ FACE-CONFIG-018（S-1）：越界值的「只钳不拒」与
// 写型路径的 PUT 拒绝。四条都以「配置未被清空 / 启动不被判 rejected」为核心断言（红线判据）。
bool test_config_lbph_threshold_over_physical_max_clamped();
bool test_config_log_budget_clamped_to_total_budget();
bool test_config_write_path_parent_ref_put_rejected();
bool test_config_write_path_startup_warns_not_rejected();
// ★ 2026-09-29 新增：热重载隔离竞态（项 2）与 rotate 快照竞态（项 3）——均经测试接缝确定性构造交错。
bool test_config_reload_isolation_skips_when_put_races();
bool test_config_rotate_does_not_clobber_concurrent_put();
// ★ 2026-09-29 新增（项 A）：密钥/密文同源不变量 —— rotate 与 PUT 交错后盘上 config.json 必须能
// 用盘上密钥文件解开（往返）；含 409 重试耗尽路径与普通 rotate 的往返。
bool test_config_rotate_and_put_keep_disk_key_matched();
bool test_config_rotate_conflict_exhaustion_keeps_invariant();
bool test_config_rotate_preserves_sensitive_field_roundtrip();
bool test_reload_endpoint_bumps_generation();
bool test_openapi_endpoint_returns_spec_json();
bool test_face_db_plaintext_vuln_documented();
bool test_face_db_encrypted_no_plaintext();
bool test_face_db_wrong_key_controlled_failure();
bool test_face_db_migration_encrypts_legacy();
bool test_face_db_migration_failure_preserves_legacy();
bool test_face_db_encrypted_roundtrip();
bool test_face_db_load_nonexistent_path_is_intentionally_ok();
// FACE-DB-002：畸形旧明文库必须受控失败（不得抛穿 load() 的 false 契约）。
bool test_face_db_malformed_legacy_is_controlled_failure();
// FACE-ARCWIN-002：initialize() 初始化自检块的判别用例（含阳性对照臂，防 cascade 静默失败造成假绿）。
bool test_arcwin_selfcheck_positive_control();
bool test_arcwin_selfcheck_rejects_dim_mismatch();
bool test_arcwin_selfcheck_rejects_unknown_input_blob();
// test_http_faces_server_path_validation 已在 core_unit_tests 注册,
// 此处不重复注册, 避免跨测试套件重复执行。

// 接通两个 runner：自定义 bool 测试（runAll）+ GoogleTest 测试（RUN_ALL_TESTS）。
// 任一失败均返回非零退出码，使 CTest 失败；全过返回 0。

// ═══ INT8 开关的消费（本用例的**定义**刻意放在本文件）═══
// 本文件是 win_unit_tests 的**唯一注册处**（下方 `main()` 里的 TestCase 表）与自定义 bool runner
// （`runAll`）。其余自定义用例的**定义**都分散在 tests/win/test_*.cpp，本文件只在表头列其前向声明；
// 本用例是**唯一例外**：没有前向声明，定义（见下方函数）与注册同在本文件。
//
// 判别点：`ModelConfig::int8Enabled` 此前**只被序列化**（WinConfig/WinJsonConfig 读写两侧都有键，
//   全仓无任何消费点）⇒ 置 true 与置 false 的 `BootstrapResult` **完全一致**。消费点落地后，
//   置 true 必须产生两处可观测差异：(1) 多一条 id=="int8_recognition" 的 disabled 快照；
//   (2) warning 文本提及该键未生效。
// ⚠️ **对照断言不可省**：若只断言 true 侧有产物，实现退化成「无条件加快照 + 无条件加告警」也照样绿。
//   故此处同时断言 false 侧**没有**这两样东西——默认路径零行为变化正是本次改动的硬要求。
// 不需要任何模型 / 级联文件：cascade 取不存在路径 ⇒ 识别器在 loadCascade 处失败（fail-open，
//   只写 warning），本用例只看 int8 相关产物，不断言 result.ok。
bool test_bootstrap_consumes_int8_enabled_flag() {
    const auto buildWith = [](bool int8On) {
        rk_win::AppConfig cfg;
        cfg.model.recognition = "lbph";  // 避开对 arcface ONNX 权重的依赖（本机通常不在仓）
        cfg.model.int8Enabled = int8On;
        cfg.recognition.cascadePath = "nonexistent/cascade_for_int8_flag_test.xml";
        cfg.recognition.databasePath = ".";
        cfg.dnn.enable = false;  // 不构造/初始化 DNN ⇒ 无需 DNN 模型文件
        return rk_win::RuntimeBootstrap::build(cfg);  // C++17 保证复制消除，BootstrapResult 未必可拷贝
    };

    const auto hasInt8Snapshot = [](const rk_win::BootstrapResult& r) {
        for (const auto& m : r.models) {
            if (m.id == "int8_recognition") {
                // status 只允许落在既有取值（disabled），不得新增枚举值——下游
                // JsonEndpointHandlers 的 loaded/failed/missing 计数与 web 的模型状态 Tab 都按既有取值分流。
                return m.status == "disabled" && !m.isInUse;
            }
        }
        return false;
    };
    const auto mentionsInt8 = [](const rk_win::BootstrapResult& r) {
        return r.warning.find("int8Enabled") != std::string::npos;
    };

    const auto off = buildWith(false);
    if (hasInt8Snapshot(off)) {
        std::cerr << "[test_bootstrap_consumes_int8_enabled_flag] int8Enabled=false 却出现 int8_recognition 快照"
                     "（应零行为变化）" << std::endl;
        return false;
    }
    if (mentionsInt8(off)) {
        std::cerr << "[test_bootstrap_consumes_int8_enabled_flag] int8Enabled=false 却在 warning 中提及 int8Enabled"
                  << std::endl;
        return false;
    }

    const auto on = buildWith(true);
    if (!hasInt8Snapshot(on)) {
        std::cerr << "[test_bootstrap_consumes_int8_enabled_flag] int8Enabled=true 未产生 disabled 的 "
                     "int8_recognition 快照——该键可能仍是「只序列化不消费」" << std::endl;
        return false;
    }
    if (!mentionsInt8(on)) {
        std::cerr << "[test_bootstrap_consumes_int8_enabled_flag] int8Enabled=true 未在 warning 中提示该键未生效"
                  << std::endl;
        return false;
    }

    // 既有快照不得因本开关增减（消费是**追加**，不是替换）。
    for (const char* id : {"cascade_frontalface", "dnn_face_detector"}) {
        bool inOn = false;
        bool inOff = false;
        for (const auto& m : on.models) if (m.id == id) inOn = true;
        for (const auto& m : off.models) if (m.id == id) inOff = true;
        if (!inOn || !inOff) {
            std::cerr << "[test_bootstrap_consumes_int8_enabled_flag] 既有快照 " << id
                      << " 缺失（on=" << inOn << " off=" << inOff << "）" << std::endl;
            return false;
        }
    }
    return true;
}

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
        {"test_recognition_config_thresholds_are_distinguishable", test_recognition_config_thresholds_are_distinguishable},
        // INT8 开关消费（用例定义在本文件内，见 main 上方）——原状「只序列化不消费」的回归钉住。
        // 注册名带 `test_` 前缀：与本表**本区域**（bootstrap_* / recognition_config_* / event_logger_*
        // 那一段）其余各条及函数名一致——原名是那段里唯一漏前缀的一条。
        // ⚠️ 勿据此以为全表统一带前缀：靠后的 pipeline_* / config_* / face_db_* 等大量条目沿用
        //    无前缀的短名，本表为「混合」形态，仅保证与相邻条目一致。
        {"test_bootstrap_consumes_int8_enabled_flag", test_bootstrap_consumes_int8_enabled_flag},
        {"test_bootstrap_arcface_dir_injects_arcface_threshold", test_bootstrap_arcface_dir_injects_arcface_threshold},
        {"test_bootstrap_lbph_dir_injects_lbph_threshold", test_bootstrap_lbph_dir_injects_lbph_threshold},
        {"test_event_logger_rotates_keeps_generations_capped", test_event_logger_rotates_keeps_generations_capped},
        {"test_event_logger_rotation_disabled_when_max_bytes_zero", test_event_logger_rotation_disabled_when_max_bytes_zero},
        {"test_event_logger_prunes_shrunk_generations", test_event_logger_prunes_shrunk_generations},
        // 日志轮转三件套（用例定义在 tests/win/test_log_rotation.cpp）——A/B/C 三组。
        {"test_prune_shrunk_generations_removes_excess", test_prune_shrunk_generations_removes_excess},
        {"test_prune_shrunk_generations_spares_current", test_prune_shrunk_generations_spares_current},
        {"test_prune_shrunk_generations_spares_foreign_suffixes", test_prune_shrunk_generations_spares_foreign_suffixes},
        {"test_prune_shrunk_generations_noop_when_disabled", test_prune_shrunk_generations_noop_when_disabled},
        {"test_prune_shrunk_generations_noop_on_missing_dir", test_prune_shrunk_generations_noop_on_missing_dir},
        {"test_prune_shrunk_generations_count_is_exact", test_prune_shrunk_generations_count_is_exact},
        {"test_prune_shrunk_generations_huge_suffix_no_throw", test_prune_shrunk_generations_huge_suffix_no_throw},
        {"test_structured_logger_rotates_and_caps_generations", test_structured_logger_rotates_and_caps_generations},
        {"test_structured_logger_no_rotation_when_max_bytes_zero", test_structured_logger_no_rotation_when_max_bytes_zero},
        {"test_structured_logger_write_handle_follows_current", test_structured_logger_write_handle_follows_current},
        {"test_structured_logger_open_rolls_oversized_cold_start", test_structured_logger_open_rolls_oversized_cold_start},
        {"test_structured_logger_open_is_idempotent", test_structured_logger_open_is_idempotent},
        {"test_structured_logger_header_preserved_across_open_rotation", test_structured_logger_header_preserved_across_open_rotation},
        {"test_structured_logger_header_preserved_across_append_rotation", test_structured_logger_header_preserved_across_append_rotation},
        {"test_structured_logger_open_prunes_shrunk_generations", test_structured_logger_open_prunes_shrunk_generations},
        {"test_structured_logger_rename_failure_does_not_reset_header", test_structured_logger_rename_failure_does_not_reset_header},
        {"test_structured_logger_reopen_recovers_and_paths_independent",
         test_structured_logger_reopen_recovers_and_paths_independent},
        {"test_structured_logger_reopen_windows_are_per_path",
         test_structured_logger_reopen_windows_are_per_path},
        {"test_render_metrics_logger_rotates_and_caps_generations", test_render_metrics_logger_rotates_and_caps_generations},
        {"test_render_metrics_logger_no_rotation_when_max_bytes_zero", test_render_metrics_logger_no_rotation_when_max_bytes_zero},
        {"test_render_metrics_logger_open_default_arguments", test_render_metrics_logger_open_default_arguments},
        {"test_render_metrics_logger_open_rolls_oversized_cold_start", test_render_metrics_logger_open_rolls_oversized_cold_start},
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
        {"config_reload_retries_after_read_failure_without_mtime_change",
         test_config_reload_retries_after_read_failure_without_mtime_change},
        {"config_put_and_external_each_consumed_once", test_config_put_and_external_each_consumed_once},
        {"config_default_serialization_passes_schema", test_config_default_serialization_passes_schema},
        {"config_each_single_field_put_passes_schema", test_config_each_single_field_put_passes_schema},
        {"config_roundtrip_and_schema_alignment", test_config_roundtrip_and_schema_alignment},
        {"config_masked_echo_preserves_real_value", test_config_masked_echo_preserves_real_value},
        {"config_empty_post_url_round_trips_and_survives_reload", test_config_empty_post_url_round_trips_and_survives_reload},
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
        {"config_initialize_cleans_stale_rollback_tmp", test_config_initialize_cleans_stale_rollback_tmp},
        {"config_lbph_threshold_over_physical_max_clamped", test_config_lbph_threshold_over_physical_max_clamped},
        {"config_log_budget_clamped_to_total_budget", test_config_log_budget_clamped_to_total_budget},
        {"config_write_path_parent_ref_put_rejected", test_config_write_path_parent_ref_put_rejected},
        {"config_write_path_startup_warns_not_rejected", test_config_write_path_startup_warns_not_rejected},
        {"config_reload_isolation_skips_when_put_races", test_config_reload_isolation_skips_when_put_races},
        {"config_rotate_does_not_clobber_concurrent_put", test_config_rotate_does_not_clobber_concurrent_put},
        {"config_rotate_and_put_keep_disk_key_matched", test_config_rotate_and_put_keep_disk_key_matched},
        {"config_rotate_conflict_exhaustion_keeps_invariant", test_config_rotate_conflict_exhaustion_keeps_invariant},
        {"config_rotate_preserves_sensitive_field_roundtrip", test_config_rotate_preserves_sensitive_field_roundtrip},
        {"reload_endpoint_bumps_generation", test_reload_endpoint_bumps_generation},
        {"openapi_endpoint_returns_spec_json", test_openapi_endpoint_returns_spec_json},
        {"face_db_plaintext_vuln_documented", test_face_db_plaintext_vuln_documented},
        {"face_db_encrypted_no_plaintext", test_face_db_encrypted_no_plaintext},
        {"face_db_wrong_key_controlled_failure", test_face_db_wrong_key_controlled_failure},
        {"face_db_migration_encrypts_legacy", test_face_db_migration_encrypts_legacy},
        {"face_db_migration_failure_preserves_legacy", test_face_db_migration_failure_preserves_legacy},
        {"face_db_encrypted_roundtrip", test_face_db_encrypted_roundtrip},
        {"face_db_load_nonexistent_path_is_intentionally_ok", test_face_db_load_nonexistent_path_is_intentionally_ok},
        {"face_db_malformed_legacy_is_controlled_failure", test_face_db_malformed_legacy_is_controlled_failure},
        {"arcwin_selfcheck_positive_control", test_arcwin_selfcheck_positive_control},
        {"arcwin_selfcheck_rejects_dim_mismatch", test_arcwin_selfcheck_rejects_dim_mismatch},
        {"arcwin_selfcheck_rejects_unknown_input_blob", test_arcwin_selfcheck_rejects_unknown_input_blob},
    };
    const bool boolOk = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    ::testing::InitGoogleTest(&argc, argv);
    const int gtestOk = RUN_ALL_TESTS();
    return (boolOk && gtestOk == 0) ? 0 : 1;
}
