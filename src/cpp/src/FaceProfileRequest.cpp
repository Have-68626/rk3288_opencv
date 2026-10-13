#include "FaceProfileRequest.h"

#include "rk_win/JsonLite.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace rk_core {

namespace {

// 读整份文本文件；失败填 err、返回空串。
std::string readTextFile(const std::filesystem::path& p, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        err = "manifest_open_failed";
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) {
        err = "manifest_read_failed";
        return {};
    }
    return ss.str();
}

// 从 manifest 原文取某子对象的 precision（仅 "fp32"/"int8"，其余/缺键一律回落 "fp32"）。
// 直接在原文上取：FaceRuntimeProfile 的 precision 字段即使由 parseV1 填充，此处亦显式以原文为准，
// 避免依赖 parseV1 的实现细节（旧 manifest 缺键 ⇒ fp32）。
std::string readPrecision(const std::string& jsonText, const char* section) {
    rk_win::JsonValue root;
    std::string perr;
    if (!rk_win::parseJson(jsonText, root, perr) || !root.isObject()) return "fp32";
    const rk_win::JsonValue* sec = root.find(section);
    if (!sec || !sec->isObject()) return "fp32";
    const rk_win::JsonValue* pv = sec->find("precision");
    if (!pv || !pv->isString()) return "fp32";
    return pv->s == "int8" ? "int8" : "fp32";
}

// manifest 的 modelVersion 是字符串（如 "1"）；非数字/负数一律回落 1（与旧默认一致）。
std::uint32_t parseModelVersion(const std::string& s) {
    if (s.empty()) return 1;
    try {
        const long v = std::stol(s);
        if (v < 0) return 1;
        return static_cast<std::uint32_t>(v);
    } catch (...) {
        return 1;
    }
}

}  // namespace

bool applyFaceProfileToRequest(const std::string& profileDir, const std::string& currentAbi,
                               FaceInferRequest& req, FaceRuntimeProfile& profile, std::string& err) {
    err.clear();
    if (profileDir.empty()) {
        err = "profile_dir_empty";
        return false;
    }

    const std::filesystem::path base(profileDir);
    const std::filesystem::path manifestPath = base / "manifest.json";
    std::string readErr;
    const std::string text = readTextFile(manifestPath, readErr);
    if (!readErr.empty()) {
        err = readErr;
        return false;
    }
    if (text.empty()) {
        err = "manifest_empty";
        return false;
    }

    // 用独立对象解析，避免调用方复用的 profile 残留上一次的 supportedAbis 等累积状态。
    FaceRuntimeProfile parsed;
    if (!FaceRuntimeManifest::parseV1(text, currentAbi, parsed, err)) return false;
    if (!FaceRuntimeManifest::verifyFiles(parsed, base.string(), err)) return false;

    // precision 直接取自 manifest 原文（不依赖 FaceRuntimeProfile 是否有该字段：旧 manifest/旧结构
    // 均缺键 ⇒ fp32）。
    const std::string detPrecision = readPrecision(text, "detector");
    const std::string embPrecision = readPrecision(text, "embedder");

    const std::string detParam = (base / parsed.detectorParam.relativePath).string();
    const std::string detBin = (base / parsed.detectorBin.relativePath).string();
    const std::string embParam = (base / parsed.embedderParam.relativePath).string();
    const std::string embBin = (base / parsed.embedderBin.relativePath).string();

    // 全部派生先落到局部 eff，最后一次性提交（保证失败路径不改 req）。
    FaceInferRequest eff = req;
    eff.detectorId = "scrfd_0.5gf";
    eff.embedderId = "arcface";
    eff.yoloBackend = "ncnn";
    eff.arcBackend = "ncnn";
    eff.yoloModelPath = detParam;
    eff.yoloNcnnParam = detParam;
    eff.yoloNcnnBin = detBin;
    eff.yoloNcnnInput = parsed.detectorInput;
    if (!parsed.detectorOutputs.empty()) eff.yoloNcnnOutput = parsed.detectorOutputs.front();
    eff.yoloInputW = parsed.detectorInputW;
    eff.yoloInputH = parsed.detectorInputH;
    eff.yoloScoreThreshold = parsed.detectionConfidence;
    eff.yoloNmsIouThreshold = parsed.nmsIou;
    if (!eff.acceptThresholdExplicit) eff.acceptThreshold = parsed.identitySimilarity;
    eff.arcModelPath = embParam;
    eff.arcNcnnParam = embParam;
    eff.arcNcnnBin = embBin;
    eff.arcNcnnInput = parsed.embedderInput;
    eff.arcNcnnOutput = parsed.embedderOutput;
    eff.arcInputW = parsed.embedderInputW;
    eff.arcInputH = parsed.embedderInputH;
    eff.arcModelVersion = parseModelVersion(parsed.modelVersion);
    // 双 INT8 profile 自动要求量化；调用方显式要求也必须保留，不能被 FP32 声明降级。
    eff.int8Enabled = eff.int8Enabled || (detPrecision == "int8" && embPrecision == "int8");

    req = eff;
    profile = parsed;
    return true;
}

} // namespace rk_core
