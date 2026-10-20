#include <iostream>

namespace rk_face_infer_test {

using TestFn = bool (*)();

struct TestCase {
    const char* name;
    TestFn fn;
};

static bool runAll(const TestCase* cases, int n) {
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

}  // namespace rk_face_infer_test

bool test_face_infer_image_load_failure();
bool test_face_infer_yolo_load_failure();
bool test_face_infer_concurrent_model_key_switch();
bool test_face_infer_concurrent_key_switch_keeps_in_use_detector();
bool test_face_infer_done_no_face();
bool test_face_infer_done_hit();
bool test_mock_preflight_rejects_invalid_magic();
bool test_mock_preflight_rejects_incomplete_file();
bool test_mock_preflight_rejects_oversize_image();
bool test_mock_preflight_rejects_huge_declared_dimension();
bool test_mock_preflight_accepts_normal_bmp();
bool test_mock_preflight_accepts_normal_os2_bmp();
bool test_mock_preflight_rejects_huge_os2_bmp_dimension();
bool test_mock_preflight_rejects_huge_png_dimension();
bool test_mock_preflight_accepts_real_png();
bool test_mock_preflight_rejects_huge_webp_dimension();
bool test_mock_preflight_accepts_real_webp();
bool test_mock_preflight_rejects_huge_jpeg_dimension();
bool test_mock_preflight_accepts_real_jpeg();
bool test_face_infer_outcome_json_missing_ctx();
bool test_face_infer_outcome_json_missing_decision();
bool test_face_infer_outcome_json_missing_metrics();
bool test_face_infer_outcome_json_full();
bool test_face_infer_outcome_json_no_face();
bool test_face_infer_outcome_json_hit_identity_edges();

bool test_video_manager_is_url_source_valid_urls();
bool test_video_manager_consumes_latest_frame_once();
bool test_video_manager_close_discards_pending_frame();
bool test_mpp_nv12_conversion_preserves_uv_and_strides();
bool test_mpp_nv12_conversion_rejects_invalid_layout();
bool test_video_manager_is_url_source_local_paths();
bool test_video_manager_is_url_source_edge_cases();

bool test_video_manager_sanitize_uri_hides_userinfo();
bool test_video_manager_sanitize_uri_keeps_host_port();
bool test_video_manager_sanitize_uri_http_variants();
bool test_video_manager_sanitize_uri_non_url_passthrough();
bool test_video_manager_sanitize_uri_no_userinfo();
bool test_video_manager_uri_log_sink_no_userinfo();

bool test_int8_precision_detection_iou();
bool test_int8_precision_arcface_similarity();

bool test_int8_yolo_face_registered();
bool test_int8_arcface_registered();
bool test_int8_mobilefacenet_registered();
bool test_model_registry_concurrent_get_and_register();
bool test_model_registry_has_no_retinaface_scrfd_entry();
bool test_int8_yolo_face_creates_detector();
bool test_int8_arcface_creates_embedder();
bool test_int8_mobilefacenet_creates_embedder();

bool test_face_align_basic();
bool test_face_align_nan_bbox();
bool test_face_align_empty_image();
bool test_face_align_negative_bbox();
bool test_face_align_keypoints_out_non112();
bool test_face_align_keypoints_uniform_fit_non_square();

bool test_cleanup_temp_file_removed();
bool test_cleanup_temp_dir_created_and_removed();
bool test_cleanup_file_write_and_read();

bool test_gallery_cache_empty_dir();
bool test_gallery_cache_repeated_call();

bool test_publisher_emits_no_face_on_empty_tracks();
bool test_publisher_throttles_repeated_calls();
bool test_publisher_concurrent_set_and_publish();
bool test_publisher_distinguishes_pending_identity_from_auth_failure();
bool test_face_infer_rejects_multiframe_consecutive_requirement();
// §I3 单次管线统一装配的判别用例（见 test_face_infer_pipeline.cpp）。
bool test_face_infer_routes_embedder_id_and_configures_arcface();
bool test_face_infer_routes_detector_id_and_calls_load();
bool test_face_infer_unknown_ids_fail_without_load();
bool test_face_infer_arcface_failure_does_not_try_mobilefacenet();
bool test_face_infer_detector_cache_reinstalls_on_spec_change();
// 缓存 key 完整性：仅「OpenCV-spec/预处理/ncnn 运行时」字段变化也必须重装。
bool test_face_infer_detector_cache_reinstalls_on_option_change();
bool test_face_infer_embedder_cache_reinstalls_on_option_change();
bool test_face_infer_wrong_backend_for_filetype_fails();
bool test_face_infer_profile_apply_derives_and_reaches_config();
bool test_face_infer_profile_failure_leaves_request_unchanged();
// FACE-TEST-005①：embed 失败的 err 必须带出差异化文本（不得回落成固定文案）。
bool test_face_infer_embed_failure_carries_differential_err();
// 统一 Embedder 接口不得把「输出维度不符」当成功（失败静默）。
bool test_face_infer_embedder_dim_mismatch_is_not_silent();
bool test_face_infer_profile_uses_manifest_identity_threshold();
bool test_face_infer_profile_rejects_anonymous_gallery_snapshot();
bool test_face_infer_gallery_template_space_isolation();
bool test_face_infer_int8_rejects_real_opencv_backend();
bool test_face_infer_int8_rejects_fp32_param_and_nonweight_flags();
bool test_face_infer_scrfd_cache_reloads_complete_decode_contract();
bool test_face_infer_int8_accepts_real_quantized_and_mixed_graphs();
bool test_perf_reporter_nearest_rank_and_stage_totals();
bool test_perf_reporter_rss_uses_batch_maximum();
bool test_perf_reporter_rss_preserves_peak_across_batches();
bool test_perf_reporter_counts_dropped_samples();
bool test_perf_reporter_shutdown_drains_pending_samples();
bool test_perf_reporter_shutdown_wakes_idle_worker();

int main() {
    using namespace rk_face_infer_test;
    const TestCase cases[] = {
        {"publisher_distinguishes_pending_identity_from_auth_failure", test_publisher_distinguishes_pending_identity_from_auth_failure},
        {"face_infer_rejects_multiframe_consecutive_requirement", test_face_infer_rejects_multiframe_consecutive_requirement},
        {"face_infer_profile_uses_manifest_identity_threshold", test_face_infer_profile_uses_manifest_identity_threshold},
        {"face_infer_profile_rejects_anonymous_gallery_snapshot", test_face_infer_profile_rejects_anonymous_gallery_snapshot},
        {"face_infer_gallery_template_space_isolation", test_face_infer_gallery_template_space_isolation},
        {"face_infer_int8_rejects_real_opencv_backend", test_face_infer_int8_rejects_real_opencv_backend},
        {"face_infer_int8_rejects_fp32_param_and_nonweight_flags", test_face_infer_int8_rejects_fp32_param_and_nonweight_flags},
        {"face_infer_scrfd_cache_reloads_complete_decode_contract", test_face_infer_scrfd_cache_reloads_complete_decode_contract},
        {"face_infer_int8_accepts_real_quantized_and_mixed_graphs", test_face_infer_int8_accepts_real_quantized_and_mixed_graphs},
        {"perf_reporter_nearest_rank_and_stage_totals", test_perf_reporter_nearest_rank_and_stage_totals},
        {"perf_reporter_rss_uses_batch_maximum", test_perf_reporter_rss_uses_batch_maximum},
        {"perf_reporter_rss_preserves_peak_across_batches", test_perf_reporter_rss_preserves_peak_across_batches},
        {"perf_reporter_counts_dropped_samples", test_perf_reporter_counts_dropped_samples},
        {"perf_reporter_shutdown_drains_pending_samples", test_perf_reporter_shutdown_drains_pending_samples},
        {"perf_reporter_shutdown_wakes_idle_worker", test_perf_reporter_shutdown_wakes_idle_worker},
        {"face_infer_image_load_failure", test_face_infer_image_load_failure},
        {"face_infer_yolo_load_failure", test_face_infer_yolo_load_failure},
        {"face_infer_concurrent_model_key_switch", test_face_infer_concurrent_model_key_switch},
        {"face_infer_concurrent_key_switch_keeps_in_use_detector", test_face_infer_concurrent_key_switch_keeps_in_use_detector},
        {"face_infer_done_no_face", test_face_infer_done_no_face},
        {"face_infer_done_hit", test_face_infer_done_hit},
        {"mock_preflight_rejects_invalid_magic", test_mock_preflight_rejects_invalid_magic},
        {"mock_preflight_rejects_incomplete_file", test_mock_preflight_rejects_incomplete_file},
        {"mock_preflight_rejects_oversize_image", test_mock_preflight_rejects_oversize_image},
        {"mock_preflight_rejects_huge_declared_dimension", test_mock_preflight_rejects_huge_declared_dimension},
        {"mock_preflight_accepts_normal_bmp", test_mock_preflight_accepts_normal_bmp},
        {"mock_preflight_accepts_normal_os2_bmp", test_mock_preflight_accepts_normal_os2_bmp},
        {"mock_preflight_rejects_huge_os2_bmp_dimension", test_mock_preflight_rejects_huge_os2_bmp_dimension},
        {"mock_preflight_rejects_huge_png_dimension", test_mock_preflight_rejects_huge_png_dimension},
        {"mock_preflight_accepts_real_png", test_mock_preflight_accepts_real_png},
        {"mock_preflight_rejects_huge_webp_dimension", test_mock_preflight_rejects_huge_webp_dimension},
        {"mock_preflight_accepts_real_webp", test_mock_preflight_accepts_real_webp},
        {"mock_preflight_rejects_huge_jpeg_dimension", test_mock_preflight_rejects_huge_jpeg_dimension},
        {"mock_preflight_accepts_real_jpeg", test_mock_preflight_accepts_real_jpeg},
        {"face_infer_outcome_json_missing_ctx", test_face_infer_outcome_json_missing_ctx},
        {"face_infer_outcome_json_missing_decision", test_face_infer_outcome_json_missing_decision},
        {"face_infer_outcome_json_missing_metrics", test_face_infer_outcome_json_missing_metrics},
        {"face_infer_outcome_json_full", test_face_infer_outcome_json_full},
        {"face_infer_outcome_json_no_face", test_face_infer_outcome_json_no_face},
        {"face_infer_outcome_json_hit_identity_edges", test_face_infer_outcome_json_hit_identity_edges},
        {"int8_yolo_face_registered", test_int8_yolo_face_registered},
        {"int8_arcface_registered", test_int8_arcface_registered},
        {"int8_mobilefacenet_registered", test_int8_mobilefacenet_registered},
        {"model_registry_concurrent_get_and_register", test_model_registry_concurrent_get_and_register},
        {"model_registry_has_no_retinaface_scrfd_entry", test_model_registry_has_no_retinaface_scrfd_entry},
        {"int8_yolo_face_creates_detector", test_int8_yolo_face_creates_detector},
        {"int8_arcface_creates_embedder", test_int8_arcface_creates_embedder},
        {"int8_mobilefacenet_creates_embedder", test_int8_mobilefacenet_creates_embedder},
        {"int8_precision_detection_iou", test_int8_precision_detection_iou},
        {"int8_precision_arcface_similarity", test_int8_precision_arcface_similarity},
        {"face_align_basic", test_face_align_basic},
        {"face_align_nan_bbox", test_face_align_nan_bbox},
        {"face_align_empty_image", test_face_align_empty_image},
        {"face_align_negative_bbox", test_face_align_negative_bbox},
        {"face_align_keypoints_out_non112", test_face_align_keypoints_out_non112},
        {"face_align_keypoints_uniform_fit_non_square", test_face_align_keypoints_uniform_fit_non_square},
        {"cleanup_temp_file_removed", test_cleanup_temp_file_removed},
        {"cleanup_temp_dir_created_and_removed", test_cleanup_temp_dir_created_and_removed},
        {"cleanup_file_write_and_read", test_cleanup_file_write_and_read},
        {"gallery_cache_empty_dir", test_gallery_cache_empty_dir},
        {"gallery_cache_repeated_call", test_gallery_cache_repeated_call},
        {"publisher_emits_no_face_on_empty_tracks", test_publisher_emits_no_face_on_empty_tracks},
        {"publisher_throttles_repeated_calls", test_publisher_throttles_repeated_calls},
        {"publisher_concurrent_set_and_publish", test_publisher_concurrent_set_and_publish},
        {"video_manager_is_url_source_valid_urls", test_video_manager_is_url_source_valid_urls},
        {"video_manager_consumes_latest_frame_once", test_video_manager_consumes_latest_frame_once},
        {"video_manager_close_discards_pending_frame", test_video_manager_close_discards_pending_frame},
        {"mpp_nv12_conversion_preserves_uv_and_strides", test_mpp_nv12_conversion_preserves_uv_and_strides},
        {"mpp_nv12_conversion_rejects_invalid_layout", test_mpp_nv12_conversion_rejects_invalid_layout},
        {"video_manager_is_url_source_local_paths", test_video_manager_is_url_source_local_paths},
        {"video_manager_is_url_source_edge_cases", test_video_manager_is_url_source_edge_cases},
        {"video_manager_sanitize_uri_hides_userinfo", test_video_manager_sanitize_uri_hides_userinfo},
        {"video_manager_sanitize_uri_keeps_host_port", test_video_manager_sanitize_uri_keeps_host_port},
        {"video_manager_sanitize_uri_http_variants", test_video_manager_sanitize_uri_http_variants},
        {"video_manager_sanitize_uri_non_url_passthrough", test_video_manager_sanitize_uri_non_url_passthrough},
        {"video_manager_sanitize_uri_no_userinfo", test_video_manager_sanitize_uri_no_userinfo},
        {"video_manager_uri_log_sink_no_userinfo", test_video_manager_uri_log_sink_no_userinfo},
        {"face_infer_routes_embedder_id_and_configures_arcface",
         test_face_infer_routes_embedder_id_and_configures_arcface},
        {"face_infer_routes_detector_id_and_calls_load",
         test_face_infer_routes_detector_id_and_calls_load},
        {"face_infer_unknown_ids_fail_without_load",
         test_face_infer_unknown_ids_fail_without_load},
        {"face_infer_arcface_failure_does_not_try_mobilefacenet",
         test_face_infer_arcface_failure_does_not_try_mobilefacenet},
        {"face_infer_detector_cache_reinstalls_on_spec_change",
         test_face_infer_detector_cache_reinstalls_on_spec_change},
        {"face_infer_detector_cache_reinstalls_on_option_change",
         test_face_infer_detector_cache_reinstalls_on_option_change},
        {"face_infer_embedder_cache_reinstalls_on_option_change",
         test_face_infer_embedder_cache_reinstalls_on_option_change},
        {"face_infer_wrong_backend_for_filetype_fails",
         test_face_infer_wrong_backend_for_filetype_fails},
        {"face_infer_profile_apply_derives_and_reaches_config",
         test_face_infer_profile_apply_derives_and_reaches_config},
        {"face_infer_profile_failure_leaves_request_unchanged",
         test_face_infer_profile_failure_leaves_request_unchanged},
        {"face_infer_embed_failure_carries_differential_err",
         test_face_infer_embed_failure_carries_differential_err},
        {"face_infer_embedder_dim_mismatch_is_not_silent",
         test_face_infer_embedder_dim_mismatch_is_not_silent},
    };
    const bool ok = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    return ok ? 0 : 1;
}

