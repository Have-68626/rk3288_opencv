#include "adapters/MobileFaceNetAdapter.h"

#include "NativeLog.h"
#include "NcnnParamGuard.h"

using namespace rk_core;

#include <filesystem>

#include <cstring>
#include <thread>

#include <opencv2/imgproc.hpp>

namespace {
// ── 分配前尺寸闸门的上界（A2 审计 M3）──
// 人脸嵌入维度的常见量级为 128 / 256 / 512 / 1024，取 4096 留足余量。作用见 embed() 内注释。
constexpr std::size_t kMaxReasonableDim = 4096;
}  // namespace

MobileFaceNetAdapter::MobileFaceNetAdapter() = default;

MobileFaceNetAdapter::~MobileFaceNetAdapter() = default;

bool MobileFaceNetAdapter::load(const std::string& modelPath, std::string& err) {
    // 本类其余读写 `loaded_`/`currentName_` 的点**全部持锁**（`embed`/`name`/三个 getter），
    // 故 `load()` 也必须持锁 —— 否则整类仍是「写加锁、读不加锁」的半吊子状态
    // （A2 审计 M2 的同类面；子代理 3 复核时指出 `load()` 是残留的最后一处）。
    {
        std::lock_guard<std::mutex> lock(mu_);
        loaded_ = false;
    }

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    std::string paramPath;
    std::string binPath;
    std::string ext = std::filesystem::path(modelPath).extension().string();

    if (ext == ".param") {
        paramPath = modelPath;
        binPath = (std::filesystem::path(modelPath).parent_path() / std::filesystem::path(modelPath).stem().replace_extension(".bin")).string();
    } else if (ext == ".bin") {
        binPath = modelPath;
        paramPath = (std::filesystem::path(modelPath).parent_path() / std::filesystem::path(modelPath).stem().replace_extension(".param")).string();
    } else {
        // Treat bare path as base name, append .param and .bin
        paramPath = modelPath + ".param";
        binPath = modelPath + ".bin";
    }

    // S-SEC-B10-001：ncnn 20260113 受 CVE-2026-50144（.param 负 id 越界堆写）影响，
    // 前置文本校验拒绝恶意 .param，再交给 ncnn 解析（镜像上游修复语义）
    std::string paramErr;
    if (!rk_core::validateNcnnParamFile(paramPath.c_str(), paramErr)) {
        err = "mobilefacenet: param 被拒绝: " + paramErr;
        return false;
    }
    if (net_.load_param(paramPath.c_str()) != 0) {
        err = "mobilefacenet: load_param failed: " + paramPath;
        return false;
    }
    if (net_.load_model(binPath.c_str()) != 0) {
        err = "mobilefacenet: load_model failed: " + binPath;
        return false;
    }

    net_.opt.lightmode = true;
    net_.opt.num_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (net_.opt.num_threads < 1) net_.opt.num_threads = 1;

    {
        std::lock_guard<std::mutex> lock(mu_);
        currentName_ = "mobilefacenet_ncnn";
        loaded_ = true;
    }
    rklog::logInfo("MobileFaceNetAdapter", "load", "Loaded ncnn model: " + paramPath);
    return true;
#else
    (void)modelPath;
    err = "mobilefacenet: ncnn not available (RK_HAVE_NCNN=0)";
    return false;
#endif
}

std::optional<std::vector<float>> MobileFaceNetAdapter::embed(const cv::Mat& alignedFaceBgr, std::string& err) {
    // 节点名与期望维度是「加载前一次性配置、此后只读」的量：在锁内取快照后即释放锁再做推理
    // （推理耗时不短，不应全程持锁）。
    std::string inName;
    std::string outName;
    int expected = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!loaded_) {
            err = "mobilefacenet: not loaded";
            return std::nullopt;
        }
        inName = inputName_;
        outName = outputName_;
        expected = expectedDim_;
    }

#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    cv::Mat resized;
    cv::resize(alignedFaceBgr, resized, cv::Size(inputW_, inputH_), 0, 0, cv::INTER_LINEAR);

    cv::Mat src = resized;
    if (!src.isContinuous()) {
        src = src.clone();
    }

    ncnn::Mat in = ncnn::Mat::from_pixels(src.data, ncnn::Mat::PIXEL_BGR2RGB, inputW_, inputH_);

    float meanVals[3] = {0.0f, 0.0f, 0.0f};
    float normVals[3] = {1.0f / 127.5f, 1.0f / 127.5f, 1.0f / 127.5f};
    in.substract_mean_normalize(meanVals, normVals);

    ncnn::Extractor ex = net_.create_extractor();

    if (ex.input(inName.c_str(), in) != 0) {
        err = "mobilefacenet: 输入节点 \"" + inName + "\" 不存在（模型节点名与配置不符）";
        return std::nullopt;
    }

    ncnn::Mat out;
    if (ex.extract(outName.c_str(), out) != 0) {
        err = "mobilefacenet: 输出节点 \"" + outName + "\" 提取失败（模型节点名与配置不符）";
        return std::nullopt;
    }

    const std::size_t total = static_cast<std::size_t>(out.total());
    // 尺寸闸门（A2 审计 M3 提出；**子代理 4 S4 修正了它的作用范围** —— 别再把它读成「分配之前」）：
    //   · 它**挡不住** ncnn 自己的分配：上面那句 `ex.extract(outName, out)` 已经按模型输出形状
    //     分配了 `out`（而 `NcnnParamGuard` 只封 key id 与数组长度，**不封**末层 `num_output`）；
    //   · 它真正界定的是**第二份**分配 —— 下面 `std::vector<float> embedding(total)` 的复制缓冲 ——
    //     以及 memcpy 的长度上界。旧代码那个硬编码的 `128` **顺带**起过同样作用，本次把它移除，
    //     故必须显式补回，但**不再**与「必须是 128」绑死。
    //   · `setExpectedEmbeddingDim(<=0)` **只关「相等性」校验，不关这道闸门**（闸门在它之前、无条件执行）。
    // ⚠️ 附带限制：该上界是编译期常量，调用方**无法上调**；若将来真有 >4096 维的模型，须同步改这里。
    if (total == 0 || total > kMaxReasonableDim) {
        err = "mobilefacenet: 输出维度异常——实际 " + std::to_string(total) + "（节点 " + outName +
              "）；超出可接受范围 [1, " + std::to_string(kMaxReasonableDim) + "]";
        return std::nullopt;
    }
    if (expected > 0 && total != static_cast<std::size_t>(expected)) {
        err = "mobilefacenet: 输出维度不符——期望 " + std::to_string(expected) +
              "，实际 " + std::to_string(total) + "（节点 " + outName +
              "）；请核对模型 .param 或调用 setExpectedEmbeddingDim()";
        return std::nullopt;
    }

    std::vector<float> embedding(total);
    std::memcpy(embedding.data(), (const float*)out.data, total * sizeof(float));

    // L2 normalize for correct cosine similarity comparison
    float sqSum = 0.0f;
    for (float v : embedding) sqSum += v * v;
    if (sqSum > 0.0f) {
        float inv = 1.0f / std::sqrt(sqSum);
        for (float& v : embedding) v *= inv;
    }

    return embedding;
#else
    // ⚠️ 本分支**当前不可达**（子代理 3 复核时实测）：非 ncnn 构建下 `load()` 走它自己的 `#else`，
    //    从不置 `loaded_ = true`，故 `embed()` 在开头的 `!loaded_` 检查处即返回 `"...: not loaded"`。
    //    保留它是为了让分支自描述（防备 `load()` 将来改行为）；其 err 文本经 `normalizeReasonCode`
    //    会归一为 `runtime_init_failed`，与改前「err 保持为空」的对外可见结果**完全一致**
    //    （A2-L3 因此判为不成立 —— 且比主会话当时掌握的还多一层：该分支根本到不了）。
    (void)alignedFaceBgr;
    (void)inName;
    (void)outName;
    (void)expected;
    err = "mobilefacenet: ncnn not available (RK_HAVE_NCNN=0)";
    return std::nullopt;
#endif
}

const char* MobileFaceNetAdapter::name() const {
    std::lock_guard<std::mutex> lock(mu_);
    return currentName_.c_str();
}

void MobileFaceNetAdapter::setNodeNames(const std::string& input, const std::string& output) {
    std::lock_guard<std::mutex> lock(mu_);
    // 空串忽略：调用方只提供其中一个时不清空另一个（与 ArcFaceAdapter 同语义）。
    if (!input.empty()) inputName_ = input;
    if (!output.empty()) outputName_ = output;
}

// 三个 getter 一律「持锁 + 按值返回」：本类的 setter 持锁写，若 getter 读时不上锁、或退内部引用，
// 就构成「写加锁、读不加锁」的半吊子线程安全（比全不加锁更危险）——A2 审计 M2。
// ⚠️ 维度类 getter 对「<=0 ⇒ 不校验相等性」的配置**回落到默认维度**，绝不返回 0（子代理 4 S5）。
std::string MobileFaceNetAdapter::inputNodeName() const {
    std::lock_guard<std::mutex> lock(mu_);
    return inputName_;
}

std::string MobileFaceNetAdapter::outputNodeName() const {
    std::lock_guard<std::mutex> lock(mu_);
    return outputName_;
}

void MobileFaceNetAdapter::setExpectedEmbeddingDim(int dim) {
    std::lock_guard<std::mutex> lock(mu_);
    expectedDim_ = dim;
}

int MobileFaceNetAdapter::expectedEmbeddingDim() const {
    std::lock_guard<std::mutex> lock(mu_);
    return expectedDim_ > 0 ? expectedDim_ : kDefaultEmbeddingDim;
}

int MobileFaceNetAdapter::embeddingDim() const {
    std::lock_guard<std::mutex> lock(mu_);
    return expectedDim_ > 0 ? expectedDim_ : kDefaultEmbeddingDim;
}
