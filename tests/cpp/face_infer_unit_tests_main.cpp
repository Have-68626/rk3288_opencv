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

int main() {
    using namespace rk_face_infer_test;
    const TestCase cases[] = {
        {"face_infer_image_load_failure", test_face_infer_image_load_failure},
        {"face_infer_yolo_load_failure", test_face_infer_yolo_load_failure},
        {"face_infer_done_no_face", test_face_infer_done_no_face},
        {"face_infer_done_hit", test_face_infer_done_hit},
    };
    const bool ok = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    return ok ? 0 : 1;
}

