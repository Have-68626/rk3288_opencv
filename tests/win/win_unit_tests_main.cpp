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

bool test_lbph_embedder_dim_and_distance();
bool test_face_metrics_confusion_matrix();
bool test_http_faces_server_path_validation();

int main() {
    using namespace rk_win_test;
    const TestCase cases[] = {
        {"lbph_embedder_dim_and_distance", test_lbph_embedder_dim_and_distance},
        {"face_metrics_confusion_matrix", test_face_metrics_confusion_matrix},
        {"http_faces_server_path_validation", test_http_faces_server_path_validation},
    };
    const bool ok = runAll(cases, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    return ok ? 0 : 1;
}

