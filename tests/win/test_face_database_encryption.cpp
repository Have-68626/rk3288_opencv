// S-SEC-B09-002 FaceDatabase 加密负面用例（RED 先行）。
//
// 修复前（明文 YAML 库）：
//   1) test_face_db_plaintext_vuln_documented — save 输出无加密 magic 且含明文身份标签（暴露证据，先通过）；
//   2) test_face_db_encrypted_no_plaintext     — 新库无明文特征（RED，修复前失败）；
//   3) test_face_db_wrong_key_controlled_failure — 错误密钥受控失败（RED，修复前 load 无视密钥返回 true）；
//   4) test_face_db_migration_encrypts_legacy  — 旧明文库自动迁移 + 数据完整 + 备份可回滚（RED，修复前失败）；
//   5) test_face_db_migration_failure_preserves_legacy — 迁移失败中止，旧库不被破坏（回滚安全网）；
//   6) test_face_db_encrypted_roundtrip        — 加密往返数据无损（回归）。
//
// 密钥文件路径约定：<数据库路径>.key.dpapi（DPAPI 保护 base64(32B AES 主密钥)，
// 与 WinJsonConfig 的 config.key.dpapi 布局一致），统一经 FaceDatabase::keyPathFor() 计算。

#include "rk_win/FaceDatabase.h"
#include "rk_win/WinCrypto.h"

#include <opencv2/core/persistence.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::filesystem::path makeTempDir(const std::string& suffix) {
    const auto base = std::filesystem::temp_directory_path() / "rk3288_opencv_face_db_enc_tests";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const auto dir = base / suffix;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string readFileBytes(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

// 复刻旧版 FaceDatabase::save 的明文 YAML 输出（cv::FileStorage），用于迁移测试。
void writeLegacyYaml(const std::filesystem::path& path, const std::string& id,
                     const std::vector<float>& mean, int count) {
    cv::FileStorage fs(path.string(), cv::FileStorage::WRITE);
    fs << "version" << 1;
    fs << "persons" << "[";
    fs << "{";
    fs << "id" << id;
    fs << "count" << count;
    cv::Mat m(1, static_cast<int>(mean.size()), CV_32F);
    std::copy(mean.begin(), mean.end(), m.ptr<float>());
    fs << "mean" << m;
    fs << "}";
    fs << "]";
    fs.release();
}

// 写一个 DPAPI 保护的 base64 密钥文件（32 字节 AES 主密钥），用于"换密钥"测试。
bool writeKeyFile(const std::filesystem::path& keyPath, const std::vector<std::uint8_t>& key) {
    std::vector<std::uint8_t> protectedBin, plainOut;
    std::string err;
    if (!rk_win::dpapiProtect(key, protectedBin, err)) return false;
    std::string b64;
    if (!rk_win::base64Encode(protectedBin, b64)) return false;
    std::ofstream ofs(keyPath, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs << b64;
    return ofs.good();
}

}  // namespace

// 明文暴露证据（RED 阶段通过 = 证明 save 曾落盘明文；GREEN 后本测试翻转为"无明文"断言）。
bool test_face_db_plaintext_vuln_documented() {
    const auto dir = makeTempDir("plain_vuln");
    const auto dbPath = dir / "db.yml";

    rk_win::FaceDatabase db;
    db.updateMean("eve-005", {0.1f, 0.2f, 0.3f});
    if (!db.save(dbPath)) return false;

    const std::string file = readFileBytes(dbPath);
    // GREEN 后：文件必须为加密容器，明文身份标签不可 grep（修复前的明文暴露已消除）。
    const bool hasMagic = file.rfind("RKFDBCRY", 0) == 0;
    const bool noPlainId = file.find("eve-005") == std::string::npos;
    const bool noYamlHeader = file.find("%YAML") == std::string::npos;
    return hasMagic && noPlainId && noYamlHeader;
}

// 新库必须无明文特征：magic 头存在、明文身份标签/特征串不可 grep（RED：修复前 save 输出明文 YAML）。
bool test_face_db_encrypted_no_plaintext() {
    const auto dir = makeTempDir("enc_no_plain");
    const auto dbPath = dir / "db.yml";

    rk_win::FaceDatabase db;
    db.updateMean("frank-006", {0.7f, 0.8f, 0.9f, 1.0f});
    if (!db.save(dbPath)) return false;

    const std::string file = readFileBytes(dbPath);
    const bool hasMagic = file.rfind("RKFDBCRY", 0) == 0;
    const bool noPlainId = file.find("frank-006") == std::string::npos;
    const bool noYamlHeader = file.find("%YAML") == std::string::npos;
    return hasMagic && noPlainId && noYamlHeader;
}

// 错误密钥解密必须受控失败：不崩溃、load=false、数据不可读（RED：修复前 load 无视密钥返回 true）。
bool test_face_db_wrong_key_controlled_failure() {
    const auto dir = makeTempDir("wrong_key");
    const auto dbPath = dir / "db.yml";

    rk_win::FaceDatabase db;
    db.updateMean("bob-002", {0.5f, 0.6f, 0.7f});
    if (!db.save(dbPath)) return false;

    // 用另一把随机密钥覆盖密钥文件（DPAPI 保护 base64）→ 之后解密必然失败。
    const auto keyPath = rk_win::FaceDatabase::keyPathFor(dbPath);
    std::vector<std::uint8_t> wrongKey(32, 0xAB);
    if (!writeKeyFile(keyPath, wrongKey)) return false;

    rk_win::FaceDatabase db2;
    const bool ok = db2.load(dbPath);
    if (ok) return false;                          // 修复前：load 无视密钥、返回 true（RED 失败点）
    if (!db2.persons().empty()) return false;      // 错误密钥下数据必须不可读
    return true;
}

// 旧明文库迁移：load 后数据完整、新库为加密容器（magic 头 + 无明文 id）、.pre-encrypt 备份可回滚。
// （RED：修复前无迁移，文件仍为明文 → magic/无明文/备份断言失败。）
bool test_face_db_migration_encrypts_legacy() {
    const auto dir = makeTempDir("migrate");
    const auto dbPath = dir / "db.yml";
    writeLegacyYaml(dbPath, "carol-003", {0.1f, -0.2f, 0.3f, 0.4f}, 2);

    rk_win::FaceDatabase db;
    if (!db.load(dbPath)) return false;                 // 明文库可读
    if (!db.hasPerson("carol-003")) return false;       // 数据完整

    const std::string file = readFileBytes(dbPath);
    if (file.rfind("RKFDBCRY", 0) != 0) return false;               // 迁移后必须为加密容器
    if (file.find("carol-003") != std::string::npos) return false;  // 无明文身份标签
    if (file.find("%YAML") != std::string::npos) return false;      // 无明文 YAML 头

    const auto bak = dbPath.string() + ".pre-encrypt";
    if (!std::filesystem::exists(bak)) return false;               // 备份存在 = 可回滚
    if (readFileBytes(bak).find("carol-003") == std::string::npos) return false;  // 备份为明文
    return true;
}

// 迁移失败必须中止且不损坏旧库：密钥文件非法 → 迁移不做，load 仍返回数据，旧明文保持原样。
bool test_face_db_migration_failure_preserves_legacy() {
    const auto dir = makeTempDir("migrate_fail");
    const auto dbPath = dir / "db.yml";
    writeLegacyYaml(dbPath, "dave-004", {0.2f, 0.3f, 0.4f, 0.5f}, 1);

    // 非法密钥文件 → ensureOrLoadKey 失败 → 迁移中止
    const auto keyPath = rk_win::FaceDatabase::keyPathFor(dbPath);
    {
        std::ofstream ofs(keyPath, std::ios::binary | std::ios::trunc);
        ofs << "%%%INVALID%%%";
    }

    rk_win::FaceDatabase db;
    if (!db.load(dbPath)) return false;
    if (!db.hasPerson("dave-004")) return false;

    const std::string file = readFileBytes(dbPath);
    if (file.find("dave-004") == std::string::npos) return false;   // 旧明文未被破坏
    if (file.rfind("RKFDBCRY", 0) == 0) return false;               // 未错误迁移成加密
    return true;
}

// 加密往返数据无损：save→load 后 id/count/mean 与源完全一致（float bit 模式精确相等）。
bool test_face_db_encrypted_roundtrip() {
    const auto dir = makeTempDir("roundtrip");
    const auto dbPath = dir / "db.yml";

    rk_win::FaceDatabase db;
    const std::vector<float> v = {0.5f, -0.25f, 3.14159f, 1.0f};
    db.updateMean("grace-007", v);
    if (!db.save(dbPath)) return false;

    rk_win::FaceDatabase db2;
    if (!db2.load(dbPath)) return false;
    if (!db2.hasPerson("grace-007")) return false;
    // 注意：persons() 按值返回副本，须先存具名变量再取引用，避免临时对象悬垂引用。
    const auto persons = db2.persons();
    const auto& e = persons.at("grace-007");
    if (e.count != 1 || e.mean.size() != v.size()) return false;
    for (std::size_t i = 0; i < v.size(); i++) {
        std::uint32_t a = 0, b = 0;
        std::memcpy(&a, &v[i], sizeof(a));
        std::memcpy(&b, &e.mean[i], sizeof(b));
        if (a != b) return false;   // 无损往返必须 bit 精确
    }
    return true;
}

// FACE-DB-001：把 `load()` 对**不存在**路径的语义**钉住**（它是**有意的、被依赖的**，不是缺陷）。
//
// 为什么要有这条：该语义此前只活在一句注释里（tests/win/test_runtime_bootstrap.cpp 的
// 「不存在的路径走 FaceDatabase::load 的 `!exists ⇒ 返回 true`（空库）⇒ 初始化成功」），
// 代码侧零显式用例。⇒ 任何人都可以「顺手把它修成 false」而**不会变红**，同时又有一条
// 生产装配链依赖它。本用例把「true」与「空库」同时钉死，并配**判别对照**证明该 true 来自
// 「路径不存在」这一分支、不是「load 恒真」。
bool test_face_db_load_nonexistent_path_is_intentionally_ok() {
    const auto dir = makeTempDir("nonexistent_path");

    // ① 首次运行形态：目标路径连父目录都不存在 ⇒ 必须 true + 空库。
    const auto missing = dir / "not_created_yet" / "db.yml";
    if (std::filesystem::exists(missing)) return false;   // 前置：确认它真的不存在
    rk_win::FaceDatabase db;
    if (!db.load(missing)) return false;                  // 不得因「库文件尚未创建」而失败
    if (!db.listPersonIds().empty()) return false;        // 且必须是**空**库，不是残留状态

    // ② 判别对照（本用例的判别力所在）：**存在**但打不开的路径必须返回 false
    //    ⇒ 证明 ① 的 true 来自 `!exists` 分支，而非「load 恒返回 true」。
    //    （同一条对照也是 test_runtime_bootstrap 里「传 "." 会失败」那句注释的机器化版本。）
    rk_win::FaceDatabase db2;
    if (db2.load(dir)) return false;
    if (!db2.listPersonIds().empty()) return false;       // 失败后不得留下半截状态

    // ③ 该语义的**用途**仍成立：空库 load 成功后，save 应能自建父目录并真正落盘
    //    （这正是「父目录缺失不需在 load 期拦」的依据）。
    const auto created = dir / "later_created" / "db.yml";
    rk_win::FaceDatabase db3;
    if (!db3.load(created)) return false;
    db3.updateMean("henry-008", {0.1f, 0.2f});
    if (!db3.save(created)) return false;
    if (!std::filesystem::exists(created)) return false;
    return true;
}
