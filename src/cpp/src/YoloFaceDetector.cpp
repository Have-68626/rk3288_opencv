#include "YoloFaceDetector.h"
#include "FileHash.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
#include <net.h>
#endif

namespace {

struct LetterboxInfo {
    float scale = 1.0f;
    float padX = 0.0f;
    float padY = 0.0f;
};

static float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static cv::Mat makeInputBgr(const cv::Mat& bgr, const YoloFaceOptions& opt, LetterboxInfo& info) {
    info = {};
    if (bgr.empty()) return {};

    if (!opt.letterbox) {
        cv::Mat resized;
        cv::resize(bgr, resized, cv::Size(opt.inputW, opt.inputH), 0, 0, cv::INTER_LINEAR);
        return resized;
    }

    const float r = std::min(static_cast<float>(opt.inputW) / static_cast<float>(bgr.cols),
                             static_cast<float>(opt.inputH) / static_cast<float>(bgr.rows));
    const int newW = std::max(1, static_cast<int>(std::round(static_cast<float>(bgr.cols) * r)));
    const int newH = std::max(1, static_cast<int>(std::round(static_cast<float>(bgr.rows) * r)));
    info.scale = r;
    info.padX = (static_cast<float>(opt.inputW) - static_cast<float>(newW)) * 0.5f;
    info.padY = (static_cast<float>(opt.inputH) - static_cast<float>(newH)) * 0.5f;

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);

    cv::Mat out(opt.inputH, opt.inputW, bgr.type(), cv::Scalar(114, 114, 114));
    const int x = static_cast<int>(std::floor(info.padX));
    const int y = static_cast<int>(std::floor(info.padY));
    resized.copyTo(out(cv::Rect(x, y, resized.cols, resized.rows)));
    return out;
}

static float iouRect(const cv::Rect2f& a, const cv::Rect2f& b) {
    const float ax1 = a.x;
    const float ay1 = a.y;
    const float ax2 = a.x + a.width;
    const float ay2 = a.y + a.height;
    const float bx1 = b.x;
    const float by1 = b.y;
    const float bx2 = b.x + b.width;
    const float by2 = b.y + b.height;

    const float ix1 = std::max(ax1, bx1);
    const float iy1 = std::max(ay1, by1);
    const float ix2 = std::min(ax2, bx2);
    const float iy2 = std::min(ay2, by2);
    const float iw = std::max(0.0f, ix2 - ix1);
    const float ih = std::max(0.0f, iy2 - iy1);
    const float inter = iw * ih;
    const float ua = std::max(0.0f, a.width) * std::max(0.0f, a.height);
    const float ub = std::max(0.0f, b.width) * std::max(0.0f, b.height);
    const float uni = ua + ub - inter;
    if (uni <= 0.0f) return 0.0f;
    return inter / uni;
}

static FaceDetections nmsFaces(const FaceDetections& in, float iouThr) {
    if (in.empty()) return {};
    if (!(iouThr > 0.0f)) return in;

    std::vector<int> idx(in.size());
    for (size_t i = 0; i < in.size(); i++) idx[i] = static_cast<int>(i);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return in[static_cast<size_t>(a)].score > in[static_cast<size_t>(b)].score; });

    std::vector<char> suppressed(in.size(), 0);
    FaceDetections out;
    out.reserve(in.size());

    for (size_t ii = 0; ii < idx.size(); ii++) {
        const int i = idx[ii];
        if (suppressed[static_cast<size_t>(i)]) continue;
        out.push_back(in[static_cast<size_t>(i)]);
        for (size_t jj = ii + 1; jj < idx.size(); jj++) {
            const int j = idx[jj];
            if (suppressed[static_cast<size_t>(j)]) continue;
            if (iouRect(in[static_cast<size_t>(i)].bbox, in[static_cast<size_t>(j)].bbox) > iouThr) {
                suppressed[static_cast<size_t>(j)] = 1;
            }
        }
    }
    return out;
}

static bool looksNormalized4(float a, float b, float c, float d) {
    const float m = std::max(std::max(std::fabs(a), std::fabs(b)), std::max(std::fabs(c), std::fabs(d)));
    return m <= 2.0f;
}

static void mapPointToOrig(float& x, float& y, const cv::Size& orig, const YoloFaceOptions& opt, const LetterboxInfo& lb) {
    if (opt.letterbox) {
        x = (x - lb.padX) / std::max(lb.scale, 1e-9f);
        y = (y - lb.padY) / std::max(lb.scale, 1e-9f);
    } else {
        const float sx = static_cast<float>(orig.width) / std::max(1, opt.inputW);
        const float sy = static_cast<float>(orig.height) / std::max(1, opt.inputH);
        x *= sx;
        y *= sy;
    }
    x = clampf(x, 0.0f, std::max(0.0f, static_cast<float>(orig.width - 1)));
    y = clampf(y, 0.0f, std::max(0.0f, static_cast<float>(orig.height - 1)));
}

static cv::Rect2f clampRectToOrig(float x1, float y1, float x2, float y2, const cv::Size& orig) {
    x1 = clampf(x1, 0.0f, std::max(0.0f, static_cast<float>(orig.width - 1)));
    y1 = clampf(y1, 0.0f, std::max(0.0f, static_cast<float>(orig.height - 1)));
    x2 = clampf(x2, 0.0f, std::max(0.0f, static_cast<float>(orig.width - 1)));
    y2 = clampf(y2, 0.0f, std::max(0.0f, static_cast<float>(orig.height - 1)));
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    const float w = std::max(0.0f, x2 - x1);
    const float h = std::max(0.0f, y2 - y1);
    return cv::Rect2f(x1, y1, w, h);
}

static FaceDetections decodeYoloLike(const float* data,
                                    int rows,
                                    int cols,
                                    const cv::Size& orig,
                                    const YoloFaceOptions& opt,
                                    const LetterboxInfo& lb) {
    FaceDetections out;
    if (!data || rows <= 0 || cols < 5) return out;

    out.reserve(static_cast<size_t>(rows));
    for (int i = 0; i < rows; i++) {
        const float* r = data + static_cast<size_t>(i) * static_cast<size_t>(cols);
        const float score = r[4];
        if (!(score >= opt.scoreThreshold)) continue;

        const bool isNorm = looksNormalized4(r[0], r[1], r[2], r[3]);

        float x1 = 0.0f;
        float y1 = 0.0f;
        float x2 = 0.0f;
        float y2 = 0.0f;

        const bool maybeXYXY = (r[2] > r[0]) && (r[3] > r[1]);
        if (maybeXYXY) {
            x1 = r[0];
            y1 = r[1];
            x2 = r[2];
            y2 = r[3];
            if (isNorm) {
                x1 *= static_cast<float>(opt.inputW);
                x2 *= static_cast<float>(opt.inputW);
                y1 *= static_cast<float>(opt.inputH);
                y2 *= static_cast<float>(opt.inputH);
            }
        } else {
            float cx = r[0];
            float cy = r[1];
            float w = r[2];
            float h = r[3];
            if (isNorm) {
                cx *= static_cast<float>(opt.inputW);
                w *= static_cast<float>(opt.inputW);
                cy *= static_cast<float>(opt.inputH);
                h *= static_cast<float>(opt.inputH);
            }
            x1 = cx - w * 0.5f;
            y1 = cy - h * 0.5f;
            x2 = cx + w * 0.5f;
            y2 = cy + h * 0.5f;
        }

        mapPointToOrig(x1, y1, orig, opt, lb);
        mapPointToOrig(x2, y2, orig, opt, lb);

        FaceDetection det;
        det.bbox = clampRectToOrig(x1, y1, x2, y2, orig);
        det.score = score;

        if (opt.enableKeypoints5 && cols >= 15) {
            std::array<cv::Point2f, 5> kps{};
            for (int j = 0; j < 5; j++) {
                float kx = r[5 + j * 2 + 0];
                float ky = r[5 + j * 2 + 1];
                if (isNorm) {
                    kx *= static_cast<float>(opt.inputW);
                    ky *= static_cast<float>(opt.inputH);
                }
                mapPointToOrig(kx, ky, orig, opt, lb);
                kps[static_cast<size_t>(j)] = cv::Point2f(kx, ky);
            }
            det.keypoints5 = kps;
        }

        if (det.bbox.width <= 0.0f || det.bbox.height <= 0.0f) continue;
        out.push_back(std::move(det));
    }

    return out;
}

static FaceDetections decodeOpenCvOut(const cv::Mat& out,
                                     const cv::Size& orig,
                                     const YoloFaceOptions& opt,
                                     const LetterboxInfo& lb) {
    if (out.empty()) return {};
    if (out.depth() != CV_32F) return {};

    if (out.dims == 2) {
        const int rows = out.rows;
        const int cols = out.cols;
        return decodeYoloLike(out.ptr<float>(), rows, cols, orig, opt, lb);
    }

    if (out.dims == 3) {
        const int rows = out.size[1];
        const int cols = out.size[2];
        return decodeYoloLike(out.ptr<float>(), rows, cols, orig, opt, lb);
    }

    if (out.dims == 4) {
        const int rows = out.size[2];
        const int cols = out.size[3];
        return decodeYoloLike(out.ptr<float>(), rows, cols, orig, opt, lb);
    }

    return {};
}

class OpenCvDnnYoloFaceDetector final : public YoloFaceDetector {
public:
    bool load(const YoloFaceModelSpec& spec, const YoloFaceOptions& opt, std::string& err) override {
        err.clear();
        opt_ = opt;
        spec_ = spec;
        try {
            if (spec.modelPath.empty()) {
                err = "model_path_empty";
                return false;
            }

            std::string hash = rk_wcfr::calculateSHA256(spec.modelPath);
            if (hash.empty()) {
                std::cerr << "[Self-Check] ERROR: Failed to read YOLO model: " << spec.modelPath << std::endl;
                std::cerr << "[Self-Check] Please download the model and place it in the correct directory." << std::endl;
            } else {
                std::cout << "[Self-Check] Loaded YOLO model: " << spec.modelPath << " | SHA256: " << hash << std::endl;
            }

            if (!spec.framework.empty()) {
                net_ = cv::dnn::readNet(spec.modelPath, spec.configPath, spec.framework);
            } else if (!spec.configPath.empty()) {
                net_ = cv::dnn::readNet(spec.modelPath, spec.configPath);
            } else {
                net_ = cv::dnn::readNet(spec.modelPath);
            }

            if (net_.empty()) {
                err = "opencv_readnet_failed";
                return false;
            }

            if (opt_.opencvBackend != 0) net_.setPreferableBackend(opt_.opencvBackend);
            if (opt_.opencvTarget != 0) net_.setPreferableTarget(opt_.opencvTarget);
            loaded_ = true;
            return true;
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        } catch (...) {
            err = "opencv_load_unknown_error";
            return false;
        }
    }

    FaceDetections detect(const cv::Mat& bgr, std::string& err) override {
        err.clear();
        if (!loaded_) {
            err = "model_not_loaded";
            return {};
        }
        if (bgr.empty()) {
            err = "image_empty";
            return {};
        }

        LetterboxInfo lb{};
        const cv::Mat inBgr = makeInputBgr(bgr, opt_, lb);
        if (inBgr.empty()) {
            err = "preprocess_failed";
            return {};
        }

        try {
            const cv::Scalar mean(opt_.meanB, opt_.meanG, opt_.meanR);
            cv::Mat blob = cv::dnn::blobFromImage(inBgr, opt_.scale, cv::Size(opt_.inputW, opt_.inputH), mean, opt_.swapRB, false);
            net_.setInput(blob);

            std::vector<cv::Mat> outs;
            if (!spec_.outputName.empty()) {
                cv::Mat o = net_.forward(spec_.outputName);
                outs.push_back(std::move(o));
            } else {
                const auto names = net_.getUnconnectedOutLayersNames();
                if (names.empty()) {
                    cv::Mat o = net_.forward();
                    outs.push_back(std::move(o));
                } else {
                    net_.forward(outs, names);
                }
            }

            FaceDetections all;
            for (const auto& o : outs) {
                auto part = decodeOpenCvOut(o, bgr.size(), opt_, lb);
                all.insert(all.end(), part.begin(), part.end());
            }

            return nmsFaces(all, opt_.nmsIouThreshold);
        } catch (const std::exception& e) {
            err = e.what();
            return {};
        } catch (...) {
            err = "opencv_forward_unknown_error";
            return {};
        }
    }

    const char* backendName() const override { return "opencv_dnn"; }

private:
    cv::dnn::Net net_;
    YoloFaceOptions opt_{};
    YoloFaceModelSpec spec_{};
    bool loaded_ = false;
};

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
class NcnnYoloFaceDetector final : public YoloFaceDetector {
public:
    explicit NcnnYoloFaceDetector(NcnnYoloFaceModelSpec spec) : ncnnSpec_(std::move(spec)) {}

    bool load(const YoloFaceModelSpec& spec, const YoloFaceOptions& opt, std::string& err) override {
        (void)spec;
        err.clear();
        opt_ = opt;

        if (ncnnSpec_.paramPath.empty() || ncnnSpec_.binPath.empty()) {
            err = "ncnn_param_or_bin_empty";
            return false;
        }

        std::string hashBin = rk_wcfr::calculateSHA256(ncnnSpec_.binPath);
        if (hashBin.empty()) {
            std::cerr << "[Self-Check] ERROR: Failed to read NCNN YOLO bin: " << ncnnSpec_.binPath << std::endl;
            std::cerr << "[Self-Check] Please download the model and place it in the correct directory." << std::endl;
        } else {
            std::cout << "[Self-Check] Loaded NCNN YOLO bin: " << ncnnSpec_.binPath << " | SHA256: " << hashBin << std::endl;
        }

        try {
            net_.clear();
            net_.opt.use_vulkan_compute = false;
            net_.opt.num_threads = std::max(1, ncnnSpec_.threads);
            net_.opt.lightmode = ncnnSpec_.lightmode;
            if (net_.load_param(ncnnSpec_.paramPath.c_str()) != 0) {
                err = "ncnn_load_param_failed";
                return false;
            }
            if (net_.load_model(ncnnSpec_.binPath.c_str()) != 0) {
                err = "ncnn_load_model_failed";
                return false;
            }
            loaded_ = true;
            return true;
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        } catch (...) {
            err = "ncnn_load_unknown_error";
            return false;
        }
    }

    FaceDetections detect(const cv::Mat& bgr, std::string& err) override {
        err.clear();
        if (!loaded_) {
            err = "model_not_loaded";
            return {};
        }
        if (bgr.empty()) {
            err = "image_empty";
            return {};
        }

        LetterboxInfo lb{};
        const cv::Mat inBgr = makeInputBgr(bgr, opt_, lb);
        if (inBgr.empty()) {
            err = "preprocess_failed";
            return {};
        }

        try {
            ncnn::Mat in;
            if (opt_.swapRB) {
                in = ncnn::Mat::from_pixels(inBgr.data, ncnn::Mat::PIXEL_BGR2RGB, inBgr.cols, inBgr.rows);
            } else {
                in = ncnn::Mat::from_pixels(inBgr.data, ncnn::Mat::PIXEL_BGR, inBgr.cols, inBgr.rows);
            }

            if (opt_.scale != 1.0f / 255.0f || opt_.meanB != 0 || opt_.meanG != 0 || opt_.meanR != 0) {
                float meanVals[3] = {0.f, 0.f, 0.f};
                if (opt_.swapRB) {
                    meanVals[0] = static_cast<float>(opt_.meanR);
                    meanVals[1] = static_cast<float>(opt_.meanG);
                    meanVals[2] = static_cast<float>(opt_.meanB);
                } else {
                    meanVals[0] = static_cast<float>(opt_.meanB);
                    meanVals[1] = static_cast<float>(opt_.meanG);
                    meanVals[2] = static_cast<float>(opt_.meanR);
                }
                const float normVals[3] = {opt_.scale, opt_.scale, opt_.scale};
                in.substract_mean_normalize(meanVals, normVals);
            } else {
                const float meanVals[3] = {0.f, 0.f, 0.f};
                const float normVals[3] = {1.f / 255.f, 1.f / 255.f, 1.f / 255.f};
                in.substract_mean_normalize(meanVals, normVals);
            }

            ncnn::Extractor ex = net_.create_extractor();
            ex.set_num_threads(std::max(1, ncnnSpec_.threads));
            ex.set_light_mode(ncnnSpec_.lightmode);
            if (ex.input(ncnnSpec_.inputName.c_str(), in) != 0) {
                err = "ncnn_set_input_failed";
                return {};
            }
            ncnn::Mat out;
            if (ex.extract(ncnnSpec_.outputName.c_str(), out) != 0) {
                err = "ncnn_extract_failed";
                return {};
            }

            FaceDetections decoded;
            if (out.dims == 2) {
                decoded = decodeYoloLike(reinterpret_cast<const float*>(out.data), out.h, out.w, bgr.size(), opt_, lb);
            } else if (out.dims == 3) {
                const int rows = out.h;
                const int cols = out.w;
                decoded = decodeYoloLike(reinterpret_cast<const float*>(out.data), rows, cols, bgr.size(), opt_, lb);
            } else {
                err = "ncnn_output_dims_unsupported";
                return {};
            }

            return nmsFaces(decoded, opt_.nmsIouThreshold);
        } catch (const std::exception& e) {
            err = e.what();
            return {};
        } catch (...) {
            err = "ncnn_forward_unknown_error";
            return {};
        }
    }

    const char* backendName() const override { return "ncnn"; }

private:
    ncnn::Net net_;
    NcnnYoloFaceModelSpec ncnnSpec_{};
    YoloFaceOptions opt_{};
    bool loaded_ = false;
};
#endif

}  // namespace

std::unique_ptr<YoloFaceDetector> CreateOpenCvDnnYoloFaceDetector() {
    return std::make_unique<OpenCvDnnYoloFaceDetector>();
}

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
std::unique_ptr<YoloFaceDetector> CreateNcnnYoloFaceDetector(const NcnnYoloFaceModelSpec& ncnnSpec) {
    return std::make_unique<NcnnYoloFaceDetector>(ncnnSpec);
}
#endif
