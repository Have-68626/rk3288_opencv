#include <iostream>

#if __has_include(<opencv2/core.hpp>)
#define RK3288_OPENCV_VERIFY_HAS_OPENCV 1
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
#else
#define RK3288_OPENCV_VERIFY_HAS_OPENCV 0
#endif

#if !RK3288_OPENCV_VERIFY_HAS_OPENCV
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::cerr << "VERIFY_ERROR opencv_headers_not_found"
              << " hint=install_opencv_and_configure_include_paths"
              << std::endl;
    return 2;
}
#else

static size_t getRssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<size_t>(pmc.WorkingSetSize);
    }
    return 0;
#else
    return 0;
#endif
}

static std::map<std::string, int> loadManifest(const std::string& manifestPath) {
    std::map<std::string, int> expected;
    if (manifestPath.empty()) return expected;

    std::ifstream in(manifestPath);
    if (!in.is_open()) return expected;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.rfind("#", 0) == 0) continue;
        if (line.find("filename") != std::string::npos && line.find("expected_faces") != std::string::npos) continue;

        auto comma = line.find(',');
        if (comma == std::string::npos) continue;

        std::string name = line.substr(0, comma);
        std::string val = line.substr(comma + 1);
        try {
            expected[name] = std::stoi(val);
        } catch (...) {
        }
    }
    return expected;
}

static bool hasImageExt(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(c));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".webp";
}

static std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(::tolower(c));
    return s;
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch));
                    out += oss.str();
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

struct VerifyConfig {
    std::string inputDir;
    std::string cascadePath;
    std::string manifestPath;
    std::string outCsv;
    std::string outJson;
    std::string visDir;
    bool enableOrb = false;
    double scaleFactor = 1.1;
    int minNeighbors = 3;
    int minSizePx = 30;
    int maxSizePx = 0;
    double nmsIou = 0.0;
};

struct VerifySummary {
    int total = 0;
    int ok = 0;
    int fail = 0;
    int loadFail = 0;
    int hasFace = 0;
    long long facesSum = 0;
    int eval = 0;
    int tp = 0;
    int fp = 0;
    int tn = 0;
    int fn = 0;
    int exactMatch = 0;
    double exactAccuracy = 0.0;
    long long absErrorSum = 0;
    double avgAbsError = 0.0;
    double avgMsLoad = 0.0;
    double avgMsDetect = 0.0;
    double avgMsOrb = 0.0;
    double avgMsTotal = 0.0;
};

struct ErrorSample {
    std::string filename;
    int expectedFaces = -1;
    int faces = 0;
    int diff = 0;
};

static double rectIou(const cv::Rect& a, const cv::Rect& b) {
    const cv::Rect inter = a & b;
    const int interArea = inter.area();
    if (interArea <= 0) return 0.0;
    const int unionArea = a.area() + b.area() - interArea;
    if (unionArea <= 0) return 0.0;
    return static_cast<double>(interArea) / static_cast<double>(unionArea);
}

static std::vector<cv::Rect> nmsRectangles(const std::vector<cv::Rect>& boxes, double iouThreshold) {
    if (iouThreshold <= 0.0) return boxes;
    if (boxes.size() <= 1) return boxes;

    std::vector<int> order;
    order.reserve(boxes.size());
    for (int i = 0; i < static_cast<int>(boxes.size()); i++) order.push_back(i);

    std::sort(order.begin(), order.end(), [&](int ia, int ib) {
        const int aa = boxes[ia].area();
        const int ab = boxes[ib].area();
        if (aa != ab) return aa > ab;
        if (boxes[ia].x != boxes[ib].x) return boxes[ia].x < boxes[ib].x;
        if (boxes[ia].y != boxes[ib].y) return boxes[ia].y < boxes[ib].y;
        return ia < ib;
    });

    std::vector<char> suppressed(boxes.size(), 0);
    std::vector<cv::Rect> kept;
    kept.reserve(boxes.size());

    for (size_t oi = 0; oi < order.size(); oi++) {
        const int i = order[oi];
        if (suppressed[i]) continue;
        kept.push_back(boxes[i]);
        for (size_t oj = oi + 1; oj < order.size(); oj++) {
            const int j = order[oj];
            if (suppressed[j]) continue;
            if (rectIou(boxes[i], boxes[j]) > iouThreshold) suppressed[j] = 1;
        }
    }

    return kept;
}

static std::string ensureDir(const std::filesystem::path& dir) {
    if (dir.empty()) return "";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return "create_dir_failed";
    return "";
}

static std::string writeVisualization(const cv::Mat& srcBgr,
                                      const std::vector<cv::Rect>& faces,
                                      const std::filesystem::path& outPath) {
    if (srcBgr.empty()) return "image_empty";
    auto dirErr = ensureDir(outPath.parent_path());
    if (!dirErr.empty()) return dirErr;

    cv::Mat vis = srcBgr.clone();
    const int thickness = (std::max)(2, ((std::min)(vis.cols, vis.rows) / 300));
    const double fontScale = (std::max)(0.6, ((std::min)(vis.cols, vis.rows) / 800.0));
    const int font = cv::FONT_HERSHEY_SIMPLEX;

    for (size_t i = 0; i < faces.size(); i++) {
        const cv::Rect r = faces[i] & cv::Rect(0, 0, vis.cols, vis.rows);
        cv::rectangle(vis, r, cv::Scalar(0, 255, 0), thickness);

        const std::string label = std::to_string(static_cast<int>(i + 1));
        int baseLine = 0;
        const cv::Size ts = cv::getTextSize(label, font, fontScale, thickness, &baseLine);
        const int tx = (std::max)(0, r.x);
        const int ty = (std::max)(ts.height + 2, r.y);
        cv::rectangle(vis,
                      cv::Rect(tx, ty - ts.height - 2, ts.width + 4, ts.height + baseLine + 4) &
                          cv::Rect(0, 0, vis.cols, vis.rows),
                      cv::Scalar(0, 0, 0),
                      cv::FILLED);
        cv::putText(vis, label, cv::Point(tx + 2, ty + 2), font, fontScale, cv::Scalar(0, 255, 0), thickness);
    }

    if (!cv::imwrite(outPath.string(), vis)) return "imwrite_failed";
    return "";
}

static void appendJsonErrorSamples(std::ostream& jout, const std::vector<ErrorSample>& samples, size_t limit) {
    jout << "[";
    for (size_t i = 0; i < samples.size() && i < limit; i++) {
        if (i > 0) jout << ",";
        const auto& s = samples[i];
        jout << "{";
        jout << "\"filename\":\"" << jsonEscape(s.filename) << "\",";
        jout << "\"expected_faces\":" << s.expectedFaces << ",";
        jout << "\"faces\":" << s.faces << ",";
        jout << "\"diff\":" << s.diff;
        jout << "}";
    }
    jout << "]";
}

static VerifySummary verifyOnce(const VerifyConfig& cfg,
                                const std::map<std::string, int>& expectedMap,
                                cv::CascadeClassifier& cc) {
    std::ofstream out(cfg.outCsv, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "VERIFY_ERROR csv_open_failed path=" << cfg.outCsv << std::endl;
        std::exit(4);
    }

    out << "filename,expected_faces,has_face,faces,match,abs_error,ms_load,ms_detect,ms_orb,ms_total,ok,err,faces_raw,vis_ok,vis_err\n";

    std::ofstream jout;
    if (!cfg.outJson.empty()) {
        jout.open(cfg.outJson, std::ios::out | std::ios::trunc);
        if (!jout.is_open()) {
            std::cerr << "VERIFY_ERROR json_open_failed path=" << cfg.outJson << std::endl;
            std::exit(4);
        }
        jout << "{";
        jout << "\"input\":\"" << jsonEscape(cfg.inputDir) << "\",";
        jout << "\"cascade\":\"" << jsonEscape(cfg.cascadePath) << "\",";
        jout << "\"manifest\":\"" << jsonEscape(cfg.manifestPath) << "\",";
        jout << "\"scale\":" << cfg.scaleFactor << ",";
        jout << "\"neighbors\":" << cfg.minNeighbors << ",";
        jout << "\"min\":" << cfg.minSizePx << ",";
        jout << "\"max\":" << cfg.maxSizePx << ",";
        jout << "\"nms_iou\":" << cfg.nmsIou << ",";
        jout << "\"vis_dir\":\"" << jsonEscape(cfg.visDir) << "\",";
        jout << "\"results\":[";
    }

    VerifySummary s{};
    long long sumMsLoad = 0;
    long long sumMsDetect = 0;
    long long sumMsOrb = 0;
    long long sumMsTotal = 0;

    std::vector<ErrorSample> overCount;
    std::vector<ErrorSample> underCount;

    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(cfg.inputDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto& p = entry.path();
        if (!hasImageExt(p)) continue;
        files.push_back(p);
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename().string() < b.filename().string(); });

    bool firstJson = true;
    for (const auto& p : files) {
        s.total++;
        const std::string filename = p.filename().string();

        int expectedFaces = -1;
        auto it = expectedMap.find(filename);
        if (it != expectedMap.end()) expectedFaces = it->second;

        std::string err = "";
        size_t rssBytes = 0;
        int facesCount = 0;
        int facesRawCount = 0;
        int keypointsCount = 0;

        std::string visErr = "";
        int visOk = 0;

        long long msLoad = 0;
        long long msDetect = 0;
        long long msOrb = 0;
        long long msTotal = 0;

        const auto t0 = std::chrono::steady_clock::now();
        cv::Mat img = cv::imread(p.string(), cv::IMREAD_COLOR);
        const auto t1 = std::chrono::steady_clock::now();
        msLoad = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (img.empty()) {
            s.loadFail++;
            err = "image_load_failed";
        } else {
            cv::Mat gray;
            cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
            cv::equalizeHist(gray, gray);

            std::vector<cv::Rect> faces;
            cv::Size minSize(cfg.minSizePx, cfg.minSizePx);
            cv::Size maxSize;
            if (cfg.maxSizePx > 0) maxSize = cv::Size(cfg.maxSizePx, cfg.maxSizePx);
            cc.detectMultiScale(gray, faces, cfg.scaleFactor, cfg.minNeighbors, 0, minSize, maxSize);
            const auto t2 = std::chrono::steady_clock::now();
            msDetect = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

            facesRawCount = static_cast<int>(faces.size());
            if (cfg.nmsIou > 0.0) faces = nmsRectangles(faces, cfg.nmsIou);
            facesCount = static_cast<int>(faces.size());

            if (cfg.enableOrb) {
                std::vector<cv::KeyPoint> kps;
                cv::Mat desc;
                auto orb = cv::ORB::create(1000);
                orb->detectAndCompute(gray, cv::noArray(), kps, desc);
                const auto t3 = std::chrono::steady_clock::now();
                msOrb = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
                keypointsCount = static_cast<int>(kps.size());
                msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count();
            } else {
                msTotal = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count();
            }
            rssBytes = getRssBytes();

            if (!cfg.visDir.empty()) {
                const auto outPath = std::filesystem::path(cfg.visDir) / filename;
                visErr = writeVisualization(img, faces, outPath);
                visOk = visErr.empty() ? 1 : 0;
            }

            s.ok++;
            sumMsLoad += msLoad;
            sumMsDetect += msDetect;
            sumMsOrb += msOrb;
            sumMsTotal += msTotal;
            s.facesSum += facesCount;
            if (facesCount > 0) s.hasFace++;

            if (expectedFaces >= 0) {
                s.eval++;
                const bool expPos = expectedFaces > 0;
                const bool gotPos = facesCount > 0;
                if (expPos && gotPos) s.tp++;
                else if (!expPos && gotPos) s.fp++;
                else if (!expPos && !gotPos) s.tn++;
                else s.fn++;

                if (facesCount == expectedFaces) s.exactMatch++;
                const int diff = facesCount - expectedFaces;
                s.absErrorSum += std::llabs(static_cast<long long>(diff));
                if (diff > 0) overCount.push_back(ErrorSample{filename, expectedFaces, facesCount, diff});
                else if (diff < 0) underCount.push_back(ErrorSample{filename, expectedFaces, facesCount, diff});
            }
        }

        const int match = (expectedFaces >= 0 && err.empty() && facesCount == expectedFaces) ? 1 : 0;
        const long long absError = (expectedFaces >= 0 && err.empty())
                                       ? std::llabs(static_cast<long long>(facesCount) - static_cast<long long>(expectedFaces))
                                       : 0;

        out << filename << "," << expectedFaces << "," << (facesCount > 0 ? 1 : 0) << "," << facesCount << ","
            << match << "," << absError << ","
            << msLoad << "," << msDetect << "," << msOrb << "," << msTotal << ","
            << (err.empty() ? 1 : 0) << "," << err << ","
            << facesRawCount << "," << visOk << "," << visErr << "\n";

        if (jout.is_open()) {
            if (!firstJson) jout << ",";
            firstJson = false;
            jout << "{";
            jout << "\"filename\":\"" << jsonEscape(filename) << "\",";
            jout << "\"has_face\":" << (facesCount > 0 ? "true" : "false") << ",";
            jout << "\"faces\":" << facesCount << ",";
            jout << "\"faces_raw\":" << facesRawCount << ",";
            jout << "\"ms_load\":" << msLoad << ",";
            jout << "\"ms_detect\":" << msDetect << ",";
            jout << "\"ms_orb\":" << msOrb << ",";
            jout << "\"ms_total\":" << msTotal << ",";
            jout << "\"ok\":" << (err.empty() ? "true" : "false") << ",";
            jout << "\"err\":\"" << jsonEscape(err) << "\",";
            jout << "\"vis_ok\":" << (visOk ? "true" : "false") << ",";
            jout << "\"vis_err\":\"" << jsonEscape(visErr) << "\"";
            if (expectedFaces >= 0) {
                jout << ",\"expected_faces\":" << expectedFaces;
                if (err.empty()) {
                    jout << ",\"match\":" << (facesCount == expectedFaces ? "true" : "false");
                    jout << ",\"abs_error\":" << std::llabs(static_cast<long long>(facesCount) - static_cast<long long>(expectedFaces));
                }
            }
            jout << "}";
        }

        if (err.empty()) {
            std::cout << "VERIFY_OK"
                      << " file=" << filename
                      << " faces=" << facesCount
                      << " ms_total=" << msTotal
                      << " rss_bytes=" << rssBytes
                      << std::endl;
        } else {
            std::cout << "VERIFY_FAIL"
                      << " file=" << filename
                      << " err=" << err
                      << std::endl;
        }
    }

    s.fail = s.total - s.ok;
    s.avgMsLoad = (s.ok > 0 ? (static_cast<double>(sumMsLoad) / s.ok) : 0.0);
    s.avgMsDetect = (s.ok > 0 ? (static_cast<double>(sumMsDetect) / s.ok) : 0.0);
    s.avgMsOrb = (s.ok > 0 ? (static_cast<double>(sumMsOrb) / s.ok) : 0.0);
    s.avgMsTotal = (s.ok > 0 ? (static_cast<double>(sumMsTotal) / s.ok) : 0.0);
    s.exactAccuracy = (s.eval > 0 ? (static_cast<double>(s.exactMatch) / s.eval) : 0.0);
    s.avgAbsError = (s.eval > 0 ? (static_cast<double>(s.absErrorSum) / s.eval) : 0.0);

    std::cout << "VERIFY_SUMMARY total=" << s.total
              << " ok=" << s.ok
              << " load_fail=" << s.loadFail
              << " has_face=" << s.hasFace
              << " faces_sum=" << s.facesSum
              << " eval=" << s.eval
              << " tp=" << s.tp << " fp=" << s.fp << " tn=" << s.tn << " fn=" << s.fn
              << " exact_match=" << s.exactMatch
              << std::endl;

    if (jout.is_open()) {
        jout << "],";
        jout << "\"summary\":{";
        jout << "\"total\":" << s.total << ",";
        jout << "\"ok\":" << s.ok << ",";
        jout << "\"fail\":" << s.fail << ",";
        jout << "\"load_fail\":" << s.loadFail << ",";
        jout << "\"has_face\":" << s.hasFace << ",";
        jout << "\"faces_sum\":" << s.facesSum << ",";
        jout << "\"avg_ms_load\":" << s.avgMsLoad << ",";
        jout << "\"avg_ms_detect\":" << s.avgMsDetect << ",";
        jout << "\"avg_ms_orb\":" << s.avgMsOrb << ",";
        jout << "\"avg_ms_total\":" << s.avgMsTotal << ",";
        jout << "\"eval\":" << s.eval << ",";
        jout << "\"tp\":" << s.tp << ",";
        jout << "\"fp\":" << s.fp << ",";
        jout << "\"tn\":" << s.tn << ",";
        jout << "\"fn\":" << s.fn << ",";
        jout << "\"exact_match\":" << s.exactMatch << ",";
        jout << "\"exact_accuracy\":" << s.exactAccuracy << ",";
        jout << "\"abs_error_sum\":" << s.absErrorSum << ",";
        jout << "\"avg_abs_error\":" << s.avgAbsError << ",";
        jout << "\"over_count\":";
        appendJsonErrorSamples(jout, overCount, 50);
        jout << ",";
        jout << "\"under_count\":";
        appendJsonErrorSamples(jout, underCount, 50);
        jout << "}";
        jout << "}";
        jout.close();
    }

    return s;
}

static bool parseParamLine(const std::string& line, std::string& tag, VerifyConfig& base, VerifyConfig& out) {
    out = base;
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n' || trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
    size_t pos = 0;
    while (pos < trimmed.size() && (trimmed[pos] == ' ' || trimmed[pos] == '\t')) pos++;
    trimmed = trimmed.substr(pos);
    if (trimmed.empty()) return false;
    if (trimmed.rfind("#", 0) == 0) return false;

    std::vector<std::string> cols;
    std::string cur;
    for (char c : trimmed) {
        if (c == ',') {
            cols.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    cols.push_back(cur);
    for (auto& c : cols) {
        size_t l = 0;
        while (l < c.size() && (c[l] == ' ' || c[l] == '\t')) l++;
        size_t r = c.size();
        while (r > l && (c[r - 1] == ' ' || c[r - 1] == '\t')) r--;
        c = c.substr(l, r - l);
    }

    if (cols.size() < 5) return false;
    if (toLower(cols[0]) == "scale") return false;

    size_t idx = 0;
    try {
        std::stod(cols[0]);
    } catch (...) {
        tag = cols[0];
        idx = 1;
    }
    if (cols.size() < idx + 5) return false;

    try {
        out.scaleFactor = std::stod(cols[idx + 0]);
        out.minNeighbors = std::stoi(cols[idx + 1]);
        out.minSizePx = std::stoi(cols[idx + 2]);
        out.maxSizePx = std::stoi(cols[idx + 3]);
        out.nmsIou = std::stod(cols[idx + 4]);
    } catch (...) {
        return false;
    }

    if (tag.empty()) {
        std::ostringstream oss;
        oss << "s" << std::fixed << std::setprecision(2) << out.scaleFactor
            << "_n" << out.minNeighbors
            << "_min" << out.minSizePx
            << "_max" << out.maxSizePx
            << "_nms" << std::fixed << std::setprecision(2) << out.nmsIou;
        tag = oss.str();
    }
    return true;
}

int main(int argc, char** argv) {
    VerifyConfig cfg{};
    cfg.inputDir = "tests/data";
    cfg.cascadePath = "tests/data/lbpcascade_frontalface.xml";
    cfg.manifestPath = "";
    cfg.outCsv = "opencv_verify_results.csv";
    cfg.outJson = "";
    cfg.visDir = "";
    cfg.enableOrb = false;
    cfg.scaleFactor = 1.1;
    cfg.minNeighbors = 3;
    cfg.minSizePx = 30;
    cfg.maxSizePx = 0;
    cfg.nmsIou = 0.0;

    std::string paramListPath = "";
    int topN = 10;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--input" && i + 1 < argc) {
            cfg.inputDir = argv[++i];
        } else if (a == "--cascade" && i + 1 < argc) {
            cfg.cascadePath = argv[++i];
        } else if (a == "--manifest" && i + 1 < argc) {
            cfg.manifestPath = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            cfg.outCsv = argv[++i];
        } else if (a == "--json" && i + 1 < argc) {
            cfg.outJson = argv[++i];
        } else if (a == "--vis-dir" && i + 1 < argc) {
            cfg.visDir = argv[++i];
        } else if (a == "--orb") {
            cfg.enableOrb = true;
        } else if (a == "--scale" && i + 1 < argc) {
            try {
                cfg.scaleFactor = std::stod(argv[++i]);
            } catch (...) {
            }
        } else if (a == "--neighbors" && i + 1 < argc) {
            try {
                cfg.minNeighbors = std::stoi(argv[++i]);
            } catch (...) {
            }
        } else if (a == "--min" && i + 1 < argc) {
            try {
                cfg.minSizePx = std::stoi(argv[++i]);
            } catch (...) {
            }
        } else if (a == "--max" && i + 1 < argc) {
            try {
                cfg.maxSizePx = std::stoi(argv[++i]);
            } catch (...) {
            }
        } else if (a == "--nms-iou" && i + 1 < argc) {
            try {
                cfg.nmsIou = std::stod(argv[++i]);
            } catch (...) {
            }
        } else if (a == "--param-list" && i + 1 < argc) {
            paramListPath = argv[++i];
        } else if (a == "--top" && i + 1 < argc) {
            try {
                topN = std::stoi(argv[++i]);
            } catch (...) {
            }
        }
    }

    cv::CascadeClassifier cc;
    if (!cc.load(cfg.cascadePath)) {
        std::cerr << "VERIFY_ERROR cascade_load_failed path=" << cfg.cascadePath << std::endl;
        return 3;
    }

    auto expectedMap = loadManifest(cfg.manifestPath);

    if (!paramListPath.empty()) {
        std::ifstream pin(paramListPath);
        if (!pin.is_open()) {
            std::cerr << "VERIFY_ERROR param_list_open_failed path=" << paramListPath << std::endl;
            return 4;
        }

        const auto summaryCsv = std::filesystem::path(cfg.outCsv);
        const auto baseOutDir = summaryCsv.parent_path().empty() ? std::filesystem::current_path() : summaryCsv.parent_path();
        const auto runDir = baseOutDir / "grid_runs";
        const auto mdPath = baseOutDir / "grid_summary.md";
        std::error_code dirEc;
        std::filesystem::create_directories(runDir, dirEc);
        if (dirEc) {
            std::cerr << "VERIFY_ERROR create_dir_failed path=" << runDir.string() << std::endl;
            return 4;
        }

        struct Row {
            std::string tag;
            VerifyConfig runCfg;
            VerifySummary summary;
            std::filesystem::path outCsv;
            std::filesystem::path outJson;
        };

        std::vector<Row> rows;
        std::string line;
        while (std::getline(pin, line)) {
            std::string tag;
            VerifyConfig runCfg;
            if (!parseParamLine(line, tag, cfg, runCfg)) continue;

            const std::filesystem::path runCsv = runDir / ("face_" + tag + ".csv");
            const std::filesystem::path runJson = runDir / ("face_" + tag + ".json");
            runCfg.outCsv = runCsv.string();
            runCfg.outJson = runJson.string();
            if (!cfg.visDir.empty()) runCfg.visDir = (std::filesystem::path(cfg.visDir) / tag).string();

            std::cout << "VERIFY_GRID_START tag=" << tag << " out_csv=" << runCsv.string() << std::endl;
            const auto summary = verifyOnce(runCfg, expectedMap, cc);
            rows.push_back(Row{tag, runCfg, summary, runCsv, runJson});
        }

        std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            if (a.summary.exactAccuracy != b.summary.exactAccuracy) return a.summary.exactAccuracy > b.summary.exactAccuracy;
            if (a.summary.avgAbsError != b.summary.avgAbsError) return a.summary.avgAbsError < b.summary.avgAbsError;
            return a.summary.avgMsDetect < b.summary.avgMsDetect;
        });

        std::ofstream sout(cfg.outCsv, std::ios::out | std::ios::trunc);
        if (!sout.is_open()) {
            std::cerr << "VERIFY_ERROR csv_open_failed path=" << cfg.outCsv << std::endl;
            return 4;
        }
        sout << "rank,tag,scale,neighbors,min,max,nms_iou,exact_accuracy,avg_abs_error,avg_ms_detect,avg_ms_total,faces_sum,has_face,eval,tp,fp,tn,fn,exact_match,out_csv,out_json\n";
        for (size_t i = 0; i < rows.size(); i++) {
            const auto& r = rows[i];
            sout << (i + 1) << "," << r.tag << ","
                 << r.runCfg.scaleFactor << "," << r.runCfg.minNeighbors << "," << r.runCfg.minSizePx << "," << r.runCfg.maxSizePx << "," << r.runCfg.nmsIou << ","
                 << r.summary.exactAccuracy << "," << r.summary.avgAbsError << "," << r.summary.avgMsDetect << "," << r.summary.avgMsTotal << ","
                 << r.summary.facesSum << "," << r.summary.hasFace << "," << r.summary.eval << ","
                 << r.summary.tp << "," << r.summary.fp << "," << r.summary.tn << "," << r.summary.fn << ","
                 << r.summary.exactMatch << ","
                 << r.outCsv.string() << "," << r.outJson.string()
                 << "\n";
        }
        sout.close();

        std::ofstream md(mdPath, std::ios::out | std::ios::trunc);
        if (md.is_open()) {
            md << u8"# \u53c2\u6570\u8bc4\u4f30\u6392\u540d\n\n";
            md << u8"- \u8f93\u5165: `" << cfg.inputDir << "`\n";
            md << "- cascade: `" << cfg.cascadePath << "`\n";
            md << "- manifest: `" << cfg.manifestPath << "`\n";
            md << u8"- \u6c47\u603b CSV: `" << cfg.outCsv << "`\n";
            md << u8"- \u7ed3\u679c\u76ee\u5f55: `" << runDir.string() << "`\n\n";
            md << "| Rank | Tag | scale | neighbors | min | max | nms_iou | exact_accuracy | avg_abs_error | avg_ms_detect |\n";
            md << "| ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
            const size_t n = (std::min)(rows.size(), static_cast<size_t>((std::max)(1, topN)));
            for (size_t i = 0; i < n; i++) {
                const auto& r = rows[i];
                md << "| " << (i + 1) << " | " << r.tag << " | " << r.runCfg.scaleFactor << " | " << r.runCfg.minNeighbors
                   << " | " << r.runCfg.minSizePx << " | " << r.runCfg.maxSizePx << " | " << r.runCfg.nmsIou
                   << " | " << r.summary.exactAccuracy << " | " << r.summary.avgAbsError << " | " << r.summary.avgMsDetect << " |\n";
            }
            md.close();
        }

        return 0;
    }

    const auto summary = verifyOnce(cfg, expectedMap, cc);
    return (summary.ok == summary.total && summary.total > 0) ? 0 : 5;
}
#endif

