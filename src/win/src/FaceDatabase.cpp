#include "rk_win/FaceDatabase.h"

#include "rk_win/WinCrypto.h"

#include <opencv2/core.hpp>
#include <opencv2/core/persistence.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace rk_win {
namespace {

// ---- 加密容器格式（落盘二进制，非明文）----
// magic(8) "RKFDBCRY" + version(1) + nonceLen(1) + tagLen(1) + nonce(12) + tag(16) + ciphertext(...)
// AAD = 前 11 字节头部：绑定密文与 magic/版本，防头部篡改被接受（GCM tag 校验）。
constexpr std::uint8_t kFaceDbMagic[8] = {'R', 'K', 'F', 'D', 'B', 'C', 'R', 'Y'};
constexpr std::uint8_t kFaceDbVersion = 1;
constexpr std::size_t kGcmNonceLen = 12;
constexpr std::size_t kGcmTagLen = 16;

// ---- 明文载荷格式（仅存在于内存，加解密后不落盘）----
// 首行签名 "RKFDBSER1"，次行 <numPersons>，随后每行一条人员：
//   <hexId> <count> <dim> <hexFloat0> <hexFloat1> ... <hexFloat(dim-1)>
// id 按 UTF-8 字节 hex 编码（任意字符安全）；mean 按 IEEE-754 bit 模式 hex 编码（无损往返）。
constexpr const char* kFaceDbPayloadSig = "RKFDBSER1";

std::string makeHeader() {
    std::string h;
    h.reserve(11);
    h.append(reinterpret_cast<const char*>(kFaceDbMagic), sizeof(kFaceDbMagic));
    h.push_back(static_cast<char>(kFaceDbVersion));
    h.push_back(static_cast<char>(kGcmNonceLen));
    h.push_back(static_cast<char>(kGcmTagLen));
    return h;
}

std::string hexEncode(const std::uint8_t* data, std::size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; i++) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

bool hexDecode(const std::string& hex, std::vector<std::uint8_t>& out) {
    out.clear();
    if (hex.size() % 2 != 0) return false;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string floatToHex(float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned int>(bits));
    return buf;
}

bool hexToFloat(const std::string& s, float& f) {
    if (s.size() != 8) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (end != s.c_str() + s.size()) return false;
    const unsigned int bits = static_cast<unsigned int>(v);
    std::memcpy(&f, &bits, sizeof(f));
    return true;
}

// 序列化人员表为明文载荷（内存内字符串，不落盘）。
std::string serializePayload(const std::unordered_map<std::string, PersonEntry>& persons) {
    std::ostringstream ss;
    ss << kFaceDbPayloadSig << '\n' << persons.size() << '\n';
    for (const auto& kv : persons) {
        const PersonEntry& e = kv.second;
        ss << hexEncode(reinterpret_cast<const std::uint8_t*>(e.id.data()), e.id.size())
           << ' ' << e.count << ' ' << e.mean.size();
        for (const float f : e.mean) ss << ' ' << floatToHex(f);
        ss << '\n';
    }
    return ss.str();
}

bool deserializePayload(const std::string& text,
                        std::unordered_map<std::string, PersonEntry>& persons) {
    persons.clear();
    std::istringstream ss(text);
    std::string sig;
    if (!(ss >> sig) || sig != kFaceDbPayloadSig) return false;
    std::size_t num = 0;
    if (!(ss >> num)) return false;
    for (std::size_t i = 0; i < num; i++) {
        std::string idHex;
        std::int64_t count = 0;
        std::size_t dim = 0;
        if (!(ss >> idHex >> count >> dim)) return false;
        if (dim > 65536) return false;  // 防御：异常维度拒绝，避免异常分配
        std::vector<std::uint8_t> idBytes;
        if (!hexDecode(idHex, idBytes) || idBytes.empty()) return false;
        PersonEntry e;
        e.id.assign(reinterpret_cast<const char*>(idBytes.data()), idBytes.size());
        e.count = count;
        e.mean.reserve(dim);
        for (std::size_t d = 0; d < dim; d++) {
            std::string fhex;
            if (!(ss >> fhex)) return false;
            float f = 0.f;
            if (!hexToFloat(fhex, f)) return false;
            e.mean.push_back(f);
        }
        if (e.id.empty() || e.mean.empty()) return false;
        persons.emplace(e.id, std::move(e));
    }
    return true;
}

// 读取 DPAPI 保护密钥文件（base64(DPAPI(32B key))），不创建。
bool loadKey(const std::filesystem::path& keyPath, std::vector<std::uint8_t>& keyOut, std::string& err) {
    err.clear();
    keyOut.clear();
    std::ifstream ifs(keyPath, std::ios::binary);
    if (!ifs) { err = "密钥文件读取失败"; return false; }
    std::string b64((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> protectedBin;
    if (!base64Decode(b64, protectedBin)) { err = "密钥文件 base64 解码失败"; return false; }
    if (!dpapiUnprotect(protectedBin, keyOut, err)) return false;
    if (keyOut.size() != 32) { err = "密钥长度异常（期望 32 bytes）"; keyOut.clear(); return false; }
    return true;
}

// 原子替换：临时文件 → 目标路径（定义见下，先声明供 ensureOrLoadKey 使用）。
bool replaceFile(const std::filesystem::path& src, const std::filesystem::path& dst, std::string& err);

// 读取或创建 DPAPI 保护密钥文件（与 config.key.dpapi 布局一致）。
bool ensureOrLoadKey(const std::filesystem::path& keyPath, std::vector<std::uint8_t>& keyOut, std::string& err) {
    err.clear();
    if (std::filesystem::exists(keyPath)) return loadKey(keyPath, keyOut, err);

    if (!randomBytes(32, keyOut, err)) return false;
    std::vector<std::uint8_t> protectedBin;
    if (!dpapiProtect(keyOut, protectedBin, err)) return false;
    std::string b64;
    if (!base64Encode(protectedBin, b64)) { err = "密钥 base64 编码失败"; return false; }

    std::error_code ec;
    std::filesystem::create_directories(keyPath.parent_path(), ec);
    if (ec) { err = "创建密钥目录失败"; return false; }

    // 临时文件 + 原子替换：防止密钥文件半写导致后续解密失败。
    const std::filesystem::path tmp = std::filesystem::path(keyPath.string() + ".tmp");
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) { err = "密钥文件写入失败"; return false; }
        ofs.write(b64.data(), static_cast<std::streamsize>(b64.size()));
        ofs.flush();
        if (!ofs.good()) { err = "密钥文件写入失败"; return false; }
    }
    if (!replaceFile(tmp, keyPath, err)) {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return false;
    }
    return true;
}

// 原子替换：临时文件 → 目标路径（Windows MoveFileExW WRITE_THROUGH，防断电半写）。
bool replaceFile(const std::filesystem::path& src, const std::filesystem::path& dst, std::string& err) {
#ifdef _WIN32
    err.clear();
    if (MoveFileExW(src.wstring().c_str(), dst.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        err = "原子替换失败";
        return false;
    }
    return true;
#else
    (void)src;
    (void)dst;
    err = "平台不支持原子替换";
    return false;
#endif
}

// 加密并写容器文件（AAD = 11 字节头部）。
bool encryptToFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& plain,
                   const std::vector<std::uint8_t>& key, std::string& err) {
    err.clear();
    AesGcmCiphertext ct;
    const std::string header = makeHeader();
    if (!aes256gcmEncrypt(key, plain, ct, err, header)) return false;
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) { err = "数据库文件写入失败"; return false; }
    ofs.write(header.data(), static_cast<std::streamsize>(header.size()));
    ofs.write(reinterpret_cast<const char*>(ct.nonce.data()), static_cast<std::streamsize>(ct.nonce.size()));
    ofs.write(reinterpret_cast<const char*>(ct.tag.data()), static_cast<std::streamsize>(ct.tag.size()));
    ofs.write(reinterpret_cast<const char*>(ct.ciphertext.data()), static_cast<std::streamsize>(ct.ciphertext.size()));
    ofs.flush();
    if (!ofs.good()) { err = "数据库文件写入失败"; return false; }
    return true;
}

// 读取加密容器（校验 magic/版本/参数，解出 nonce/tag/ciphertext）。
bool readContainer(const std::filesystem::path& path, std::vector<std::uint8_t>& nonce,
                   std::vector<std::uint8_t>& tag, std::vector<std::uint8_t>& ciphertext, std::string& err) {
    err.clear();
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) { err = "数据库文件读取失败"; return false; }
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    const std::string header = makeHeader();
    if (file.size() < header.size() + kGcmNonceLen + kGcmTagLen) { err = "加密文件尺寸过短"; return false; }
    if (!std::equal(header.begin(), header.end(), file.begin())) { err = "加密文件头部不匹配"; return false; }
    nonce.assign(file.begin() + static_cast<std::ptrdiff_t>(header.size()),
                 file.begin() + static_cast<std::ptrdiff_t>(header.size() + kGcmNonceLen));
    tag.assign(file.begin() + static_cast<std::ptrdiff_t>(header.size() + kGcmNonceLen),
               file.begin() + static_cast<std::ptrdiff_t>(header.size() + kGcmNonceLen + kGcmTagLen));
    ciphertext.assign(file.begin() + static_cast<std::ptrdiff_t>(header.size() + kGcmNonceLen + kGcmTagLen),
                      file.end());
    return true;
}

// A11：回读**实际磁盘密文**并用原解密解析链还原，再把完整记录与期望**逐项逐值**比对。
//   判据是**内容一致**（人员集合 / count / 维度 / 每个 float 的 IEEE-754 bit），**不是**余弦相似。
//   返回 false 时 errOut 说明原因（**不含**人员身份明文，只用数量/字段名，避免把身份写进诊断）。
bool encryptedPayloadMatches(const std::filesystem::path& path,
                             const std::unordered_map<std::string, PersonEntry>& expected,
                             const std::vector<std::uint8_t>& key, std::string& errOut) {
    errOut.clear();
    std::vector<std::uint8_t> nonce, tag, ciphertext;
    if (!readContainer(path, nonce, tag, ciphertext, errOut)) return false;
    AesGcmCiphertext ct{std::move(nonce), std::move(ciphertext), std::move(tag)};
    std::vector<std::uint8_t> plain;
    if (!aes256gcmDecrypt(key, ct, plain, errOut, makeHeader())) return false;
    const std::string payload(reinterpret_cast<const char*>(plain.data()), plain.size());
    std::unordered_map<std::string, PersonEntry> got;
    if (!deserializePayload(payload, got)) {
        errOut = "载荷反序列化失败";
        return false;
    }
    if (got.size() != expected.size()) {
        errOut = "人员数量不一致";
        return false;
    }
    for (const auto& kv : expected) {
        const auto it = got.find(kv.first);
        if (it == got.end()) {
            errOut = "存在缺失记录";
            return false;
        }
        const PersonEntry& a = kv.second;
        const PersonEntry& b = it->second;
        if (a.count != b.count) { errOut = "记录 count 不一致"; return false; }
        if (a.mean.size() != b.mean.size()) { errOut = "记录维度不一致"; return false; }
        for (std::size_t i = 0; i < a.mean.size(); ++i) {
            std::uint32_t x = 0, y = 0;
            std::memcpy(&x, &a.mean[i], sizeof(x));
            std::memcpy(&y, &b.mean[i], sizeof(y));
            if (x != y) { errOut = "记录特征值不一致（非逐值等价）"; return false; }
        }
    }
    return true;
}

}  // namespace

std::filesystem::path FaceDatabase::keyPathFor(const std::filesystem::path& dbPath) {
    return std::filesystem::path(dbPath.string() + ".key.dpapi");
}

bool FaceDatabase::load(const std::filesystem::path& path) {
    if (path.empty()) return false;
    // A11：每次 load 先复位迁移结果（仅明文分支才可能改写为正结果；加密/不存在/失败均为 NotAttempted）。
    {
        std::lock_guard<std::mutex> lock(mu_);
        migrationOutcome_ = MigrationOutcome::NotAttempted;
        migrationDetail_.clear();
    }
    if (!std::filesystem::exists(path)) {
        std::lock_guard<std::mutex> lock(mu_);
        persons_.clear();
        return true;
    }

    // 格式判别：前 8 字节 == magic → 加密容器；否则 → 旧明文 YAML（触发迁移）。
    bool encrypted = false;
    {
        std::ifstream ifs(path, std::ios::binary);
        char head[8] = {0};
        if (ifs) ifs.read(head, sizeof(head));
        encrypted = std::equal(kFaceDbMagic, kFaceDbMagic + sizeof(kFaceDbMagic), head);
    }

    std::unordered_map<std::string, PersonEntry> tmp;
    // A11：迁移结果暂存（在末尾统一提交到成员，与 persons_ 同处 mu_ 临界区）。
    MigrationOutcome migOutcome = MigrationOutcome::NotAttempted;
    std::string migDetail;
    if (encrypted) {
        std::vector<std::uint8_t> nonce, tag, ciphertext, key;
        std::string err;
        if (!readContainer(path, nonce, tag, ciphertext, err)) {
            std::lock_guard<std::mutex> lock(mu_);
            persons_.clear();
            return false;
        }
        {
            // 加密库必须用既有密钥解密：密钥文件缺失/损坏 → 受控失败，不新建密钥强行解密。
            std::lock_guard<std::mutex> lock(keyMu_);
            if (!loadKey(keyPathFor(path), key, err)) {
                std::lock_guard<std::mutex> lock(mu_);
                persons_.clear();
                return false;
            }
        }
        AesGcmCiphertext ct{std::move(nonce), std::move(ciphertext), std::move(tag)};
        std::vector<std::uint8_t> plain;
        if (!aes256gcmDecrypt(key, ct, plain, err, makeHeader())) {
            // 错误密钥/密文被篡改：GCM tag 校验失败 → 受控失败（不崩溃、不写明文、数据不可读）。
            std::lock_guard<std::mutex> lock(mu_);
            persons_.clear();
            return false;
        }
        const std::string payload(reinterpret_cast<const char*>(plain.data()), plain.size());
        if (!deserializePayload(payload, tmp)) {
            std::lock_guard<std::mutex> lock(mu_);
            persons_.clear();
            return false;
        }
    } else {
        // 旧明文 YAML：先读明文（兼容既有库），再尝试加密迁移（失败/中止保留旧库，可回滚）。
        //
        // ⚠️ **本段必须整体受异常保护**（2026-09-26 二次审计登记、本轮修）：本分支是 `load()` 里
        //    唯一触碰 OpenCV **解析**的面，而它的操作**是抛异常的**（不是返回错误码）。
        //    ★ 精确机制（验收 A1 用独立探针实测后订正；我原先写的「序列元素不是 map」**不成立**——
        //      实测 `persons` 的元素 **isMap=1（是 map）**）：
        //      ①② 两个畸形臂**都栽在 `mean` 这一个字段上**，只是经由不同代码：
        //        - `mean` 是**标量**（如 `mean: 3.14`）时：`(*it)["mean"] >> meanMat` 进入
        //          `cv::read(const FileNode&, Mat&, …)`，其**第一条语句**读 `node["dt"]` ⇒ 对**非 map**
        //          节点调 `cv::FileNode::operator[]` ⇒ 抛 `isMap()`（实测文本：
        //          `OpenCV(4.10.0) … persistence.cpp:2171: error: (-215:Assertion failed) isMap() in
        //           function 'cv::FileNode::operator []'`，本机 4.10.0 原样复现）；
        //        - `mean` 是**CV_64F 矩阵**（结构合法、类型不符）时：不抛，落到下面的**显式校验**。
        //    修前：上述①会**抛穿** `load()`（无 try/catch），而 `load()` 的契约是「受控返回 false 并
        //    清空内存库」（见 `FaceDatabase.h` 的 javadoc），调用链
        //    （`ArcFaceWinRecognizer::initialize` → `RuntimeBootstrap::build`）**没有** try/catch
        //    ⇒ 会在**装配期**直接抛穿。
        //    ⇒ 现口径：本分支的一切 **`cv::Exception`** 等价于「库不可读」，与
        //      「读不开 / 解密失败 / 反序列化失败」同一处置（清空内存库 + 返回 false）。
        //    ⚠️ **代价（不得隐瞒，验收 A2 指出）**：① 该 catch **只兜 `cv::Exception`** ——
        //      `std::bad_alloc` / `std::length_error`（`e.mean.resize`、`tmp.emplace`）与
        //      `std::filesystem` 异常**仍会抛穿**（修前是「全都不兜」，故不构成回归，但**不得**把
        //      现口径写成「对一切可预期的坏库都是受控返回」）；
        //      ② `cv::Exception` 也覆盖 `cv::Error::OutOfMemoryError` 等 ⇒ 真 OOM / 内部 bug 会被
        //      报成「库不可读」，且本函数**没有日志依赖**（`FaceDatabase.cpp` 无 `rklog`）⇒
        //      坏库与错密钥在诊断上**不可区分**。收窄它需引入日志依赖，**不在本轮改动面**。
        try {
            cv::FileStorage fs(path.string(), cv::FileStorage::READ);
            if (!fs.isOpened()) {
                std::lock_guard<std::mutex> lock(mu_);
                persons_.clear();
                return false;
            }
            cv::FileNode persons = fs["persons"];
            if (persons.type() == cv::FileNode::SEQ) {
                for (auto it = persons.begin(); it != persons.end(); ++it) {
                    PersonEntry e;
                    (*it)["id"] >> e.id;
                    int count = 0;
                    (*it)["count"] >> count;
                    e.count = count;
                    cv::Mat meanMat;
                    (*it)["mean"] >> meanMat;
                    meanMat = meanMat.reshape(1, 1);
                    e.mean.resize(static_cast<std::size_t>(meanMat.cols));
                    // 修前此处是 `CV_Assert(...)`：畸形条目 ⇒ 抛 cv::Exception 穿出 `load()`。
                    // 现改为**显式校验**：不满足契约即判该库不可读（不抛、不留半截状态）。
                    if (meanMat.type() != CV_32F || !meanMat.isContinuous() ||
                        e.mean.size() != static_cast<std::size_t>(meanMat.total())) {
                        std::lock_guard<std::mutex> lock(mu_);
                        persons_.clear();
                        return false;
                    }
                    std::copy(meanMat.ptr<float>(), meanMat.ptr<float>() + meanMat.total(), e.mean.begin());
                    if (!e.id.empty() && !e.mean.empty()) {
                        tmp.emplace(e.id, std::move(e));
                    }
                }
            }
        } catch (const cv::Exception&) {
            // 兜住本段其余 cv:: 抛出点（FileStorage 解析畸形 YAML、`>>` 类型不匹配等）。
            std::lock_guard<std::mutex> lock(mu_);
            persons_.clear();
            return false;
        }
        // `migratePlaintextToEncrypted` 不使用 OpenCV（纯加密 + 文件 IO，自带回滚）⇒ 刻意留在 try 之外。
        std::string migErr;
        migOutcome = migratePlaintextToEncrypted(path, tmp, migErr);
        migDetail = migErr;
    }

    std::lock_guard<std::mutex> lock(mu_);
    persons_ = std::move(tmp);
    migrationOutcome_ = migOutcome;
    migrationDetail_ = migDetail;
    return true;
}

bool FaceDatabase::save(const std::filesystem::path& path) const {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::unordered_map<std::string, PersonEntry> snap;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap = persons_;
    }

    // 序列化 → 加密 → 原子写。密钥缺失/损坏 → save 失败，绝不回退明文落盘。
    const std::string payload = serializePayload(snap);
    std::vector<std::uint8_t> plain(payload.begin(), payload.end());

    std::vector<std::uint8_t> key;
    std::string err;
    {
        std::lock_guard<std::mutex> lock(keyMu_);
        if (!ensureOrLoadKey(keyPathFor(path), key, err)) return false;
    }

    const std::filesystem::path tmp = std::filesystem::path(path.string() + ".save.tmp");
    if (!encryptToFile(tmp, plain, key, err)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    if (!replaceFile(tmp, path, err)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

MigrationOutcome FaceDatabase::migratePlaintextToEncrypted(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, PersonEntry>& persons,
    std::string& err) {
    err.clear();
    const std::filesystem::path bak = std::filesystem::path(path.string() + ".pre-encrypt");

    // A11：迁移前若**已存在**同名 .pre-encrypt 副本，必须明确报错停止 —— 它可能是人工恢复的
    //   最后凭据（或上一次迁移的产物），静默覆写/删除都不允许。此支不改动任何文件（主库仍为明文）。
    std::error_code existsEc;
    if (std::filesystem::exists(bak, existsEc)) {
        err = "已存在历史 .pre-encrypt 副本，为避免覆盖而中止迁移（未做任何改动）";
        return MigrationOutcome::HistoricalBackupPresent;
    }

    const std::string payload = serializePayload(persons);
    std::vector<std::uint8_t> plain(payload.begin(), payload.end());

    std::vector<std::uint8_t> key;
    {
        std::lock_guard<std::mutex> lock(keyMu_);
        // 密钥不可用 → 迁移中止：不加密、不动旧库（后续仍可明文读取）。
        if (!ensureOrLoadKey(keyPathFor(path), key, err)) return MigrationOutcome::Aborted;
    }

    // 先写临时加密文件；成功后再备份旧明文并原子替换。任一步失败旧库保持原样。
    const std::filesystem::path tmp = std::filesystem::path(path.string() + ".enc.tmp");
    if (!encryptToFile(tmp, plain, key, err)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return MigrationOutcome::Aborted;
    }

    // 备份旧明文（**本次迁移**的副本）。按冻结规格 §3.2：迁移任一步失败即中止——若备份失败仍继续
    // 替换，替换成功后明文与回滚点会双双消失（备份未生成 + 原路径已被密文覆盖），无回滚点。
    // 这里用 copy_options::none（而非 overwrite_existing）：上面已判过不存在，若此刻竞态出现同名文件
    // 则 copy 失败并中止，绝不静默覆盖（与「历史副本不覆写」同一安全方向）。
    std::error_code ec;
    std::filesystem::copy_file(path, bak, std::filesystem::copy_options::none, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        err = "明文备份失败: " + ec.message();
        return MigrationOutcome::Aborted;  // 备份失败：旧明文仍在原路径，未损坏。
    }

    if (!replaceFile(tmp, path, err)) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return MigrationOutcome::Aborted;  // 替换失败：旧明文仍在原路径，未损坏。
    }

    // A11：回读**实际密文主库**，用原解密解析链还原完整记录，与本次迁移源逐项逐值等价（非余弦相似）。
    std::string verifyErr;
    if (!encryptedPayloadMatches(path, persons, key, verifyErr)) {
        err = "迁移后回读校验失败（" + verifyErr + "）；已保留本次明文副本";
        return MigrationOutcome::VerifyFailed;  // 密文已替换主库，但校验不等价 ⇒ 副本保留（失败）
    }

    // 验证通过才删**本次**生成的备份；删除失败显式报告并保留（此时密文已可用，属清理告警而非失败）。
    std::error_code rmBakEc;
    if (!std::filesystem::remove(bak, rmBakEc)) {
        err = "迁移验证通过但删除本次明文副本失败: " + rmBakEc.message() + "；副本保留";
        return MigrationOutcome::CleanupFailed;
    }
    return MigrationOutcome::Success;
}

MigrationOutcome FaceDatabase::lastMigrationOutcome() const {
    std::lock_guard<std::mutex> lock(mu_);
    return migrationOutcome_;
}

std::string FaceDatabase::lastMigrationDetail() const {
    std::lock_guard<std::mutex> lock(mu_);
    return migrationDetail_;
}

void FaceDatabase::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    persons_.clear();
}

bool FaceDatabase::hasPerson(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return persons_.find(id) != persons_.end();
}

std::vector<std::string> FaceDatabase::listPersonIds() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> ids;
    ids.reserve(persons_.size());
    for (const auto& kv : persons_) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool FaceDatabase::updateMean(const std::string& id, const std::vector<float>& embedding) {
    if (id.empty() || embedding.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto& e = persons_[id];
    if (e.id.empty()) e.id = id;

    if (e.mean.empty()) {
        e.mean = embedding;
        e.count = 1;
        return true;
    }
    if (e.mean.size() != embedding.size()) return false;

    const double prev = static_cast<double>(e.count);
    const double next = prev + 1.0;
    for (std::size_t i = 0; i < e.mean.size(); i++) {
        const double v = (static_cast<double>(e.mean[i]) * prev + static_cast<double>(embedding[i])) / next;
        e.mean[i] = static_cast<float>(v);
    }
    e.count++;
    return true;
}

std::unordered_map<std::string, PersonEntry> FaceDatabase::persons() const {
    std::lock_guard<std::mutex> lock(mu_);
    return persons_;
}

}  // namespace rk_win
