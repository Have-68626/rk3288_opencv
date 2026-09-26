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
// INT8 回落链：int8Enabled 生产者 + arcface_int8 节点名注入（见 test_face_infer_pipeline.cpp）。
// 判别点：去掉「捕获节点名」的替身所观察到的 in0/out0 注入即红。
bool test_face_infer_int8_chain_consumes_flag_and_injects_arcface_node_names();
// FACE-TEST-005①：embed 失败的 err 必须带出差异化文本（不得回落成固定文案）。
bool test_face_infer_embed_failure_carries_differential_err();

int main() {
    using namespace rk_face_infer_test;
    const TestCase cases[] = {
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
        {"video_manager_is_url_source_local_paths", test_video_manager_is_url_source_local_paths},
        {"video_manager_is_url_source_edge_cases", test_video_manager_is_url_source_edge_cases},
        {"video_manager_sanitize_uri_hides_userinfo", test_video_manager_sanitize_uri_hides_userinfo},
        {"video_manager_sanitize_uri_keeps_host_port", test_video_manager_sanitize_uri_keeps_host_port},
        {"video_manager_sanitize_uri_http_variants", test_video_manager_sanitize_uri_http_variants},
        {"video_manager_sanitize_uri_non_url_passthrough", test_video_manager_sanitize_uri_non_url_passthrough},
        {"video_manager_sanitize_uri_no_userinfo", test_video_manager_sanitize_uri_no_userinfo},
        {"video_manager_uri_log_sink_no_userinfo", test_video_manager_uri_log_sink_no_userinfo},
        {"face_infer_int8_chain_consumes_flag_and_injects_arcface_node_names",
         test_face_infer_int8_chain_consumes_flag_and_injects_arcface_node_names},
        {"face_infer_embed_failure_carries_differential_err",
         test_face_infer_embed_failure_carries_differential_err},
    };
    const bool ok = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    return ok ? 0 : 1;
}

