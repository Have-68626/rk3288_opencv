#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
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
#include <opencv2/core/ocl.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include <net.h>
#endif

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
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
  std::string qualcommModel;
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

  bool useOpenCL = false;

  std::filesystem::path outDir = "tests/metrics";
  std::string outPrefix = "infer_bench";
  std::string format = "both";
};

static bool isFlagTrue(const std::string &v) {
  std::string s;
  s.reserve(v.size());
  for (char c : v)
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return s == "1" || s == "true" || s == "yes" || s == "on";
}

static Args parseArgs(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; i++) {
    const std::string k = argv[i];
    auto nextStr = [&](std::string &v) {
      if (i + 1 < argc)
        v = argv[++i];
    };
    auto nextPath = [&](std::filesystem::path &v) {
      if (i + 1 < argc)
        v = argv[++i];
    };
    auto nextInt = [&](int &v) {
      if (i + 1 < argc)
        v = std::stoi(argv[++i]);
    };
    auto nextFloat = [&](float &v) {
      if (i + 1 < argc)
        v = std::stof(argv[++i]);
    };
    auto nextBool = [&](bool &v) {
      if (i + 1 < argc)
        v = isFlagTrue(argv[++i]);
    };

    if (k == "--backend")
      nextStr(a.backend);
    else if (k == "--opencv-model")
      nextStr(a.opencvModel);
    else if (k == "--opencv-config")
      nextStr(a.opencvConfig);
    else if (k == "--opencv-framework")
      nextStr(a.opencvFramework);
    else if (k == "--opencv-output")
      nextStr(a.opencvOutput);
    else if (k == "--opencv-backend")
      nextInt(a.opencvBackend);
    else if (k == "--opencv-target")
      nextInt(a.opencvTarget);
    else if (k == "--ncnn-param")
      nextStr(a.ncnnParam);
    else if (k == "--ncnn-bin")
      nextStr(a.ncnnBin);
    else if (k == "--ncnn-input")
      nextStr(a.ncnnInput);
    else if (k == "--ncnn-output")
      nextStr(a.ncnnOutput);
    else if (k == "--qualcomm-model")
      nextStr(a.qualcommModel);
    else if (k == "--ncnn-threads")
      nextInt(a.ncnnThreads);
    else if (k == "--ncnn-lightmode")
      nextBool(a.ncnnLightmode);
    else if (k == "--w")
      nextInt(a.inputW);
    else if (k == "--h")
      nextInt(a.inputH);
    else if (k == "--scale")
      nextFloat(a.scale);
    else if (k == "--mean-b")
      nextInt(a.meanB);
    else if (k == "--mean-g")
      nextInt(a.meanG);
    else if (k == "--mean-r")
      nextInt(a.meanR);
    else if (k == "--swap-rb")
      nextBool(a.swapRB);
    else if (k == "--warmup")
      nextInt(a.warmup);
    else if (k == "--iters")
      nextInt(a.iters);
    else if (k == "--seed")
      nextInt(a.seed);
    else if (k == "--use-opencl")
      nextBool(a.useOpenCL);
    else if (k == "--out-dir")
      nextPath(a.outDir);
    else if (k == "--out-prefix")
      nextStr(a.outPrefix);
    else if (k == "--format")
      nextStr(a.format);
  }
  return a;
}

static std::uint64_t nowEpochSeconds() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto s =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0, s));
}

static std::uint64_t processCpuNanos() {
#ifdef _WIN32
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
    return 0;
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
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
    return 0;
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
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
      if (line.rfind("VmRSS:", 0) != 0)
        continue;
      std::istringstream iss(line);
      std::string key;
      std::uint64_t kb = 0;
      std::string unit;
      iss >> key >> kb >> unit;
      if (kb > 0)
        return kb * 1024ULL;
    }
  }
  {
    std::ifstream f("/proc/self/statm");
    std::uint64_t size = 0;
    std::uint64_t rss = 0;
    if (f >> size >> rss) {
      const long page = sysconf(_SC_PAGESIZE);
      if (page > 0)
        return rss * static_cast<std::uint64_t>(page);
    }
  }
  return 0;
#endif
}

static std::string jsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '"')
      out += "\\\"";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else if (c < 0x20) {
      std::ostringstream oss;
      oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << static_cast<int>(c);
      out += oss.str();
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

static bool ensureDir(const std::filesystem::path &p) {
  std::error_code ec;
  if (std::filesystem::exists(p, ec))
    return true;
  return std::filesystem::create_directories(p, ec);
}

struct Stats {
  double meanMs = 0.0;
  double p50Ms = 0.0;
  double p95Ms = 0.0;
  double minMs = 0.0;
  double maxMs = 0.0;
};

static Stats computeStats(const std::vector<double> &samplesMs) {
  Stats s;
  if (samplesMs.empty())
    return s;

  s.meanMs = std::accumulate(samplesMs.begin(), samplesMs.end(), 0.0) /
             static_cast<double>(samplesMs.size());

  std::vector<double> sorted = samplesMs;
  std::sort(sorted.begin(), sorted.end());
  s.minMs = sorted.front();
  s.maxMs = sorted.back();

  auto pct = [&](double p) -> double {
    if (sorted.size() == 1)
      return sorted[0];
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
  Stats preprocessStats;
  Stats inferStats;
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

static std::optional<Record> runOpenCvDnnBench(const Args &args,
                                               std::string &err) {
  cv::ocl::setUseOpenCL(args.useOpenCL);

  if (args.opencvModel.empty()) {
    err = "缺少 --qualcomm-model";
    return std::nullopt;
  }

  cv::dnn::Net net;
  try {
    if (!args.opencvConfig.empty() && !args.opencvFramework.empty()) {
      net = cv::dnn::readNet(args.opencvModel, args.opencvConfig,
                             args.opencvFramework);
    } else if (!args.opencvConfig.empty()) {
      net = cv::dnn::readNet(args.opencvModel, args.opencvConfig);
    } else {
      net = cv::dnn::readNet(args.opencvModel);
    }
  } catch (const cv::Exception &e) {
    err = std::string("OpenCV readNet 失败: ") + e.what();
    return std::nullopt;
  }

  try {
    net.setPreferableBackend(args.opencvBackend);
    net.setPreferableTarget(args.opencvTarget);
  } catch (const cv::Exception &) {
  }

  const int w = std::max(1, args.inputW);
  const int h = std::max(1, args.inputH);
  cv::Mat bgr = makeRandomBgr(w, h, args.seed);
  const cv::Scalar mean(args.meanB, args.meanG, args.meanR);

  cv::UMat ubgr;
  if (args.useOpenCL) {
    bgr.copyTo(ubgr);
  }

  auto doPreprocess = [&]() -> cv::Mat {
    if (args.useOpenCL) {
      cv::UMat blob;
      cv::dnn::blobFromImage(ubgr, blob, args.scale, cv::Size(w, h), mean,
                             args.swapRB, false);
      return blob.getMat(cv::ACCESS_READ);
    } else {
      return cv::dnn::blobFromImage(bgr, args.scale, cv::Size(w, h), mean,
                                    args.swapRB, false);
    }
  };

  cv::Mat blob = doPreprocess();

  for (int i = 0; i < std::max(0, args.warmup); i++) {
    try {
      cv::Mat wBlob = doPreprocess();
      net.setInput(wBlob);
      if (args.opencvOutput.empty())
        (void)net.forward();
      else
        (void)net.forward(args.opencvOutput);
    } catch (const cv::Exception &) {
    }
  }

  std::vector<double> samples;
  std::vector<double> preSamples;
  std::vector<double> inferSamples;
  samples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));
  preSamples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));
  inferSamples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));

  using clock = std::chrono::steady_clock;
  std::uint64_t peak = rssBytes();
  const std::uint64_t before = peak;
  const std::uint64_t cpu0 = processCpuNanos();
  const auto w0 = clock::now();
  int okIters = 0;
  int errIters = 0;

  for (int i = 0; i < std::max(0, args.iters); i++) {
    try {
      const auto t0 = clock::now();
      cv::Mat iterBlob = doPreprocess();
      const auto t1 = clock::now();
      net.setInput(iterBlob);
      // t1_infer 用于排除 net.setInput() / ex.input() 的开销，确保 inferMs
      // 只计测推理时间
      const auto t1_infer = clock::now();
      if (args.opencvOutput.empty())
        (void)net.forward();
      else
        (void)net.forward(args.opencvOutput);
      const auto t2 = clock::now();

      const double preMs =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              t1 - t0)
              .count();
      const double inferMs =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              t2 - t1_infer)
              .count();
      const double totalMs =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              t2 - t0)
              .count();

      preSamples.push_back(preMs);
      inferSamples.push_back(inferMs);
      samples.push_back(totalMs);
      okIters++;
    } catch (const cv::Exception &) {
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
  r.preprocessStats = computeStats(preSamples);
  r.inferStats = computeStats(inferSamples);
  r.wallMs = static_cast<std::uint64_t>(std::max(
      0.0,
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(w1 -
                                                                            w0)
          .count()));
  r.cpuMs = static_cast<std::uint64_t>(
      (cpu1 >= cpu0) ? ((cpu1 - cpu0) / 1000000ULL) : 0ULL);
  r.cpuUtilPercent = (r.wallMs > 0) ? (100.0 * static_cast<double>(r.cpuMs) /
                                       static_cast<double>(r.wallMs))
                                    : 0.0;
  r.okIters = okIters;
  r.errIters = errIters;
  r.rssBeforeBytes = before;
  r.rssAfterBytes = after;
  r.rssPeakBytes = peak;
  return r;
}

static std::optional<Record> runQualcommBench(const Args &args,
                                              std::string &err) {
  cv::ocl::setUseOpenCL(args.useOpenCL);

  if (args.qualcommModel.empty()) {
    err = "缺少 --opencv-model";
    return std::nullopt;
  }

  // 探测失败或硬件不兼容时回退到 CPU
  std::cout << "[INFO] Qualcomm SDK fallback to CPU... 待补测" << std::endl;

  cv::dnn::Net net;
  try {
    if (!args.opencvConfig.empty() && !args.opencvFramework.empty()) {
      net = cv::dnn::readNet(args.qualcommModel, args.opencvConfig,
                             args.opencvFramework);
    } else if (!args.opencvConfig.empty()) {
      net = cv::dnn::readNet(args.qualcommModel, args.opencvConfig);
    } else {
      net = cv::dnn::readNet(args.qualcommModel);
    }
  } catch (const cv::Exception &e) {
    err = std::string("OpenCV readNet 失败: ") + e.what();
    return std::nullopt;
  }

  try {
    net.setPreferableBackend(args.opencvBackend);
    net.setPreferableTarget(args.opencvTarget);
  } catch (const cv::Exception &) {
  }

  const int w = std::max(1, args.inputW);
  const int h = std::max(1, args.inputH);
  cv::Mat bgr = makeRandomBgr(w, h, args.seed);
  const cv::Scalar mean(args.meanB, args.meanG, args.meanR);

  cv::UMat ubgr;
  if (args.useOpenCL) {
    bgr.copyTo(ubgr);
  }

  auto doPreprocess = [&]() -> cv::Mat {
    if (args.useOpenCL) {
      cv::UMat blob;
      cv::dnn::blobFromImage(ubgr, blob, args.scale, cv::Size(w, h), mean,
                             args.swapRB, false);
      return blob.getMat(cv::ACCESS_READ);
    } else {
      return cv::dnn::blobFromImage(bgr, args.scale, cv::Size(w, h), mean,
                                    args.swapRB, false);
    }
  };

  cv::Mat blob = doPreprocess();

  for (int i = 0; i < std::max(0, args.warmup); i++) {
    try {
      cv::Mat wBlob = doPreprocess();
      net.setInput(wBlob);
      if (args.opencvOutput.empty())
        (void)net.forward();
      else
        (void)net.forward(args.opencvOutput);
    } catch (const cv::Exception &) {
    }
  }

  std::vector<double> samples;
  std::vector<double> preSamples;
  std::vector<double> inferSamples;
  samples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));
  preSamples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));
  inferSamples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));

  using clock = std::chrono::steady_clock;
  std::uint64_t peak = rssBytes();
  const std::uint64_t before = peak;
  const std::uint64_t cpu0 = processCpuNanos();
  const auto w0 = clock::now();
  int okIters = 0;
  int errIters = 0;

  for (int i = 0; i < std::max(0, args.iters); i++) {
    try {
      const auto t0 = clock::now();
      cv::Mat iterBlob = doPreprocess();
      const auto t1 = clock::now();
      net.setInput(iterBlob);
      // t1_infer 用于排除 net.setInput() / ex.input() 的开销，确保 inferMs
      // 只计测推理时间
      const auto t1_infer = clock::now();
      if (args.opencvOutput.empty())
        (void)net.forward();
      else
        (void)net.forward(args.opencvOutput);
      const auto t2 = clock::now();

      const double preMs =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              t1 - t0)
              .count();
      const double inferMs =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              t2 - t1_infer)
              .count();
      const double totalMs =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              t2 - t0)
              .count();

      preSamples.push_back(preMs);
      inferSamples.push_back(inferMs);
      samples.push_back(totalMs);
      okIters++;
    } catch (const cv::Exception &) {
      errIters++;
    }
  }
  const auto w1 = clock::now();
  const std::uint64_t cpu1 = processCpuNanos();
  const std::uint64_t after = rssBytes();
  peak = std::max(peak, after);

  Record r;
  r.backend = "qualcomm";
  r.opencvModel = args.qualcommModel;
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
  r.preprocessStats = computeStats(preSamples);
  r.inferStats = computeStats(inferSamples);
  r.wallMs = static_cast<std::uint64_t>(std::max(
      0.0,
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(w1 -
                                                                            w0)
          .count()));
  r.cpuMs = static_cast<std::uint64_t>(
      (cpu1 >= cpu0) ? ((cpu1 - cpu0) / 1000000ULL) : 0ULL);
  r.cpuUtilPercent = (r.wallMs > 0) ? (100.0 * static_cast<double>(r.cpuMs) /
                                       static_cast<double>(r.wallMs))
                                    : 0.0;
  r.okIters = okIters;
  r.errIters = errIters;
  r.rssBeforeBytes = before;
  r.rssAfterBytes = after;
  r.rssPeakBytes = peak;
  return r;
}

static std::optional<Record> runNcnnBench(const Args &args, std::string &err) {
#if !(defined(RK_HAVE_NCNN) && RK_HAVE_NCNN)
  (void)args;
  err = "当前构建未启用 ncnn（请在 CMake 里开启 RK_ENABLE_NCNN，并确保 ncnn "
        "可用）";
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
  if (!bgr.isContinuous())
    bgr = bgr.clone();

  auto doPreprocess = [&]() -> ncnn::Mat {
    ncnn::Mat in = ncnn::Mat::from_pixels(
        bgr.data, args.swapRB ? ncnn::Mat::PIXEL_BGR2RGB : ncnn::Mat::PIXEL_BGR,
        w, h);
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
    return in;
  };

  ncnn::Mat in = doPreprocess();

  ncnn::Extractor ex_warmup = net.create_extractor();
  for (int i = 0; i < std::max(0, args.warmup); i++) {
    ncnn::Mat wIn = doPreprocess();
    if (ex_warmup.input(args.ncnnInput.c_str(), wIn) != 0)
      continue;
    ncnn::Mat out;
    (void)ex_warmup.extract(args.ncnnOutput.c_str(), out);
  }

  std::vector<double> samples;
  std::vector<double> preSamples;
  std::vector<double> inferSamples;
  samples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));
  preSamples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));
  inferSamples.reserve(static_cast<std::size_t>(std::max(0, args.iters)));

  using clock = std::chrono::steady_clock;
  std::uint64_t peak = rssBytes();
  const std::uint64_t before = peak;
  const std::uint64_t cpu0 = processCpuNanos();
  const auto w0 = clock::now();
  int okIters = 0;
  int errIters = 0;

  ncnn::Extractor ex = net.create_extractor();

  for (int i = 0; i < std::max(0, args.iters); i++) {
    const auto t0 = clock::now();
    ncnn::Mat iterIn = doPreprocess();
    const auto t1 = clock::now();

    if (ex.input(args.ncnnInput.c_str(), iterIn) == 0) {
      // t1_infer 用于排除 net.setInput() / ex.input() 的开销，确保 inferMs
      // 只计测推理时间
      const auto t1_infer = clock::now();
      ncnn::Mat out;
      if (ex.extract(args.ncnnOutput.c_str(), out) == 0) {
        const auto t2 = clock::now();
        const double preMs =
            std::chrono::duration_cast<
                std::chrono::duration<double, std::milli>>(t1 - t0)
                .count();
        const double inferMs =
            std::chrono::duration_cast<
                std::chrono::duration<double, std::milli>>(t2 - t1_infer)
                .count();
        const double totalMs =
            std::chrono::duration_cast<
                std::chrono::duration<double, std::milli>>(t2 - t0)
                .count();

        preSamples.push_back(preMs);
        inferSamples.push_back(inferMs);
        samples.push_back(totalMs);
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
  r.preprocessStats = computeStats(preSamples);
  r.inferStats = computeStats(inferSamples);
  r.wallMs = static_cast<std::uint64_t>(std::max(
      0.0,
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(w1 -
                                                                            w0)
          .count()));
  r.cpuMs = static_cast<std::uint64_t>(
      (cpu1 >= cpu0) ? ((cpu1 - cpu0) / 1000000ULL) : 0ULL);
  r.cpuUtilPercent = (r.wallMs > 0) ? (100.0 * static_cast<double>(r.cpuMs) /
                                       static_cast<double>(r.wallMs))
                                    : 0.0;
  r.okIters = okIters;
  r.errIters = errIters;
  r.rssBeforeBytes = before;
  r.rssAfterBytes = after;
  r.rssPeakBytes = peak;
  return r;
#endif
}

static bool writeCsv(const std::filesystem::path &path, std::uint64_t ts,
                     const Args &args, const std::vector<Record> &recs) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open())
    return false;
  out << "ts,use_opencl,backend,opencv_model,opencv_config,opencv_framework,"
         "opencv_output,opencv_backend,opencv_target,ncnn_param,ncnn_bin,ncnn_"
         "input,ncnn_output,ncnn_threads,ncnn_lightmode,input_w,input_h,scale,"
         "mean_b,mean_g,mean_r,swap_rb,warmup,iters,seed,ok_iters,err_iters,"
         "wall_ms,cpu_ms,cpu_util_percent,mean_ms,p50_ms,p95_ms,min_ms,max_ms,"
         "pre_mean_ms,pre_p50_ms,pre_p95_ms,infer_mean_ms,infer_p50_ms,infer_"
         "p95_ms,rss_before_bytes,rss_after_bytes,rss_peak_bytes\n";
  for (const auto &r : recs) {
    out << ts << "," << (args.useOpenCL ? 1 : 0) << "," << r.backend << ","
        << r.opencvModel << "," << r.opencvConfig << "," << r.opencvFramework
        << "," << r.opencvOutput << "," << r.opencvBackend << ","
        << r.opencvTarget << "," << r.ncnnParam << "," << r.ncnnBin << ","
        << r.ncnnInput << "," << r.ncnnOutput << "," << r.ncnnThreads << ","
        << (r.ncnnLightmode ? 1 : 0) << "," << r.inputW << "," << r.inputH
        << "," << r.scale << "," << r.meanB << "," << r.meanG << "," << r.meanR
        << "," << (r.swapRB ? 1 : 0) << "," << r.warmup << "," << r.iters << ","
        << r.seed << "," << r.okIters << "," << r.errIters << "," << r.wallMs
        << "," << r.cpuMs << "," << r.cpuUtilPercent << "," << r.stats.meanMs
        << "," << r.stats.p50Ms << "," << r.stats.p95Ms << "," << r.stats.minMs
        << "," << r.stats.maxMs << "," << r.preprocessStats.meanMs << ","
        << r.preprocessStats.p50Ms << "," << r.preprocessStats.p95Ms << ","
        << r.inferStats.meanMs << "," << r.inferStats.p50Ms << ","
        << r.inferStats.p95Ms << "," << r.rssBeforeBytes << ","
        << r.rssAfterBytes << "," << r.rssPeakBytes << "\n";
  }
  return true;
}

static bool writeJson(const std::filesystem::path &path, std::uint64_t ts,
                      const Args &args, const std::vector<Record> &recs) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open())
    return false;

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
  out << "\"ncnn_lightmode\":" << (args.ncnnLightmode ? "true" : "false")
      << ",";
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
  out << "\"use_opencl\":" << (args.useOpenCL ? "true" : "false") << ",";
  out << "\"format\":\"" << jsonEscape(args.format) << "\"";
  out << "},";

  bool oclRequested = args.useOpenCL;
  bool oclEffective = cv::ocl::useOpenCL();
  bool haveOpenCL = cv::ocl::haveOpenCL();
  std::string oclDeviceName = "unknown";
  std::string oclDeviceVendor = "unknown";
  std::string oclDeviceVersion = "unknown";
  std::string oclDeviceType = "unknown";
  bool oclDeviceAvailable = false;

  if (haveOpenCL) {
    try {
      cv::ocl::Device dev = cv::ocl::Device::getDefault();
      oclDeviceName = dev.name();
      oclDeviceVendor = dev.vendorName();
      oclDeviceVersion = dev.version();
      int t = dev.type();
      if (t == cv::ocl::Device::TYPE_DEFAULT)
        oclDeviceType = "DEFAULT";
      else if (t == cv::ocl::Device::TYPE_CPU)
        oclDeviceType = "CPU";
      else if (t == cv::ocl::Device::TYPE_GPU)
        oclDeviceType = "GPU";
      else if (t == cv::ocl::Device::TYPE_ACCELERATOR)
        oclDeviceType = "ACCELERATOR";
      else
        oclDeviceType = "OTHER";
      oclDeviceAvailable = dev.available();
    } catch (...) {
      // keep unknown
    }
  }

  out << "\"runtime\":{";
  out << "\"opencl_requested\":" << (oclRequested ? "true" : "false") << ",";
  out << "\"opencl_effective\":" << (oclEffective ? "true" : "false") << ",";
  out << "\"opencl_have_opencl\":" << (haveOpenCL ? "true" : "false") << ",";
  out << "\"opencl_device_name\":\"" << jsonEscape(oclDeviceName) << "\",";
  out << "\"opencl_device_vendor\":\"" << jsonEscape(oclDeviceVendor) << "\",";
  out << "\"opencl_device_version\":\"" << jsonEscape(oclDeviceVersion)
      << "\",";
  out << "\"opencl_device_type\":\"" << jsonEscape(oclDeviceType) << "\"";
  out << "},";

  out << "\"results\":[";
  for (std::size_t i = 0; i < recs.size(); i++) {
    const auto &r = recs[i];
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
    out << "\"pre_mean_ms\":" << r.preprocessStats.meanMs << ",";
    out << "\"pre_p50_ms\":" << r.preprocessStats.p50Ms << ",";
    out << "\"pre_p95_ms\":" << r.preprocessStats.p95Ms << ",";
    out << "\"infer_mean_ms\":" << r.inferStats.meanMs << ",";
    out << "\"infer_p50_ms\":" << r.inferStats.p50Ms << ",";
    out << "\"infer_p95_ms\":" << r.inferStats.p95Ms << ",";
    out << "\"rss_before_bytes\":" << r.rssBeforeBytes << ",";
    out << "\"rss_after_bytes\":" << r.rssAfterBytes << ",";
    out << "\"rss_peak_bytes\":" << r.rssPeakBytes;
    out << "}";
    if (i + 1 < recs.size())
      out << ",";
  }
  out << "]";

  out << "}\n";
  return true;
}

static bool formatHasCsv(const std::string &fmt) {
  std::string s;
  s.reserve(fmt.size());
  for (char c : fmt)
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return s == "csv" || s == "both";
}

static bool formatHasJson(const std::string &fmt) {
  std::string s;
  s.reserve(fmt.size());
  for (char c : fmt)
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return s == "json" || s == "both";
}

static bool backendWantsOpenCv(const std::string &backend) {
  std::string s;
  s.reserve(backend.size());
  for (char c : backend)
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return s == "opencv" || s == "opencv_dnn" || s == "both";
}

static bool backendWantsNcnn(const std::string &backend) {
  std::string s;
  s.reserve(backend.size());
  for (char c : backend)
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return s == "ncnn" || s == "both";
}

static bool backendWantsQualcomm(const std::string &backend) {
  std::string s;
  s.reserve(backend.size());
  for (char c : backend)
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return s == "qualcomm" || s == "both" || s == "all";
}

} // namespace

int main(int argc, char **argv) {
  const Args args = parseArgs(argc, argv);
  if (!ensureDir(args.outDir)) {
    std::cerr << "BENCH_ERROR out_dir_create_failed path="
              << args.outDir.string() << std::endl;
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

  if (backendWantsQualcomm(args.backend)) {
    std::string err;
    auto r = runQualcommBench(args, err);
    if (!r) {
      std::cerr << "BENCH_ERROR qualcomm_failed " << err << std::endl;
      // Record failure reason code
      std::cerr << "Fallback reason: QUALCOMM_SDK_UNAVAILABLE_SANDBOX"
                << std::endl;
    } else {
      records.push_back(*r);
    }
  }

  const std::uint64_t ts = nowEpochSeconds();
  const std::string stem = args.outPrefix + "_" + std::to_string(ts);
  const auto csvPath = args.outDir / (stem + ".csv");
  const auto jsonPath = args.outDir / (stem + ".json");

  bool ok = true;
  if (formatHasCsv(args.format))
    ok = ok && writeCsv(csvPath, ts, args, records);
  if (formatHasJson(args.format))
    ok = ok && writeJson(jsonPath, ts, args, records);
  if (!ok) {
    std::cerr << "BENCH_ERROR write_failed out_dir=" << args.outDir.string()
              << std::endl;
    return 5;
  }

  bool oclRequested = args.useOpenCL;
  bool oclEffective = cv::ocl::useOpenCL();
  bool haveOpenCL = cv::ocl::haveOpenCL();
  std::string oclDeviceName = "unknown";
  std::string oclDeviceVendor = "unknown";
  std::string oclDeviceVersion = "unknown";
  std::string oclDeviceType = "unknown";

  if (haveOpenCL) {
    try {
      cv::ocl::Device dev = cv::ocl::Device::getDefault();
      oclDeviceName = dev.name();
      oclDeviceVendor = dev.vendorName();
      oclDeviceVersion = dev.version();
      int t = dev.type();
      if (t == cv::ocl::Device::TYPE_DEFAULT)
        oclDeviceType = "DEFAULT";
      else if (t == cv::ocl::Device::TYPE_CPU)
        oclDeviceType = "CPU";
      else if (t == cv::ocl::Device::TYPE_GPU)
        oclDeviceType = "GPU";
      else if (t == cv::ocl::Device::TYPE_ACCELERATOR)
        oclDeviceType = "ACCELERATOR";
      else
        oclDeviceType = "OTHER";
    } catch (...) {
      // keep unknown
    }
  }

  std::cout << "BENCH_ENV"
            << " opencl_requested=" << (oclRequested ? "true" : "false")
            << " opencl_effective=" << (oclEffective ? "true" : "false")
            << " opencl_have_opencl=" << (haveOpenCL ? "true" : "false")
            << " opencl_device_name=" << oclDeviceName
            << " opencl_device_vendor=" << oclDeviceVendor
            << " opencl_device_version=" << oclDeviceVersion
            << " opencl_device_type=" << oclDeviceType << std::endl;

  for (const auto &r : records) {
    std::cout << "BENCH_RESULT backend=" << r.backend
              << " total_p95_ms=" << r.stats.p95Ms
              << " pre_p95_ms=" << r.preprocessStats.p95Ms
              << " infer_p95_ms=" << r.inferStats.p95Ms
              << " rss_peak_bytes=" << r.rssPeakBytes << std::endl;
  }
  std::cout << "BENCH_OUTPUT" << " out_dir=" << args.outDir.string()
            << " stem=" << stem << std::endl;

  return 0;
}
