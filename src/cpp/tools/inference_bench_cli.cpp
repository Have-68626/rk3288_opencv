#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include <net.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace {

struct Args {
    std::string backend = "both";

    std::string opencvModel;
    std::string opencvConfig;
    std::string opencvFramework;
    std::string opencvOutput;
    int opencvBackend = 0;
    int opencvTarget = 0;

    std::string ncnnParam;
    std::string ncnnBin;
    std::string ncnnInput = "data";
    std::string ncnnOutput = "output";
    int ncnnThreads = 1;
    bool ncnnLightmode = true;

    int inputW = 320;
    int inputH = 320;
    float scale = 1.0f;
    int meanB = 0;
    int meanG = 0;
    int meanR = 0;
    bool swapRB = false;

    int warmup = 5;
    int iters = 50;
    int seed = 123;

    std::filesystem::path outDir = "tests/metrics";
    std::string outPrefix = "infer_bench";
    std::string format = "both";
};

static bool isFlagTrue(const std::string& v) {
    std::string s;
    s.reserve(v.size());
    for (char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        auto nextStr = [&](std::string& v) {
            if (i + 1 < argc) v = argv[++i];
        };
        auto nextPath = [&](std::filesystem::path& v) {
            if (i + 1 < argc) v = argv[++i];
        };
        auto nextInt = [&](int& v) {
            if (i + 1 < argc) v = std::stoi(argv[++i]);
        };
        auto nextFloat = [&](float& v) {
            if (i + 1 < argc) v = std::stof(argv[++i]);
        };
        auto nextBool = [&](bool& v) {
            if (i + 1 < argc) v = isFlagTrue(argv[++i]);
        };

        if (k == "--backend") nextStr(a.backend);
        else if (k == "--opencv-model") nextStr(a.opencvModel);
        else if (k == "--opencv-config") nextStr(a.opencvConfig);
        else if (k == "--opencv-framework") nextStr(a.opencvFramework);
        else if (k == "--opencv-output") nextStr(a.opencvOutput);
        else if (k == "--opencv-backend") nextInt(a.opencvBackend);
        else if (k == "--opencv-target") nextInt(a.opencvTarget);
        else if (k == "--ncnn-param") nextStr(a.ncnnParam);
        else if (k == "--ncnn-bin") nextStr(a.ncnnBin);
        else if (k == "--ncnn-input") nextStr(a.ncnnInput);
        else if (k == "--ncnn-output") nextStr(a.ncnnOutput);
        else if (k == "--ncnn-threads") nextInt(a.ncnnThreads);
        else if (k == "--ncnn-lightmode") nextBool(a.ncnnLightmode);
        else if (k == "--w") nextInt(a.inputW);
        else if (k == "--h") nextInt(a.inputH);
        else if (k == "--scale") nextFloat(a.scale);
        else if (k == "--mean-b") nextInt(a.meanB);
        else if (k == "--mean-g") nextInt(a.meanG);
        else if (k == "--mean-r") nextInt(a.meanR);
        else if (k == "--swap-rb") nextBool(a.swapRB);
        else if (k == "--warmup") nextInt(a.warmup);
        else if (k == "--iters") nextInt(a.iters);
        else if (k == "--seed") nextInt(a.seed);
        else if (k == "--out-dir") nextPath(a.outDir);
        else if (k == "--out-prefix") nextStr(a.outPrefix);
        else if (k == "--format") nextStr(a.format);
    }
    return a;
}

static std::uint64_t nowEpochSeconds() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return static_cast<std::uint64_t>(std::max<std::int64_t>(0, s));
}

static std::uint64_t processCpuNanos() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
    ULARGE_INTEGER k{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER u{};
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    const std::uint64_t total100ns = (k.QuadPart + u.QuadPart);
    return total100ns * 100ULL;
#else
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

static std::uint64_t rssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    }
    return 0;
#else
    {
        std::ifstream f("/proc/self/status");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) != 0) continue;
            std::istringstream iss(line);
            std::string key;
            std::uint64_t kb = 0;
            std::string unit;
            iss >> key >> kb >> unit;
            if (kb > 0) return kb * 1024ULL;
        }
    }
    {
        std::ifstream f("/proc/self/statm");
        std::uint64_t size = 0;
        std::uint64_t rss = 0;
        if (f >> size >> rss) {
            const long page = sysconf(_SC_PAGESIZE);
            if (page > 0) return rss * static_cast<std::uint64_t>(page);
        }
    }
    return 0;
#endif
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            std::ostringstream oss;
            oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
            out += oss.str();
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static bool ensureDir(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return true;
    return std::filesystem::create_directories(p, ec);
}

struct Stats {
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
};

static Stats computeStats(const std::vector<double>& samplesMs) {
    Stats s;
    if (samplesMs.empty()) return s;

    s.meanMs = std::accumulate(samplesMs.begin(), samplesMs.end(), 0.0) / static_cast<double>(samplesMs.size());

    std::vector<double> sorted = samplesMs;
    std::sort(sorted.begin(), sorted.end());
    s.minMs = sorted.front();
    s.maxMs = sorted.back();

    auto pct = [&](double p) -> double {
        if (sorted.size() == 1) return sorted[0];
        const double idx = (p / 100.0) * static_cast<double>(sorted.size() - 1);
        const auto i0 = static_cast<std::size_t>(std::floor(idx));
        const auto i1 = std::min(sorted.size() - 1, i0 + 1);
        const double t = idx - static_cast<double>(i0);
        return sorted[i0] * (1.0 - t) + sorted[i1] * t;
    };

    s.p50Ms = pct(50.0);
    s.p95Ms = pct(95.0);
    return s;
}

struct Record {
    std::string backend;

    std::string opencvModel;
    std::string opencvConfig;
    std::string opencvFramework;
    std::string opencvOutput;
    int opencvBackend = 0;
    int opencvTarget = 0;

    std::string ncnnParam;
    std::string ncnnBin;
    std::string ncnnInput;
    std::string ncnnOutput;
    int ncnnThreads = 1;
    bool ncnnLightmode = true;

    int inputW = 0;
    int inputH = 0;
    float scale = 1.0f;
    int meanB = 0;
    int meanG = 0;
    int meanR = 0;
    bool swapRB = false;

    int warmup = 0;
    int iters = 0;
    int seed = 0;

    Stats stats;
    std::uint64_t wallMs = 0;
    std::uint64_t cpuMs = 0;
    double cpuUtilPercent = 0.0;
    int okIters = 0;
    int errIters = 0;
    std::uint64_t rssBeforeBytes = 0;
    std::uint64_t rssAfterBytes = 0;
    std::uint64_t rssPeakBytes = 0;
};

static cv::Mat makeRandomBgr(int w, int h, int seed) {
    cv::Mat img(h, w, CV_8UC3);
    cv::RNG rng(static_cast<std::uint64_t>(seed));
    rng.fill(img, cv::RNG::UNIFORM, 0, 255);
    return img;
}

static std::optional<Record> runOpenCvDnnBench(const Args& args, std::string& err) {
    if (args.opencvModel.empty()) {
        err = "缺少 --opencv-model";
        return std::nullopt;
    }

    cv::dnn::Net net;
    try {
        if (!args.opencvConfig.empty() && !args.opencvFramework.empty()) {
            net = cv::dnn::readNet(args.opencvModel, args.opencvConfig, args.opencvFramework);
        } else if (!args.opencvConfig.empty()) {
            net = cv::dnn::readNet(args.opencvModel, args.opencvConfig);
        } else {
            net = cv::dnn::readNet(args.opencvModel);
        }
    } catch (const cv::Exception& e) {
        err = std::string("OpenCV readNet 失败: ") + e.what();
        return std::nullopt;
    }

    try {
        net.setPreferableBackend(args.opencvBackend);
        net.setPreferableTarget(args.opencvTarget);
    } catch (const cv::Exception&) {
    }

    const int w = std::max(1, args.inputW);
    const int h = std::max(1, args.inputH);
    cv::Mat bgr = makeRandomBgr(w, h, args.seed);
    const cv::Scalar mean(args.meanB, args.meanG, args.meanR);
    cv::Mat blob = cv::dnn::blobFromImage(bgr, args.scale, cv::Size(w, h), mean, args.swapRB, false);

    for (int i = 0; i < std::max(0, args.warmup); i++) {
        try {
            net.setInput(blob);
            if (args.opencvOutput.empty()) (void)net.forward();
            else (void)net.forward(args.opencvOutput);
        } catch (const cv::Exception&) {
        }
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));

    using clock = std::chrono::steady_clock;
    std::uint64_t peak = rssBytes();
    const std::uint64_t before = peak;
    const std::uint64_t cpu0 = processCpuNanos();
    const auto w0 = clock::now();
    int okIters = 0;
    int errIters = 0;

    for (int i = 0; i < std::max(0, args.iters); i++) {
        try {
            net.setInput(blob);
            const auto t0 = clock::now();
            if (args.opencvOutput.empty()) (void)net.forward();
            else (void)net.forward(args.opencvOutput);
            const auto t1 = clock::now();
            const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
            samples.push_back(ms);
            okIters++;
        } catch (const cv::Exception&) {
            errIters++;
        }
    }
    const auto w1 = clock::now();
    const std::uint64_t cpu1 = processCpuNanos();
    const std::uint64_t after = rssBytes();
    peak = std::max(peak, after);

    Record r;
    r.backend = "opencv_dnn";
    r.opencvModel = args.opencvModel;
    r.opencvConfig = args.opencvConfig;
    r.opencvFramework = args.opencvFramework;
    r.opencvOutput = args.opencvOutput;
    r.opencvBackend = args.opencvBackend;
    r.opencvTarget = args.opencvTarget;
    r.inputW = w;
    r.inputH = h;
    r.scale = args.scale;
    r.meanB = args.meanB;
    r.meanG = args.meanG;
    r.meanR = args.meanR;
    r.swapRB = args.swapRB;
    r.warmup = args.warmup;
    r.iters = args.iters;
    r.seed = args.seed;
    r.stats = computeStats(samples);
    r.wallMs = static_cast<std::uint64_t>(std::max(0.0, std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(w1 - w0).count()));
    r.cpuMs = static_cast<std::uint64_t>((cpu1 >= cpu0) ? ((cpu1 - cpu0) / 1000000ULL) : 0ULL);
    r.cpuUtilPercent = (r.wallMs > 0) ? (100.0 * static_cast<double>(r.cpuMs) / static_cast<double>(r.wallMs)) : 0.0;
    r.okIters = okIters;
    r.errIters = errIters;
    r.rssBeforeBytes = before;
    r.rssAfterBytes = after;
    r.rssPeakBytes = peak;
    return r;
}

static std::optional<Record> runNcnnBench(const Args& args, std::string& err) {
#if !(defined(RK_HAVE_NCNN) && RK_HAVE_NCNN)
    (void)args;
    err = "当前构建未启用 ncnn（请在 CMake 里开启 RK_ENABLE_NCNN，并确保 ncnn 可用）";
    return std::nullopt;
#else
    if (args.ncnnParam.empty()) {
        err = "缺少 --ncnn-param";
        return std::nullopt;
    }
    if (args.ncnnBin.empty()) {
        err = "缺少 --ncnn-bin";
        return std::nullopt;
    }
    if (args.ncnnInput.empty()) {
        err = "缺少 --ncnn-input";
        return std::nullopt;
    }
    if (args.ncnnOutput.empty()) {
        err = "缺少 --ncnn-output";
        return std::nullopt;
    }

    ncnn::Net net;
    net.opt.num_threads = std::max(1, args.ncnnThreads);
    net.opt.lightmode = args.ncnnLightmode;

    if (net.load_param(args.ncnnParam.c_str()) != 0) {
        err = "ncnn load_param 失败";
        return std::nullopt;
    }
    if (net.load_model(args.ncnnBin.c_str()) != 0) {
        err = "ncnn load_model 失败";
        return std::nullopt;
    }

    const int w = std::max(1, args.inputW);
    const int h = std::max(1, args.inputH);
    cv::Mat bgr = makeRandomBgr(w, h, args.seed);

    cv::Mat src = bgr;
    if (args.swapRB) {
        cv::Mat rgb;
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        src = std::move(rgb);
    }

    ncnn::Mat in = ncnn::Mat::from_pixels(src.data, args.swapRB ? ncnn::Mat::PIXEL_RGB : ncnn::Mat::PIXEL_BGR, w, h);
    float meanVals[3];
    if (args.swapRB) {
        meanVals[0] = static_cast<float>(args.meanR);
        meanVals[1] = static_cast<float>(args.meanG);
        meanVals[2] = static_cast<float>(args.meanB);
    } else {
        meanVals[0] = static_cast<float>(args.meanB);
        meanVals[1] = static_cast<float>(args.meanG);
        meanVals[2] = static_cast<float>(args.meanR);
    }

    float normVals[3] = {args.scale, args.scale, args.scale};
    in.substract_mean_normalize(meanVals, normVals);

    ncnn::Extractor ex_warmup = net.create_extractor();
    ex_warmup.set_num_threads(std::max(1, args.ncnnThreads));
    for (int i = 0; i < std::max(0, args.warmup); i++) {
        if (ex_warmup.input(args.ncnnInput.c_str(), in) != 0) continue;
        ncnn::Mat out;
        (void)ex_warmup.extract(args.ncnnOutput.c_str(), out);
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));

    using clock = std::chrono::steady_clock;
    std::uint64_t peak = rssBytes();
    const std::uint64_t before = peak;
    const std::uint64_t cpu0 = processCpuNanos();
    const auto w0 = clock::now();
    int okIters = 0;
    int errIters = 0;

    ncnn::Extractor ex = net.create_extractor();
    ex.set_num_threads(std::max(1, args.ncnnThreads));

    for (int i = 0; i < std::max(0, args.iters); i++) {
        if (ex.input(args.ncnnInput.c_str(), in) == 0) {
            ncnn::Mat out;
            const auto t0 = clock::now();
            if (ex.extract(args.ncnnOutput.c_str(), out) == 0) {
                const auto t1 = clock::now();
                const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
                samples.push_back(ms);
                okIters++;
            } else {
                errIters++;
            }
        } else {
            errIters++;
        }
    }
    const auto w1 = clock::now();
    const std::uint64_t cpu1 = processCpuNanos();
    const std::uint64_t after = rssBytes();
    peak = std::max(peak, after);

    Record r;
    r.backend = "ncnn";
    r.ncnnParam = args.ncnnParam;
    r.ncnnBin = args.ncnnBin;
    r.ncnnInput = args.ncnnInput;
    r.ncnnOutput = args.ncnnOutput;
    r.ncnnThreads = args.ncnnThreads;
    r.ncnnLightmode = args.ncnnLightmode;
    r.inputW = w;
    r.inputH = h;
    r.scale = args.scale;
    r.meanB = args.meanB;
    r.meanG = args.meanG;
    r.meanR = args.meanR;
    r.swapRB = args.swapRB;
    r.warmup = args.warmup;
    r.iters = args.iters;
    r.seed = args.seed;
    r.stats = computeStats(samples);
    r.wallMs = static_cast<std::uint64_t>(std::max(0.0, std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(w1 - w0).count()));
    r.cpuMs = static_cast<std::uint64_t>((cpu1 >= cpu0) ? ((cpu1 - cpu0) / 1000000ULL) : 0ULL);
    r.cpuUtilPercent = (r.wallMs > 0) ? (100.0 * static_cast<double>(r.cpuMs) / static_cast<double>(r.wallMs)) : 0.0;
    r.okIters = okIters;
    r.errIters = errIters;
    r.rssBeforeBytes = before;
    r.rssAfterBytes = after;
    r.rssPeakBytes = peak;
    return r;
#endif
}

static bool writeCsv(const std::filesystem::path& path, std::uint64_t ts, const std::vector<Record>& recs) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;
    out << "ts,backend,opencv_model,opencv_config,opencv_framework,opencv_output,opencv_backend,opencv_target,ncnn_param,ncnn_bin,ncnn_input,ncnn_output,ncnn_threads,ncnn_lightmode,input_w,input_h,scale,mean_b,mean_g,mean_r,swap_rb,warmup,iters,seed,ok_iters,err_iters,wall_ms,cpu_ms,cpu_util_percent,mean_ms,p50_ms,p95_ms,min_ms,max_ms,rss_before_bytes,rss_after_bytes,rss_peak_bytes\n";
    for (const auto& r : recs) {
        out << ts << ","
            << r.backend << ","
            << r.opencvModel << ","
            << r.opencvConfig << ","
            << r.opencvFramework << ","
            << r.opencvOutput << ","
            << r.opencvBackend << ","
            << r.opencvTarget << ","
            << r.ncnnParam << ","
            << r.ncnnBin << ","
            << r.ncnnInput << ","
            << r.ncnnOutput << ","
            << r.ncnnThreads << ","
            << (r.ncnnLightmode ? 1 : 0) << ","
            << r.inputW << ","
            << r.inputH << ","
            << r.scale << ","
            << r.meanB << ","
            << r.meanG << ","
            << r.meanR << ","
            << (r.swapRB ? 1 : 0) << ","
            << r.warmup << ","
            << r.iters << ","
            << r.seed << ","
            << r.okIters << ","
            << r.errIters << ","
            << r.wallMs << ","
            << r.cpuMs << ","
            << r.cpuUtilPercent << ","
            << r.stats.meanMs << ","
            << r.stats.p50Ms << ","
            << r.stats.p95Ms << ","
            << r.stats.minMs << ","
            << r.stats.maxMs << ","
            << r.rssBeforeBytes << ","
            << r.rssAfterBytes << ","
            << r.rssPeakBytes
            << "\n";
    }
    return true;
}

static bool writeJson(const std::filesystem::path& path, std::uint64_t ts, const Args& args, const std::vector<Record>& recs) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;

    out << "{";
    out << "\"ts\":" << ts << ",";
    out << "\"args\":{";
    out << "\"backend\":\"" << jsonEscape(args.backend) << "\",";
    out << "\"opencv_model\":\"" << jsonEscape(args.opencvModel) << "\",";
    out << "\"opencv_config\":\"" << jsonEscape(args.opencvConfig) << "\",";
    out << "\"opencv_framework\":\"" << jsonEscape(args.opencvFramework) << "\",";
    out << "\"opencv_output\":\"" << jsonEscape(args.opencvOutput) << "\",";
    out << "\"opencv_backend\":" << args.opencvBackend << ",";
    out << "\"opencv_target\":" << args.opencvTarget << ",";
    out << "\"ncnn_param\":\"" << jsonEscape(args.ncnnParam) << "\",";
    out << "\"ncnn_bin\":\"" << jsonEscape(args.ncnnBin) << "\",";
    out << "\"ncnn_input\":\"" << jsonEscape(args.ncnnInput) << "\",";
    out << "\"ncnn_output\":\"" << jsonEscape(args.ncnnOutput) << "\",";
    out << "\"ncnn_threads\":" << args.ncnnThreads << ",";
    out << "\"ncnn_lightmode\":" << (args.ncnnLightmode ? "true" : "false") << ",";
    out << "\"input_w\":" << args.inputW << ",";
    out << "\"input_h\":" << args.inputH << ",";
    out << "\"scale\":" << args.scale << ",";
    out << "\"mean_b\":" << args.meanB << ",";
    out << "\"mean_g\":" << args.meanG << ",";
    out << "\"mean_r\":" << args.meanR << ",";
    out << "\"swap_rb\":" << (args.swapRB ? "true" : "false") << ",";
    out << "\"warmup\":" << args.warmup << ",";
    out << "\"iters\":" << args.iters << ",";
    out << "\"seed\":" << args.seed << ",";
    out << "\"out_dir\":\"" << jsonEscape(args.outDir.string()) << "\",";
    out << "\"out_prefix\":\"" << jsonEscape(args.outPrefix) << "\",";
    out << "\"format\":\"" << jsonEscape(args.format) << "\"";
    out << "},";

    out << "\"results\":[";
    for (std::size_t i = 0; i < recs.size(); i++) {
        const auto& r = recs[i];
        out << "{";
        out << "\"backend\":\"" << jsonEscape(r.backend) << "\",";
        out << "\"opencv_model\":\"" << jsonEscape(r.opencvModel) << "\",";
        out << "\"opencv_config\":\"" << jsonEscape(r.opencvConfig) << "\",";
        out << "\"opencv_framework\":\"" << jsonEscape(r.opencvFramework) << "\",";
        out << "\"opencv_output\":\"" << jsonEscape(r.opencvOutput) << "\",";
        out << "\"opencv_backend\":" << r.opencvBackend << ",";
        out << "\"opencv_target\":" << r.opencvTarget << ",";
        out << "\"ncnn_param\":\"" << jsonEscape(r.ncnnParam) << "\",";
        out << "\"ncnn_bin\":\"" << jsonEscape(r.ncnnBin) << "\",";
        out << "\"ncnn_input\":\"" << jsonEscape(r.ncnnInput) << "\",";
        out << "\"ncnn_output\":\"" << jsonEscape(r.ncnnOutput) << "\",";
        out << "\"ncnn_threads\":" << r.ncnnThreads << ",";
        out << "\"ncnn_lightmode\":" << (r.ncnnLightmode ? "true" : "false") << ",";
        out << "\"input_w\":" << r.inputW << ",";
        out << "\"input_h\":" << r.inputH << ",";
        out << "\"scale\":" << r.scale << ",";
        out << "\"mean_b\":" << r.meanB << ",";
        out << "\"mean_g\":" << r.meanG << ",";
        out << "\"mean_r\":" << r.meanR << ",";
        out << "\"swap_rb\":" << (r.swapRB ? "true" : "false") << ",";
        out << "\"warmup\":" << r.warmup << ",";
        out << "\"iters\":" << r.iters << ",";
        out << "\"seed\":" << r.seed << ",";
        out << "\"ok_iters\":" << r.okIters << ",";
        out << "\"err_iters\":" << r.errIters << ",";
        out << "\"wall_ms\":" << r.wallMs << ",";
        out << "\"cpu_ms\":" << r.cpuMs << ",";
        out << "\"cpu_util_percent\":" << r.cpuUtilPercent << ",";
        out << "\"mean_ms\":" << r.stats.meanMs << ",";
        out << "\"p50_ms\":" << r.stats.p50Ms << ",";
        out << "\"p95_ms\":" << r.stats.p95Ms << ",";
        out << "\"min_ms\":" << r.stats.minMs << ",";
        out << "\"max_ms\":" << r.stats.maxMs << ",";
        out << "\"rss_before_bytes\":" << r.rssBeforeBytes << ",";
        out << "\"rss_after_bytes\":" << r.rssAfterBytes << ",";
        out << "\"rss_peak_bytes\":" << r.rssPeakBytes;
        out << "}";
        if (i + 1 < recs.size()) out << ",";
    }
    out << "]";

    out << "}\n";
    return true;
}

static bool formatHasCsv(const std::string& fmt) {
    std::string s;
    s.reserve(fmt.size());
    for (char c : fmt) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "csv" || s == "both";
}

static bool formatHasJson(const std::string& fmt) {
    std::string s;
    s.reserve(fmt.size());
    for (char c : fmt) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "json" || s == "both";
}

static bool backendWantsOpenCv(const std::string& backend) {
    std::string s;
    s.reserve(backend.size());
    for (char c : backend) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "opencv" || s == "opencv_dnn" || s == "both";
}

static bool backendWantsNcnn(const std::string& backend) {
    std::string s;
    s.reserve(backend.size());
    for (char c : backend) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "ncnn" || s == "both";
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (!ensureDir(args.outDir)) {
        std::cerr << "BENCH_ERROR out_dir_create_failed path=" << args.outDir.string() << std::endl;
        return 2;
    }

    std::vector<Record> records;
    records.reserve(2);

    if (backendWantsOpenCv(args.backend)) {
        std::string err;
        auto r = runOpenCvDnnBench(args, err);
        if (!r) {
            std::cerr << "BENCH_ERROR opencv_failed " << err << std::endl;
            return 3;
        }
        records.push_back(*r);
    }

    if (backendWantsNcnn(args.backend)) {
        std::string err;
        auto r = runNcnnBench(args, err);
        if (!r) {
            std::cerr << "BENCH_ERROR ncnn_failed " << err << std::endl;
            return 4;
        }
        records.push_back(*r);
    }

    const std::uint64_t ts = nowEpochSeconds();
    const std::string stem = args.outPrefix + "_" + std::to_string(ts);
    const auto csvPath = args.outDir / (stem + ".csv");
    const auto jsonPath = args.outDir / (stem + ".json");

    bool ok = true;
    if (formatHasCsv(args.format)) ok = ok && writeCsv(csvPath, ts, records);
    if (formatHasJson(args.format)) ok = ok && writeJson(jsonPath, ts, args, records);
    if (!ok) {
        std::cerr << "BENCH_ERROR write_failed out_dir=" << args.outDir.string() << std::endl;
        return 5;
    }

    for (const auto& r : records) {
        std::cout << "BENCH_RESULT backend=" << r.backend
                  << " mean_ms=" << r.stats.meanMs
                  << " p50_ms=" << r.stats.p50Ms
                  << " p95_ms=" << r.stats.p95Ms
                  << " rss_peak_bytes=" << r.rssPeakBytes
                  << std::endl;
    }
    std::cout << "BENCH_OUTPUT"
              << " out_dir=" << args.outDir.string()
              << " stem=" << stem
              << std::endl;

    return 0;
}
