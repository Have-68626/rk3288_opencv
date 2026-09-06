#include "FaceRuntimeProfile.h"

#include "FileHash.h"
#include "rk_win/JsonLite.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace rk_core {

namespace {

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

std::string toLowerHex(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool getString(const rk_win::JsonValue& o, const std::string& key, std::string& out) {
    const rk_win::JsonValue* v = o.find(key);
    if (!v || !v->isString() || v->s.empty()) return false;
    out = v->s;
    return true;
}

bool parseEntry(const rk_win::JsonValue& v, FaceModelManifestEntry& e) {
    if (!v.isObject()) return false;
    if (!getString(v, "path", e.relativePath)) return false;
    const rk_win::JsonValue* h = v.find("sha256");
    if (h) {
        if (!h->isString()) return false;
        e.sha256 = h->s;
    }
    return true;
}

bool parseFilePair(const rk_win::JsonValue& obj, FaceModelManifestEntry& param,
                   FaceModelManifestEntry& bin) {
    if (!obj.isObject()) return false;
    const rk_win::JsonValue* p = obj.find("param");
    const rk_win::JsonValue* b = obj.find("bin");
    if (!p || !b) return false;
    return parseEntry(*p, param) && parseEntry(*b, bin);
}

bool parsePositiveInt(const rk_win::JsonValue& o, const std::string& key, int& out) {
    const rk_win::JsonValue* v = o.find(key);
    if (!v || !v->isNumber()) return false;
    const int x = static_cast<int>(v->n);
    if (x <= 0) return false;
    out = x;
    return true;
}

bool parseThreshold(const rk_win::JsonValue& o, const std::string& key, float& out) {
    const rk_win::JsonValue* v = o.find(key);
    if (!v || !v->isNumber()) return false;
    const float x = static_cast<float>(v->n);
    if (!(x > 0.0f && x <= 1.0f)) return false;  // ≤0 或 >1 均非法（NaN 亦然）
    out = x;
    return true;
}

}  // namespace

bool FaceRuntimeManifest::isSafeRelativePath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return false;                     // 绝对路径
    if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') {
        return false;  // Windows 盘符
    }
    std::string seg;
    for (char ch : p) {
        if (ch == '/' || ch == '\\') {
            if (seg == "..") return false;  // 父目录段
            seg.clear();
        } else {
            seg += ch;
        }
    }
    if (seg == "..") return false;
    return true;
}

bool FaceRuntimeManifest::parseV1(const std::string& jsonText, const std::string& currentAbi,
                                  FaceRuntimeProfile& out, std::string& err) {
    err.clear();
    rk_win::JsonValue root;
    if (!rk_win::parseJson(jsonText, root, err)) return false;
    if (!root.isObject()) {
        err = "manifest 顶层非对象";
        return false;
    }

    const rk_win::JsonValue* sv = root.find("schemaVersion");
    if (!sv || !sv->isNumber() || sv->n != 1.0) {  // 精确 1.0，拒绝 1.5 等截断（A2 LOW-2）
        err = "未知 manifest schema 版本（仅支持 v1）";
        return false;
    }
    out.schemaVersion = 1;

    if (!getString(root, "profileId", out.profileId) ||
        !getString(root, "displayName", out.displayName)) {
        err = "缺少 profileId 或 displayName";
        return false;
    }

    const rk_win::JsonValue* abis = root.find("supportedAbis");
    if (!abis || !abis->isArray() || abis->a.empty()) {
        err = "缺少 supportedAbis";
        return false;
    }
    bool abiOk = false;
    for (const auto& a : abis->a) {
        if (a.isString()) out.supportedAbis.push_back(a.s);
        if (a.isString() && a.s == currentAbi) abiOk = true;
    }
    if (!abiOk) {
        err = "ABI 不匹配（当前 " + currentAbi + " 不在 supportedAbis）";
        return false;
    }

    const rk_win::JsonValue* det = root.find("detector");
    if (!det || !parseFilePair(*det, out.detectorParam, out.detectorBin)) {
        err = "detector 配置缺失或非法";
        return false;
    }
    if (!getString(*det, "input", out.detectorInput)) out.detectorInput = "in0";
    const rk_win::JsonValue* detOuts = det->find("outputs");
    if (detOuts) {
        if (!detOuts->isArray()) {
            err = "detector.outputs 非数组";
            return false;
        }
        out.detectorOutputs.clear();
        for (const auto& n : detOuts->a) {
            if (!n.isString()) {
                err = "detector.outputs 含非字符串";
                return false;
            }
            out.detectorOutputs.push_back(n.s);
        }
        if (out.detectorOutputs.size() != 9) {
            err = "detector.outputs 必须恰 9 个（score/bbox/kps × 3）";
            return false;
        }
    }
    if (!parsePositiveInt(*det, "inputWidth", out.detectorInputW)) out.detectorInputW = 640;
    if (!parsePositiveInt(*det, "inputHeight", out.detectorInputH)) out.detectorInputH = 640;

    const rk_win::JsonValue* emb = root.find("embedder");
    if (!emb || !parseFilePair(*emb, out.embedderParam, out.embedderBin)) {
        err = "embedder 配置缺失或非法";
        return false;
    }
    if (!getString(*emb, "input", out.embedderInput)) out.embedderInput = "in0";
    if (!getString(*emb, "output", out.embedderOutput)) out.embedderOutput = "out0";
    if (!parsePositiveInt(*emb, "inputWidth", out.embedderInputW)) out.embedderInputW = 112;
    if (!parsePositiveInt(*emb, "inputHeight", out.embedderInputH)) out.embedderInputH = 112;
    if (!parsePositiveInt(*emb, "embeddingDim", out.embeddingDim)) out.embeddingDim = 512;

    // 路径安全
    const FaceModelManifestEntry* entries[4] = {
        &out.detectorParam, &out.detectorBin, &out.embedderParam, &out.embedderBin};
    for (const auto* e : entries) {
        if (!isSafeRelativePath(e->relativePath)) {
            err = "非法模型路径（绝对或含 ..）：" + e->relativePath;
            return false;
        }
        if (!e->sha256.empty() && !isHex64(e->sha256)) {
            err = "sha256 非 64 位 hex：" + e->relativePath;
            return false;
        }
    }

    // 阈值（计划固定值；NaN/≤0/>1 均拒绝）
    if (!parseThreshold(root, "detectionConfidence", out.detectionConfidence) ||
        !parseThreshold(root, "nmsIou", out.nmsIou) ||
        !parseThreshold(root, "identitySimilarity", out.identitySimilarity)) {
        err = "阈值非法（须 >0 且 <=1）";
        return false;
    }

    if (!getString(root, "modelVersion", out.modelVersion)) out.modelVersion = "1";
    if (!getString(root, "licenseId", out.licenseId)) out.licenseId = "";

    // 校验条目 sha256 已 hex（上文）；schema/维度已检
    err.clear();
    return true;
}

bool FaceRuntimeManifest::fileIsLfsPointer(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char buf[512] = {0};
    in.read(buf, static_cast<std::streamsize>(sizeof(buf) - 1));
    const std::string head(buf, static_cast<std::size_t>(in.gcount()));
    return head.find("version https://git-lfs.github.com/spec/v1") != std::string::npos;
}

bool FaceRuntimeManifest::verifyFiles(const FaceRuntimeProfile& profile,
                                      const std::string& baseDir, std::string& err) {
    err.clear();
    const FaceModelManifestEntry* entries[4] = {
        &profile.detectorParam, &profile.detectorBin,
        &profile.embedderParam, &profile.embedderBin};
    for (const auto* e : entries) {
        // 防御：即使绕过 parseV1 直接构造 profile 调 verifyFiles 也重校路径（A2 MED-1）
        if (!isSafeRelativePath(e->relativePath)) {
            err = "非法模型路径（绝对或含 ..）：" + e->relativePath;
            return false;
        }
        const std::filesystem::path full = std::filesystem::path(baseDir) / e->relativePath;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(full, ec) || ec) {
            err = "缺失模型文件: " + e->relativePath;
            return false;
        }
        if (fileIsLfsPointer(full.string())) {
            err = "模型文件是 Git LFS pointer（未拉取）: " + e->relativePath;
            return false;
        }
        if (!e->sha256.empty()) {
            const std::string h = rk_wcfr::calculateSHA256(full);
            if (h.empty() || toLowerHex(h) != toLowerHex(e->sha256)) {  // 大小写不敏感（A2 LOW-1）
                err = "SHA-256 不匹配: " + e->relativePath;
                return false;
            }
        }
    }
    return true;
}

}  // namespace rk_core