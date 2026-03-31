#include <map>
#include <string>

namespace {

struct Summary {
    int total = 0;
    int correct = 0;
    int misid = 0;
    int reject = 0;
};

Summary accumulate(const std::map<std::string, std::map<std::string, int>>& cm) {
    Summary s;
    for (const auto& gtRow : cm) {
        for (const auto& predCol : gtRow.second) {
            const int n = predCol.second;
            s.total += n;
            if (predCol.first == "UNKNOWN") s.reject += n;
            else if (predCol.first == gtRow.first) s.correct += n;
            else s.misid += n;
        }
    }
    return s;
}

}  // namespace

bool test_face_metrics_confusion_matrix() {
    std::map<std::string, std::map<std::string, int>> cm;
    cm["alice"]["alice"] = 9;
    cm["alice"]["bob"] = 1;
    cm["bob"]["UNKNOWN"] = 2;
    cm["bob"]["bob"] = 8;

    const auto s = accumulate(cm);
    if (s.total != 20) return false;
    if (s.correct != 17) return false;
    if (s.misid != 1) return false;
    if (s.reject != 2) return false;
    return true;
}

