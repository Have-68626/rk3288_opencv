#include "FaceRuntimeProfile.h"

#include "FileHash.h"
#include "rk_win/JsonLite.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace rk_core {

namespace {

// anchorsPerPos 每元素上限：真实 SCRFD 每位置 2 anchor，留余量取 8；
// 防下游 4*N/10*N 通道数溢出与超大 double→int 转换的未定义行为。
constexpr double kMaxAnchorsPerPos = 8.0;

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

// §I2：templateSpaceId 须恰 64 位**小写** hex（[0-9a-f]）。
// 与 isHex64（大小写不敏感，用于 sha256）刻意不同：空间 ID 会被直接拼进图库目录名，
// 只认小写可避免同一逻辑空间出现大小写两份目录。
bool isLowerHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (unsigned char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// §I2 精度字段：可选；缺省不改 out（保持调用方预设 "fp32"）；出现则仅 "fp32"/"int8"。
bool parsePrecision(const rk_win::JsonValue& o, const std::string& label, std::string& out,
                    std::string& err) {
    const rk_win::JsonValue* v = o.find("precision");
    if (!v) return true;  // 可选字段，旧 manifest 缺省走 fp32
    if (!v->isString() || (v->s != "fp32" && v->s != "int8")) {
        err = label + ".precision 非法（仅 fp32/int8）";
        return false;
    }
    out = v->s;
    return true;
}

bool getString(const rk_win::JsonValue& o, const std::string& key, std::string& out) {
    const rk_win::JsonValue* v = o.find(key);
    if (!v || !v->isString() || v->s.empty()) return false;
    out = v->s;
    return true;
}

// profileId 字符集校验——Java FaceModelManifest 的**逐字镜像**（契约见 FaceModelManifest.java 类注释：
// 字段名/缺省值/非法判定必须一致，否则会出现「Java 通过、native 拒绝」或反之）。
// 拒绝当且仅当满足任一条：
//   ① 任一字符 c 满足 c < 0x20（C0 控制字符，含 \n \r \t）或 c == 0x7F；
//   ② 首字符或末字符是空格 0x20（首尾空白）。
// **放行**：'='、内部空格、'.'、非 ASCII 等一切其它字符。**不加长度上限**。
// 注意 char 符号性：std::string 的 char 在 x86/ARM 上默认**有符号**，`c < 0x20` 对 >0x7F 的字节
// （UTF-8 续字节 0x80-0xFF）会因符号扩展成负值而误判——故这里逐字符先转 unsigned char（范围 0-255）
// 再比较，与 Java char（无符号 16 位）的语义对齐。
bool isValidProfileId(const std::string& s) {
    if (s.empty()) return false;  // 与 getString 的非空约束一致（空串另由缺字段分支拒绝）
    for (const unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    if (s.front() == ' ' || s.back() == ' ') return false;
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

// 可选 "anchorsPerPos"：恰 3 个正整数（每位置 anchor 数，P1-5）。
// 缺省返回 true 且不改 out（保持调用方预设默认）；非法长度/非正整数 → false + err。
bool parseAnchorsPerPos(const rk_win::JsonValue& o, int out[3], std::string& err) {
    const rk_win::JsonValue* v = o.find("anchorsPerPos");
    if (!v) return true;  // 可选字段
    if (!v->isArray() || v->a.size() != 3) {
        err = "detector.anchorsPerPos 必须恰 3 个正整数";
        return false;
    }
    for (std::size_t i = 0; i < v->a.size(); i++) {
        const rk_win::JsonValue& e = v->a[i];
        // 先判有限且 >0（NaN/Inf/非正均拒）且不超上限，再转 int：
        // 防超大 double（如 1e100）→int 的未定义行为，以及下游 4*N/10*N 溢出。
        // 真实 SCRFD 每位置 2 anchor，上限 8 留余量（codex 二次审计 P2）。
        if (!e.isNumber() || !std::isfinite(e.n) || !(e.n > 0.0) || e.n > kMaxAnchorsPerPos) {
            err = "detector.anchorsPerPos 元素非法（须 1..8 正整数）";
            return false;
        }
        const int n = static_cast<int>(e.n);
        if (static_cast<double>(n) != e.n) {  // 拒绝 1.5 等非整数值
            err = "detector.anchorsPerPos 元素非法（须正整数）";
            return false;
        }
        out[i] = n;
    }
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

    // §I2 新增可选字段：显式重置默认，避免复用 out 时残留上一次解析值。
    // 旧 manifest 缺键 ⇒ 双 fp32 + 空 templateSpaceId ⇒ 走旧图库路径（读取兼容，不自动升级/搬库）。
    out.detectorPrecision = "fp32";
    out.embedderPrecision = "fp32";
    out.templateSpaceId.clear();

    if (!getString(root, "profileId", out.profileId) ||
        !getString(root, "displayName", out.displayName)) {
        err = "缺少 profileId 或 displayName";
        return false;
    }
    // profileId 字符集校验（Java FaceModelManifest 镜像，见 isValidProfileId）。
    // 返回 false 与 Java 侧经 FaceModelManager 归入 Status.MANIFEST_INVALID 语义对应（C++ 无状态枚举）。
    if (!isValidProfileId(out.profileId)) {
        err = "profileId 含非法字符（控制字符或首尾空格）";
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
    // §I2 检测精度（可选；仅 fp32/int8）。一致性（须与嵌入精度相同）在下方 emb 解析后统一校验。
    if (!parsePrecision(*det, "detector", out.detectorPrecision, err)) return false;
    // 检测器输入节点缺键回落值 = "input.1"。
    // 证据：**`models/` 下全部 10 个** SCRFD .param（`models/scrfd_face_detect_ncnn/scrfd_*-opt2.param`
    // 及 int8 侧的 2 个）的 Input 层名**全部**是 input.1，与 manifest 用 sha256 钉住的
    // scrfd_500m/2.5g_kps-opt2.param 一致。（⚠️ 勿推广到 `deps/insightface/det_10g.ncnn.param`——
    // 那份未经 PNNX 转换的 ncnn 版实测是 `Input in0`。）
    // 旧值 "in0" 是 ArcFace 侧的名字（models/arcface_ncnn/arcface.param 第 3 行），曾被误抄到检测器侧；
    // 用错会让 ex.input() 失败（err="ncnn_set_input_failed"）。
    if (!getString(*det, "input", out.detectorInput)) out.detectorInput = "input.1";
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

    // P1-5 可选解码语义：缺省 delta / {1,1,1} / auto（向后兼容旧 manifest）。
    // 显式置默认，避免复用 out 时残留上一次解析值。
    out.detectorDecodeMode = FaceRuntimeProfile::DetectorDecodeMode::kDelta;
    out.detectorAnchorsPerPos[0] = 1;
    out.detectorAnchorsPerPos[1] = 1;
    out.detectorAnchorsPerPos[2] = 1;
    out.detectorOutputLayout = FaceRuntimeProfile::DetectorOutputLayout::kAuto;
    const rk_win::JsonValue* dm = det->find("decodeMode");
    if (dm) {
        if (!dm->isString() || (dm->s != "delta" && dm->s != "distance")) {
            err = "detector.decodeMode 非法（仅 delta/distance）";
            return false;
        }
        out.detectorDecodeMode = (dm->s == "distance")
                                     ? FaceRuntimeProfile::DetectorDecodeMode::kDistance
                                     : FaceRuntimeProfile::DetectorDecodeMode::kDelta;
    }
    if (!parseAnchorsPerPos(*det, out.detectorAnchorsPerPos, err)) return false;
    const rk_win::JsonValue* ol = det->find("outputLayout");
    if (ol) {
        if (!ol->isString()) {
            err = "detector.outputLayout 非字符串";
            return false;
        }
        if (ol->s == "auto") {
            out.detectorOutputLayout = FaceRuntimeProfile::DetectorOutputLayout::kAuto;
        } else if (ol->s == "nchw") {
            out.detectorOutputLayout = FaceRuntimeProfile::DetectorOutputLayout::kNChw;
        } else if (ol->s == "flat") {
            out.detectorOutputLayout = FaceRuntimeProfile::DetectorOutputLayout::kFlat;
        } else {
            err = "detector.outputLayout 非法（仅 auto/nchw/flat）";
            return false;
        }
    }

    const rk_win::JsonValue* emb = root.find("embedder");
    if (!emb || !parseFilePair(*emb, out.embedderParam, out.embedderBin)) {
        err = "embedder 配置缺失或非法";
        return false;
    }
    // §I2 嵌入精度（可选；仅 fp32/int8）。
    if (!parsePrecision(*emb, "embedder", out.embedderPrecision, err)) return false;
    // §I2 一致性判据：检测与识别精度必须一致——新 INT8 profile 要求两者皆为 int8，
    // 不允许 detector/embedder 一 int8 一 fp32 的混合（否则「INT8」名不副实且无法对照）。
    if (out.detectorPrecision != out.embedderPrecision) {
        err = "detector/embedder precision 必须一致（新 INT8 要求检测与识别都为 int8）";
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

    // §I2 独立图库空间 ID（可选顶层字段）。缺省或空串 ⇒ 保持空、走旧图库路径（读取兼容）。
    // 仅当其为非空字符串时校验：须 64 位小写 hex；否则拒绝（禁止把未校验值拼入图库路径）。
    const rk_win::JsonValue* ts = root.find("templateSpaceId");
    if (ts && ts->isString() && !ts->s.empty()) {
        if (!isLowerHex64(ts->s)) {
            err = "templateSpaceId 非法（须 64 位小写 hex）";
            return false;
        }
        out.templateSpaceId = ts->s;
    }

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

const char* FaceRuntimeManifest::templateSpaceContract() {
    // §I2 固定契约串：预处理/对齐契约的规范名。它刻画了「同一空间」的可比条件——
    // 一旦改动，按旧串算出的全部 templateSpaceId 必须视为新空间（不得原地复用旧库）。
    return "scrfd_rgb_127.5_1over128_letterbox0__arcface_rgb_127.5_1over127.5_align112_5pt_v2";
}

std::string FaceRuntimeManifest::composeTemplateSpaceHashInput(const std::string& detParamSha,
                                                              const std::string& detBinSha,
                                                              const std::string& embParamSha,
                                                              const std::string& embBinSha) {
    // 固定顺序 det_param/det_bin/emb_param/emb_bin，以 '\n' 分隔，再接契约串（同样 '\n' 分隔），
    // 整体 UTF-8（本仓全仓 UTF-8，std::string 即字节序列，无需转码）。
    // 发行方对返回值取 SHA256 小写 hex 即得 templateSpaceId；FP32 与其量化版传入同一组 FP32 哈希，
    // 故得到同一空间 ID，而 INT8 文件哈希不参与。
    std::string input;
    input.reserve(64 * 4 + 4 + 80);
    input += detParamSha;
    input += '\n';
    input += detBinSha;
    input += '\n';
    input += embParamSha;
    input += '\n';
    input += embBinSha;
    input += '\n';
    input += templateSpaceContract();
    return input;
}

bool FaceRuntimeManifest::isValidTemplateSpaceId(const std::string& s) { return isLowerHex64(s); }

}  // namespace rk_core