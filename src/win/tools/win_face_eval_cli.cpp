#include "rk_win/FaceRecognizer.h"
#include "rk_win/StructuredLogger.h"
#include "rk_win/WinConfig.h"

#include <opencv2/imgcodecs.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace rk_win {
namespace {

struct Args {
    std::filesystem::path enrollDir = "datasets/enroll";
    std::filesystem::path probeDir = "datasets/probe";
    std::filesystem::path cascadePath = "tests/data/lbpcascade_frontalface.xml";
    std::filesystem::path outCsv = "storage/win_eval_results.csv";
    std::filesystem::path outJson = "storage/win_eval_summary.json";
    std::filesystem::path dbPath = "storage/win_eval_db.yml";
    int minFaceSizePx = 60;
    double threshold = 55.0;
    double targetAccuracy = 0.95;
};

bool isImageFile(const std::filesystem::path& p) {
    const auto ext = p.extension().string();
    std::string e;
    e.resize(ext.size());
    std::transform(ext.begin(), ext.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp";
}

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        auto next = [&](std::filesystem::path& v) {
            if (i + 1 < argc) v = argv[++i];
        };
        auto nextInt = [&](int& v) {
            if (i + 1 < argc) v = std::stoi(argv[++i]);
        };
        auto nextDouble = [&](double& v) {
            if (i + 1 < argc) v = std::stod(argv[++i]);
        };

        if (k == "--enroll") next(a.enrollDir);
        else if (k == "--probe") next(a.probeDir);
        else if (k == "--cascade") next(a.cascadePath);
        else if (k == "--out_csv") next(a.outCsv);
        else if (k == "--out_json") next(a.outJson);
        else if (k == "--db") next(a.dbPath);
        else if (k == "--min_face") nextInt(a.minFaceSizePx);
        else if (k == "--threshold") nextDouble(a.threshold);
        else if (k == "--target_accuracy") nextDouble(a.targetAccuracy);
    }
    return a;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

void ensureParentDir(const std::filesystem::path& p) {
    std::error_code ec;
    auto parent = p.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
}

}  // namespace
}  // namespace rk_win

int main(int argc, char** argv) {
    using namespace rk_win;
    const auto args = parseArgs(argc, argv);

    const auto enrollDir = resolvePathFromExeDir(args.enrollDir);
    const auto probeDir = resolvePathFromExeDir(args.probeDir);
    const auto cascadePath = resolvePathFromExeDir(args.cascadePath);
    const auto outCsv = resolvePathFromExeDir(args.outCsv);
    const auto outJson = resolvePathFromExeDir(args.outJson);
    const auto dbPath = resolvePathFromExeDir(args.dbPath);

    ensureParentDir(outCsv);
    ensureParentDir(outJson);
    ensureParentDir(dbPath);

    FaceRecognizer rec;
    if (!rec.initialize(cascadePath.string(), dbPath, args.minFaceSizePx, args.threshold)) {
        std::cerr << "EVAL_ERROR init_failed cascade=" << cascadePath.string() << " db=" << dbPath.string() << std::endl;
        return 2;
    }
    rec.clearDb();

    std::cout << "EVAL_INFO enroll_dir=" << enrollDir.string() << " probe_dir=" << probeDir.string() << " threshold=" << args.threshold << std::endl;

    int enrollCount = 0;
    for (const auto& idEntry : std::filesystem::directory_iterator(enrollDir)) {
        if (!idEntry.is_directory()) continue;
        const std::string personId = idEntry.path().filename().string();
        for (const auto& imgEntry : std::filesystem::directory_iterator(idEntry.path())) {
            if (!imgEntry.is_regular_file() || !isImageFile(imgEntry.path())) continue;
            cv::Mat img = cv::imread(imgEntry.path().string(), cv::IMREAD_COLOR);
            if (img.empty()) continue;
            int taken = 0;
            if (rec.enrollFromFrame(personId, img, 1, taken) && taken > 0) enrollCount += taken;
        }
    }
    rec.saveDb();

    std::ofstream csv(outCsv, std::ios::out | std::ios::trunc);
    if (!csv.is_open()) {
        std::cerr << "EVAL_ERROR csv_open_failed path=" << outCsv.string() << std::endl;
        return 3;
    }
    csv << "ts,gt,pred,accepted,distance,confidence,image\n";

    std::set<std::string> labelSet;
    std::map<std::string, std::map<std::string, int>> cm;

    int total = 0;
    int correct = 0;
    int misid = 0;
    int reject = 0;
    int loadFail = 0;
    int noFace = 0;

    for (const auto& idEntry : std::filesystem::directory_iterator(probeDir)) {
        if (!idEntry.is_directory()) continue;
        const std::string gt = idEntry.path().filename().string();
        for (const auto& imgEntry : std::filesystem::directory_iterator(idEntry.path())) {
            if (!imgEntry.is_regular_file() || !isImageFile(imgEntry.path())) continue;
            total++;
            const std::string imgPath = imgEntry.path().string();
            cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
            if (img.empty()) {
                loadFail++;
                continue;
            }
            const auto matches = rec.identify(img);
            if (matches.empty()) {
                noFace++;
                reject++;
                cm[gt]["NO_FACE"]++;
                labelSet.insert(gt);
                labelSet.insert("NO_FACE");
                csv << nowIso8601Local() << "," << gt << ",NO_FACE,0,0,0," << imgPath << "\n";
                continue;
            }

            const auto& best = matches[0];
            const std::string pred = best.accepted ? best.personId : "UNKNOWN";
            if (!best.accepted) reject++;
            else if (pred == gt) correct++;
            else misid++;

            cm[gt][pred]++;
            labelSet.insert(gt);
            labelSet.insert(pred);

            csv << nowIso8601Local() << "," << gt << "," << pred << "," << (best.accepted ? 1 : 0) << ","
                << best.distance << "," << best.confidence << "," << imgPath << "\n";
        }
    }
    csv.close();

    const double acc = (total > 0) ? (static_cast<double>(correct) / static_cast<double>(total)) : 0.0;
    const double misidRate = (total > 0) ? (static_cast<double>(misid) / static_cast<double>(total)) : 0.0;
    const double rejectRate = (total > 0) ? (static_cast<double>(reject) / static_cast<double>(total)) : 0.0;

    std::ofstream json(outJson, std::ios::out | std::ios::trunc);
    if (!json.is_open()) {
        std::cerr << "EVAL_ERROR json_open_failed path=" << outJson.string() << std::endl;
        return 4;
    }

    json << "{";
    json << "\"ts\":\"" << jsonEscape(nowIso8601Local()) << "\",";
    json << "\"enroll_dir\":\"" << jsonEscape(enrollDir.string()) << "\",";
    json << "\"probe_dir\":\"" << jsonEscape(probeDir.string()) << "\",";
    json << "\"cascade\":\"" << jsonEscape(cascadePath.string()) << "\",";
    json << "\"db\":\"" << jsonEscape(dbPath.string()) << "\",";
    json << "\"threshold\":" << args.threshold << ",";
    json << "\"total\":" << total << ",";
    json << "\"enroll_samples\":" << enrollCount << ",";
    json << "\"correct\":" << correct << ",";
    json << "\"misid\":" << misid << ",";
    json << "\"reject\":" << reject << ",";
    json << "\"load_fail\":" << loadFail << ",";
    json << "\"no_face\":" << noFace << ",";
    json << "\"accuracy\":" << acc << ",";
    json << "\"misid_rate\":" << misidRate << ",";
    json << "\"reject_rate\":" << rejectRate << ",";
    json << "\"criterion\":{";
    json << "\"target_accuracy\":" << args.targetAccuracy << ",";
    json << "\"pass\":" << ((acc >= args.targetAccuracy) ? "true" : "false");
    json << "},";

    std::vector<std::string> labels(labelSet.begin(), labelSet.end());
    json << "\"labels\":[";
    for (size_t i = 0; i < labels.size(); i++) {
        if (i) json << ",";
        json << "\"" << jsonEscape(labels[i]) << "\"";
    }
    json << "],";

    json << "\"confusion_matrix\":{";
    bool firstGt = true;
    for (const auto& gtRow : cm) {
        if (!firstGt) json << ",";
        firstGt = false;
        json << "\"" << jsonEscape(gtRow.first) << "\":{";
        bool firstPred = true;
        for (const auto& predCol : gtRow.second) {
            if (!firstPred) json << ",";
            firstPred = false;
            json << "\"" << jsonEscape(predCol.first) << "\":" << predCol.second;
        }
        json << "}";
    }
    json << "}";

    if (acc < args.targetAccuracy) {
        json << ",\"tuning_suggestions\":[";
        json << "\"降低阈值可降低误识但可能增加拒识，请在误识/拒识之间权衡\",";
        json << "\"提高 enroll 样本数量（不同光照/角度）可提升稳定性\",";
        json << "\"必要时引入更强的特征模型（例如 DNN embedding），并对齐/归一化人脸\"";
        json << "]";
    }

    json << "}";
    json.close();

    std::cout << "EVAL_SUMMARY total=" << total << " correct=" << correct << " misid=" << misid << " reject=" << reject
              << " acc=" << acc << " out_csv=" << outCsv.string() << " out_json=" << outJson.string() << std::endl;

    return (acc >= args.targetAccuracy) ? 0 : 5;
}

