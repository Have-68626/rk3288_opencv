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
#include <iostream>
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

// FACE-DB-002（2026-09-26 二次审计登记、本轮修）：畸形「旧明文库」必须**受控失败**，不得抛穿。
//
// 修前：明文分支的所有 OpenCV 解析**都是抛出式的**，而 `load()` 内既无 try/catch、其契约又是
// 「返回 false 并清空内存库」⇒ 装配链（`ArcFaceWinRecognizer::initialize` → `RuntimeBootstrap::build`）
// 会在**初始化期直接抛穿**。
//
// 本用例覆盖**两条防线**（缺一不可；★ 机制描述已按验收 A1/S3 的独立探针订正）：
//   A 手写 YAML：条目的 `mean` 写成**实数标量** ⇒ `(*it)["mean"] >> meanMat` 进入
//     `cv::read(const FileNode&, Mat&, …)`，其**第一条语句**先取 `node["dt"]` ⇒ 对**非 map** 的
//     `mean` 节点触发 `cv::FileNode::operator[]` 的 `isMap()` 断言
//     （实测异常文本：`persistence.cpp:2171: error: (-215:Assertion failed) isMap()`）
//     ⇒ 由**新加的 try/catch** 兜住；
//   B 用 `cv::FileStorage` 写出的**结构合法**库，但 `mean` 写成 **CV_64F 矩阵**（旧格式是 CV_32F）
//     ⇒ 不抛，落到新的**显式校验**（修前是 `CV_Assert` ⇒ 抛 ⇒ 也被 catch 兜住）。
//   ★ 勿把 A 说成「`persons` 的元素不是 map」——**实测元素 `isMap=1`（是 map）**，`["id"]`/`["count"]`
//     都不抛；**两条畸形臂都栽在 `mean` 这同一个字段上**，只是经由不同的代码路径。
// 两者都必须：**不抛** + 返回 **false** + 内存库为空（与「读不开 / 解密失败 / 反序列化失败」同一处置）。
//
// ★ 本用例的**判别力边界（实测，如实登记）**：
//   - 撤销整条修复（显式校验换回 `CV_Assert` **且**让 catch 重抛）⇒ 本用例**红**（异常文本为
//     `persistence.cpp:2171: (-215:Assertion failed) isMap()`）⇒ 判别力成立；
//   - 但**只**把显式校验换回 `CV_Assert`、**保留** try/catch ⇒ 本用例**仍绿**
//     ⇒ 两半在**可观测行为上等价**（异常同样被 catch 成 false）。故：
//       · 判别力落在 **try/catch** 那一半；
//       · **显式校验那一半无法被单独判别**（它的价值是「不用异常表达常规失败」+ 在将来可能的
//         无异常编译配置下仍成立），上表 B 臂对它只有**覆盖率**意义、没有判别力。
bool test_face_db_malformed_legacy_is_controlled_failure() {
    const auto dir = makeTempDir("malformed_legacy");

    struct Case {
        const char* name;
        std::filesystem::path path;
    };
    std::vector<Case> cases;

    // A：手写畸形 YAML（首 8 字节不是 `RKFDBCRY` ⇒ 走明文分支）。
    {
        const auto p = dir / "handwritten_bad.yml";
        std::ofstream ofs(p, std::ios::binary);
        if (!ofs) return false;
        ofs << "%YAML:1.0\n---\npersons:\n   - id: \"bad-A\"\n     count: 1\n     mean: 3.1400001\n";
        cases.push_back({"手写畸形（mean 是实数标量 ⇒ Mat 读取器对非 map 节点抛 isMap）", p});
    }
    // B：结构合法但 `mean` 的**类型**不符（CV_64F 而非 CV_32F）。
    {
        const auto p = dir / "double_mean.yml";
        cv::FileStorage fs(p.string(), cv::FileStorage::WRITE);
        fs << "version" << 1;
        fs << "persons" << "[";
        fs << "{";
        fs << "id" << std::string("bad-B");
        fs << "count" << 1;
        cv::Mat m(1, 2, CV_64F);
        m.at<double>(0, 0) = 0.1;
        m.at<double>(0, 1) = 0.2;
        fs << "mean" << m;
        fs << "}";
        fs << "]";
        fs.release();
        cases.push_back({"结构合法但 mean 是 CV_64F", p});
    }

    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) return false;

        rk_win::FaceDatabase db;
        bool threw = false;
        bool ok = true;
        try {
            ok = db.load(c.path);
        } catch (const std::exception& ex) {
            threw = true;
            std::cerr << "[face_db_malformed] " << c.name
                      << "：load() 抛出了异常（契约要求受控返回 false）：" << ex.what() << std::endl;
        }
        if (threw) return false;                        // 判别点 ①：不得抛
        if (ok) return false;                           // 判别点 ②：不得静默成功
        if (!db.listPersonIds().empty()) return false;  // 失败后不得留下半截状态
    }
    return true;
}
