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
bool test_face_infer_done_no_face();
bool test_face_infer_done_hit();
bool test_mock_preflight_rejects_invalid_magic();
bool test_mock_preflight_rejects_incomplete_file();
bool test_mock_preflight_rejects_oversize_image();

bool test_video_manager_is_url_source_valid_urls();
bool test_video_manager_is_url_source_local_paths();
bool test_video_manager_is_url_source_edge_cases();

int main() {
    using namespace rk_face_infer_test;
    const TestCase cases[] = {
        {"face_infer_image_load_failure", test_face_infer_image_load_failure},
        {"face_infer_yolo_load_failure", test_face_infer_yolo_load_failure},
        {"face_infer_done_no_face", test_face_infer_done_no_face},
        {"face_infer_done_hit", test_face_infer_done_hit},
        {"mock_preflight_rejects_invalid_magic", test_mock_preflight_rejects_invalid_magic},
        {"mock_preflight_rejects_incomplete_file", test_mock_preflight_rejects_incomplete_file},
        {"mock_preflight_rejects_oversize_image", test_mock_preflight_rejects_oversize_image},
        {"video_manager_is_url_source_valid_urls", test_video_manager_is_url_source_valid_urls},
        {"video_manager_is_url_source_local_paths", test_video_manager_is_url_source_local_paths},
        {"video_manager_is_url_source_edge_cases", test_video_manager_is_url_source_edge_cases},
    };
    const bool ok = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    return ok ? 0 : 1;
}

