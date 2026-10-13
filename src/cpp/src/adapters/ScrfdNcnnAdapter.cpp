#include "adapters/ScrfdNcnnAdapter.h"

#include "NativeLog.h"
#include "NcnnParamGuard.h"
#include "pipeline/ScrfdDecoder.h"

#include <opencv2/imgproc.hpp>  // cv::resize（letterbox）

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include <net.h>
#endif

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace rk_core {

namespace {

// letterbox 填充背景（SCRFD 官方/nihui 转换链为 0 填充，对齐 golden 查证 2026-09-03）
constexpr unsigned char kPadFill = 0;

// 图像按比例缩放入框并居中 pad（simplified letterbox），返回 pad/scale 供坐标回原图。
// pad 取整数与复制区共用，避免 float/int 截断不一致造成的亚像素偏斜（A2 M4）
void letterboxBgr(const cv::Mat& bgr, int outW, int outH,
                  cv::Mat& out, float& padX, float& padY, float& scale) {
    const float s = std::min(static_cast<float>(outW) / std::max(1, bgr.cols),
                             static_cast<float>(outH) / std::max(1, bgr.rows));
    const int dw = static_cast<int>(std::lround(bgr.cols * s));
    const int dh = static_cast<int>(std::lround(bgr.rows * s));
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(dw, dh));
    out.create(outH, outW, CV_8UC3);
    out.setTo(cv::Scalar(kPadFill, kPadFill, kPadFill));
    const int padXi = (outW - dw) / 2;
    const int padYi = (outH - dh) / 2;
    padX = static_cast<float>(padXi);
    padY = static_cast<float>(padYi);
    resized.copyTo(out(cv::Rect(padXi, padYi, dw, dh)));
    scale = s;
}

}  // namespace

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
// ncnn 持有器：隐藏 ncnn 头，避免非 ncnn 构建牵连
class ncnn_net_holder {
public:
    ~ncnn_net_holder() = default;

    bool load(const ScrfdNcnnConfig& cfg, std::string& err) {
        net_.clear();
        net_.opt.use_vulkan_compute = false;
        net_.opt.num_threads = std::max(1, cfg.threads);
        // CVE-2026-50144：.param 文本预校验（与 YoloFace ncnn 路径一致的防护）
        std::string paramErr;
        if (!rk_core::validateNcnnParamFile(cfg.paramPath.c_str(), paramErr)) {
            err = "ncnn_param_rejected: " + paramErr;
            return false;
        }
        if (net_.load_param(cfg.paramPath.c_str()) != 0) {
            err = "ncnn_load_param_failed";
            return false;
        }
        if (net_.load_model(cfg.binPath.c_str()) != 0) {
            err = "ncnn_load_model_failed";
            return false;
        }
        return true;
    }

    // forward 9 个输出（score/bbox/kps × 3），data 已 copy 到独立 vector（引用安全）
    bool run(const cv::Mat& inBgr, const ScrfdNcnnConfig& cfg,
             std::vector<std::vector<float>>& outs,
             std::vector<std::array<int, 3>>& shapes,  // channels, h, w
             std::string& err) {
        // 预处理对齐 SCRFD 官方/nihui 转换链（golden 查证 2026-09-03）：
        //   BGR→RGB，mean 127.5×3，norm 1/128×3。
        cv::Mat inRgb;
        cv::cvtColor(inBgr, inRgb, cv::COLOR_BGR2RGB);
        ncnn::Mat in = ncnn::Mat::from_pixels(inRgb.data, ncnn::Mat::PIXEL_RGB,
                                              inRgb.cols, inRgb.rows);
        const float meanVals[3] = {127.5f, 127.5f, 127.5f};
        const float normVals[3] = {1.f / 128.f, 1.f / 128.f, 1.f / 128.f};
        in.substract_mean_normalize(meanVals, normVals);

        ncnn::Extractor ex = net_.create_extractor();
        ex.set_light_mode(true);
        if (ex.input(cfg.inputName.c_str(), in) != 0) {
            err = "ncnn_set_input_failed";
            return false;
        }
        const char* names[9] = {
            cfg.scoreNames[0].c_str(), cfg.scoreNames[1].c_str(), cfg.scoreNames[2].c_str(),
            cfg.bboxNames[0].c_str(),  cfg.bboxNames[1].c_str(),  cfg.bboxNames[2].c_str(),
            cfg.kpsNames[0].c_str(),   cfg.kpsNames[1].c_str(),   cfg.kpsNames[2].c_str(),
        };
        outs.assign(9, {});
        shapes.assign(9, {0, 0, 0});
        for (int i = 0; i < 9; i++) {
            ncnn::Mat o;
            if (ex.extract(names[i], o) != 0) {
                err = "ncnn_extract_failed: " + std::string(names[i]);
                return false;
            }
            // PNNX 输出为 NCHW：channels、h、w
            const int c = o.c > 0 ? o.c : 1;
            const int h = o.h;
            const int w = o.w;
            const std::size_t total = static_cast<std::size_t>(c) * h * w;
            if (total == 0 || !o.data) {
                err = "ncnn_output_empty: " + std::string(names[i]);
                return false;
            }
            // ncnn 的通道跨度 cstep 含对齐 padding，逐通道复制逻辑元素才能形成紧密 CHW。
            outs[i].resize(total);
            const std::size_t plane = static_cast<std::size_t>(h) * w;
            for (int ch = 0; ch < c; ++ch) {
                const float* p = o.channel(ch);
                std::copy_n(p, plane, outs[i].data() + static_cast<std::size_t>(ch) * plane);
            }
            shapes[i] = {c, h, w};
        }
        return true;
    }

private:
    ncnn::Net net_;
};
#endif

ScrfdNcnnAdapter::ScrfdNcnnAdapter() = default;

ScrfdNcnnAdapter::ScrfdNcnnAdapter(ScrfdNcnnConfig cfg) : cfg_(std::move(cfg)) {}

ScrfdNcnnAdapter::~ScrfdNcnnAdapter() = default;

bool ScrfdNcnnAdapter::load(const std::string& modelPath, std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    err.clear();
    loaded_ = false;
#if !(defined(RK_HAVE_NCNN) && RK_HAVE_NCNN)
    err = "build_disabled";
    return false;
#else
    // 推导 param/bin：modelPath 可为 .param 或 .bin（对称），目录则取 scrfd.param/.bin；
    // 构造传入 ScrfdNcnnConfig 已含 param+bin 时不覆盖（manifest 装配优先，A2 M3）
    bool cfgGiven = !cfg_.paramPath.empty() && !cfg_.binPath.empty();
    if (!cfgGiven) {
        auto extOf = [](const std::string& p) { return std::filesystem::path(p).extension().string(); };
        if (std::filesystem::is_directory(modelPath)) {
            cfg_.paramPath = (std::filesystem::path(modelPath) / "scrfd.param").string();
            cfg_.binPath = (std::filesystem::path(modelPath) / "scrfd.bin").string();
        } else if (extOf(modelPath) == ".param") {
            cfg_.paramPath = modelPath;
            cfg_.binPath = std::filesystem::path(modelPath).replace_extension(".bin").string();
        } else if (extOf(modelPath) == ".bin") {
            cfg_.binPath = modelPath;
            cfg_.paramPath = std::filesystem::path(modelPath).replace_extension(".param").string();
        } else {
            cfg_.paramPath = modelPath;
            cfg_.binPath = std::filesystem::path(modelPath).replace_extension(".bin").string();
        }
    }

    if (!std::filesystem::exists(cfg_.paramPath) || !std::filesystem::exists(cfg_.binPath)) {
        err = "missing_model";
        return false;
    }

    try {
        ncnn_ = std::make_unique<ncnn_net_holder>();
        if (!ncnn_->load(cfg_, err)) {
            return false;
        }
        loaded_ = true;
        return true;
    } catch (const std::exception& e) {
        err = "ncnn_load_exception: " + std::string(e.what());
        return false;
    } catch (...) {
        err = "ncnn_load_unknown_error";
        return false;
    }
#endif
}

FaceDetections ScrfdNcnnAdapter::detect(const cv::Mat& bgr, std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    err.clear();
#if !(defined(RK_HAVE_NCNN) && RK_HAVE_NCNN)
    err = "build_disabled";
    return {};
#else
    if (!loaded_ || !ncnn_) {
        err = "detector_not_loaded";
        return {};
    }
    if (bgr.empty()) {
        err = "image_empty";
        return {};
    }

    float padX = 0, padY = 0, scale = 1.0f;
    cv::Mat inBgr;
    std::vector<std::vector<float>> outs;
    std::vector<std::array<int, 3>> shapes;
    pipeline::ScrfdTensor score[3], bbox[3], kps[3];
    pipeline::ScrfdDecodeOptions opt{};
    FaceDetections dets;
    try {
        // letterbox 的 cv::resize/create 可能抛 cv::Exception——整段收进 try（A2 L5）
        letterboxBgr(bgr, cfg_.inputW, cfg_.inputH, inBgr, padX, padY, scale);
        if (!ncnn_->run(inBgr, cfg_, outs, shapes, err)) {
            return {};
        }
        // 9 输出 → 布局归一化为 canonical（score 0-2 / bbox 3-5 / kps 6-8）
        // 每层 perPosCh：score=1、bbox=4、kps=10；featureSide = inputW / stride
        std::vector<std::vector<float>> scratch(9);  // NCHW 重排缓冲区（flat 零拷贝不占用）
        for (int l = 0; l < 3; l++) {
            const int perPosCh[3] = {1, 4, 10};  // score / bbox / kps
            const int side = cfg_.inputW / cfg_.stride[l];
            const int N = cfg_.anchorsPerPos[l];
            pipeline::ScrfdTensor* dst[3] = {&score[l], &bbox[l], &kps[l]};
            for (int k = 0; k < 3; k++) {
                const int idx = k * 3 + l;  // 0-2 score、3-5 bbox、6-8 kps
                if (!normalizeLevel(shapes[idx][0], shapes[idx][1], shapes[idx][2],
                                    outs[idx].data(), outs[idx].size(),
                                    perPosCh[k], N, side, cfg_.outputLayout,
                                    scratch[idx], *dst[k], err)) {
                    return {};
                }
            }
        }
        opt = makeDecodeOptions(bgr.cols, bgr.rows, padX, padY, scale, cfg_);
        dets = decodeFromTensors(score, bbox, kps, opt, err);
    } catch (const std::exception& e) {
        err = "scrfd_forward_exception: " + std::string(e.what());
        return {};
    } catch (...) {
        err = "scrfd_forward_unknown_error";
        return {};
    }
    if (err.empty()) effective_ = true;  // 解码生效仅当输出合法（A2 M2）
    return dets;
#endif
}

const char* ScrfdNcnnAdapter::name() const {
    return "scrfd_ncnn";
}

bool ScrfdNcnnAdapter::normalizeLevel(
    int channels, int h, int w, const float* data, std::size_t dataLen,
    int perPosCh, int N, int featureSide, const ScrfdNcnnConfig::OutputLayout layout,
    std::vector<float>& scratch, pipeline::ScrfdTensor& out, std::string& err) {
    err.clear();
    if (featureSide <= 0 || N <= 0 || perPosCh <= 0 || !data) {
        err = "scrfd normalizeLevel 非法入参";
        return false;
    }
    const std::size_t grid = static_cast<std::size_t>(featureSide) * featureSide;
    const std::size_t rows = grid * static_cast<std::size_t>(N);
    const std::size_t canonicalCh = static_cast<std::size_t>(perPosCh) * N;

    const bool flat = (layout == ScrfdNcnnConfig::OutputLayout::kFlat) ||
                      (layout == ScrfdNcnnConfig::OutputLayout::kAuto &&
                       channels == 1 && static_cast<std::size_t>(h) == rows && w == perPosCh);
    const bool nchw = (layout == ScrfdNcnnConfig::OutputLayout::kNChw) ||
                      (layout == ScrfdNcnnConfig::OutputLayout::kAuto &&
                       h == featureSide && w == featureSide &&
                       channels == static_cast<int>(canonicalCh));
    if (!flat && !nchw) {
        err = "scrfd 输出布局无法识别（c=" + std::to_string(channels) +
              " h=" + std::to_string(h) + " w=" + std::to_string(w) +
              "；期望 flat 或 NCHW）";
        return false;
    }

    const std::size_t expected = rows * static_cast<std::size_t>(perPosCh);
    if (dataLen > 0 && dataLen < expected) {
        err = "scrfd 层数据长度不足";
        return false;
    }

    out.featW = featureSide;
    out.featH = featureSide;
    if (flat) {
        // 零拷贝：ncnn 1D 展平内存顺序已是 [pos*N+a][perPosCh] 行连续
        out.channels = static_cast<int>(canonicalCh);
        out.data = data;
        out.dataLen = dataLen;
        return true;
    }

    // NCHW ch-major：重排为 [pos*N+a][perPosCh] 行连续（canonical 布局）
    scratch.assign(expected, 0.0f);
    for (std::size_t pos = 0; pos < grid; pos++) {
        for (int a = 0; a < N; a++) {
            for (int o = 0; o < perPosCh; o++) {
                scratch[(pos * static_cast<std::size_t>(N) + a) * perPosCh + o] =
                    data[(static_cast<std::size_t>(a) * perPosCh + o) * grid + pos];
            }
        }
    }
    out.channels = static_cast<int>(canonicalCh);
    out.data = scratch.data();
    out.dataLen = expected;
    return true;
}

pipeline::ScrfdDecodeOptions ScrfdNcnnAdapter::makeDecodeOptions(
    int origWidth, int origHeight, float padX, float padY, float scale,
    const ScrfdNcnnConfig& cfg) {
    pipeline::ScrfdDecodeOptions opt;
    opt.origWidth = origWidth;
    opt.origHeight = origHeight;
    opt.padX = padX;
    opt.padY = padY;
    opt.scale = scale;
    for (int i = 0; i < 3; i++) {
        opt.stride[i] = cfg.stride[i];
        opt.anchorScale[i] = cfg.anchorScale[i];
        opt.numAnchors[i] = cfg.anchorsPerPos[i];
    }
    opt.mode = cfg.decodeMode;
    opt.confThreshold = cfg.confThreshold;
    opt.post = cfg.post;
    return opt;
}

ScrfdNcnnConfig ScrfdNcnnAdapter::configFromProfile(
    const FaceRuntimeProfile& profile, const std::string& paramPath,
    const std::string& binPath, std::string& err) {
    err.clear();
    ScrfdNcnnConfig cfg;
    cfg.paramPath = paramPath;
    cfg.binPath = binPath;
    if (!profile.detectorInput.empty()) cfg.inputName = profile.detectorInput;
    // manifest detectorOutputs 顺序：score×3 / bbox×3 / kps×3（parseV1 已校验长度）
    if (!profile.detectorOutputs.empty()) {
        if (profile.detectorOutputs.size() != 9) {
            err = "detectorOutputs 非 9 个，无法装配 SCRFD 配置";
            return ScrfdNcnnConfig{};
        }
        for (int i = 0; i < 3; i++) {
            cfg.scoreNames[i] = profile.detectorOutputs[static_cast<std::size_t>(i)];
            cfg.bboxNames[i] = profile.detectorOutputs[static_cast<std::size_t>(3 + i)];
            cfg.kpsNames[i] = profile.detectorOutputs[static_cast<std::size_t>(6 + i)];
        }
    }
    if (profile.detectorInputW > 0) cfg.inputW = profile.detectorInputW;
    if (profile.detectorInputH > 0) cfg.inputH = profile.detectorInputH;
    cfg.stride[0] = 8;  // SCRFD 约定固定（profile 无 stride 字段）
    cfg.stride[1] = 16;
    cfg.stride[2] = 32;
    // 阈值：检测置信度 + NMS IoU（包含抑制/maxFaces 沿用 FacePostProcessOptions 默认）
    cfg.confThreshold = profile.detectionConfidence;
    cfg.post.nmsIouThreshold = profile.nmsIou;
    cfg.decodeMode =
        (profile.detectorDecodeMode == FaceRuntimeProfile::DetectorDecodeMode::kDistance)
            ? pipeline::DecodeMode::kDistance
            : pipeline::DecodeMode::kDelta;
    for (int i = 0; i < 3; i++) cfg.anchorsPerPos[i] = profile.detectorAnchorsPerPos[i];
    switch (profile.detectorOutputLayout) {
        case FaceRuntimeProfile::DetectorOutputLayout::kNChw:
            cfg.outputLayout = ScrfdNcnnConfig::OutputLayout::kNChw;
            break;
        case FaceRuntimeProfile::DetectorOutputLayout::kFlat:
            cfg.outputLayout = ScrfdNcnnConfig::OutputLayout::kFlat;
            break;
        default:
            cfg.outputLayout = ScrfdNcnnConfig::OutputLayout::kAuto;
            break;
    }
    return cfg;
}

pipeline::ScrfdDecodeOptions ScrfdNcnnAdapter::decodeOptionsFromProfile(
    const FaceRuntimeProfile& profile) {
    // 经 configFromProfile + makeDecodeOptions 走与推理时一致的透传路径，避免字段漂移。
    // param/bin 对解码选项无意义（传空）；几何字段留默认由运行时 letterbox 回填。
    std::string err;
    const ScrfdNcnnConfig cfg = configFromProfile(profile, std::string(), std::string(), err);
    return makeDecodeOptions(0, 0, 0.0f, 0.0f, 1.0f, cfg);
}

FaceDetections ScrfdNcnnAdapter::decodeFromTensors(
    const pipeline::ScrfdTensor score[3], const pipeline::ScrfdTensor bbox[3],
    const pipeline::ScrfdTensor kps[3], const pipeline::ScrfdDecodeOptions& opt,
    std::string& err) {
    std::vector<pipeline::ScrfdDetection> raw =
        pipeline::ScrfdDecoder::decode(score, bbox, kps, opt, err);
    FaceDetections out;
    out.reserve(raw.size());
    for (const auto& d : raw) {
        FaceDetection fd;
        fd.bbox = cv::Rect2f(static_cast<float>(d.bbox.x), static_cast<float>(d.bbox.y),
                             static_cast<float>(d.bbox.width), static_cast<float>(d.bbox.height));
        fd.score = d.score;
        if (d.keypoints.size() >= 5) {
            std::array<cv::Point2f, 5> kp;
            for (int k = 0; k < 5; k++) kp[k] = cv::Point2f(d.keypoints[k].x, d.keypoints[k].y);
            fd.keypoints5 = kp;
        }
        out.push_back(std::move(fd));
    }
    return out;
}

}  // namespace rk_core
