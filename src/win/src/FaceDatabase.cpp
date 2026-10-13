#include "rk_win/FaceDatabase.h"

#include "rk_win/WinCrypto.h"

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

}  // namespace

std::filesystem::path FaceDatabase::keyPathFor(const std::filesystem::path& dbPath) {
    return std::filesystem::path(dbPath.string() + ".key.dpapi");
}

bool FaceDatabase::load(const std::filesystem::path& path) {
    if (path.empty()) return false;
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
                CV_Assert(meanMat.type() == CV_32F && meanMat.isContinuous() &&
                          e.mean.size() == static_cast<std::size_t>(meanMat.total()));
                std::copy(meanMat.ptr<float>(), meanMat.ptr<float>() + meanMat.total(), e.mean.begin());
                if (!e.id.empty() && !e.mean.empty()) {
                    tmp.emplace(e.id, std::move(e));
                }
            }
        }
        std::string migErr;
        (void)migratePlaintextToEncrypted(path, tmp, migErr);
    }

    std::lock_guard<std::mutex> lock(mu_);
    persons_ = std::move(tmp);
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

bool FaceDatabase::migratePlaintextToEncrypted(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, PersonEntry>& persons,
    std::string& err) {
    err.clear();
    const std::string payload = serializePayload(persons);
    std::vector<std::uint8_t> plain(payload.begin(), payload.end());

    std::vector<std::uint8_t> key;
    {
        std::lock_guard<std::mutex> lock(keyMu_);
        // 密钥不可用 → 迁移中止：不加密、不动旧库（后续仍可明文读取）。
        if (!ensureOrLoadKey(keyPathFor(path), key, err)) return false;
    }

    // 先写临时加密文件；成功后再备份旧明文并原子替换。任一步失败旧库保持原样。
    const std::filesystem::path tmp = std::filesystem::path(path.string() + ".enc.tmp");
    if (!encryptToFile(tmp, plain, key, err)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }

    // 备份旧明文（回滚点）。按冻结规格 §3.2：迁移任一步失败即中止——若备份失败仍继续
    // 替换，替换成功后明文与回滚点会双双消失（备份未生成 + 原路径已被密文覆盖），
    // 无回滚点（P2-1 S3 CONFIRMED）。故备份失败必须中止、保留旧明文原路径可重试。
    std::error_code ec;
    std::filesystem::copy_file(path, std::filesystem::path(path.string() + ".pre-encrypt"),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;  // 备份失败：旧明文仍在原路径，未损坏。
    }

    if (!replaceFile(tmp, path, err)) {
        std::filesystem::remove(tmp, ec);
        return false;  // 替换失败：旧明文仍在原路径，未损坏。
    }
    return true;
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
