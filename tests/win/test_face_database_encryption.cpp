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
#include <sstream>
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

struct LogCapture {
    std::ostringstream lines;
    std::streambuf* previous;

    LogCapture() : previous(std::cerr.rdbuf(lines.rdbuf())) {}
    ~LogCapture() { std::cerr.rdbuf(previous); }
};

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

// 旧明文库迁移（A11 新契约）：load 后数据完整、新库为加密容器（magic 头 + 无明文 id），且
//   **本次**生成的 .pre-encrypt 明文副本在回读逐项等价验证通过后被**删除**（MigrationOutcome::Success）。
// （RED：修复前无验证/删除，文件仍为明文 + 备份恒留存 ⇒ magic/无明文/副本消失/outcome 四条断言失败。）
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

    // A11：迁移成功并经**回读逐项等价**验证后，**本次**生成的明文副本必须被删除（不再留作回滚点）。
    const auto bak = dbPath.string() + ".pre-encrypt.pending";
    if (std::filesystem::exists(bak)) {
        std::cerr << "[migrate] 迁移成功后本次 .pre-encrypt 副本仍在（应验证通过后删除）" << std::endl;
        return false;
    }
    if (db.lastMigrationOutcome() != rk_win::MigrationOutcome::Success) {
        std::cerr << "[migrate] lastMigrationOutcome 非 Success" << std::endl;
        return false;
    }

    // 密文自足：删除副本后重新装载同一文件仍能读回同一人员（证明验证删副本未损数据）。
    rk_win::FaceDatabase re;
    if (!re.load(dbPath)) return false;
    if (!re.hasPerson("carol-003")) return false;
    const auto persons = re.persons();
    const auto& e = persons.at("carol-003");
    if (e.count != 2 || e.mean.size() != 4) return false;  // count/dim 逐项保留
    if (e.mean[0] != 0.1f || e.mean[1] != -0.2f || e.mean[2] != 0.3f || e.mean[3] != 0.4f) return false;
    return true;
}

// A11（历史同名副本）：迁移前若已存在 .pre-encrypt（可能是人工恢复的最后凭据），必须**明确报错停止**，
//   不覆写、不删历史副本，且不动主库（保持原明文）。
// 判别点：删掉「存在即中止」的判据（回到 copy_file(overwrite_existing)）⇒ 历史副本被覆盖、主库被
//   替换成密文 ⇒ 红。
//
// ⚠️ A11 的四态中 VerifyFailed / CleanupFailed **无确定性构造用例**（如实登记，与本仓既有「无判别锚」
//   惯例一致，如 StructuredLogger 的磁盘满写入失败）：
//   · VerifyFailed 要求「replaceFile 之后、回读校验之前」磁盘密文与迁移源不等价——本迁移在一步内
//     完成 write→copy→replace→verify，其间**无非接缝可注入**；且 serialize/deserialize 为无损 hex
//     往返 ⇒ 正常路径**恒**等价。
//   · CleanupFailed 要求 `std::filesystem::remove(bak)` 失败——bak 由本迁移**当次创建并随即删除**，
//     测试无法在其存续窗口内加锁/改权限。
//   两态仍由 MigrationOutcome 枚举**如实表达**并在实现中可达（防御性），只是不经公开 API 注入。
bool test_face_db_migration_historical_backup_aborts() {
    const auto dir = makeTempDir("migrate_hist_backup");
    const auto dbPath = dir / "db.yml";
    writeLegacyYaml(dbPath, "ivan-009", {0.1f, 0.2f}, 1);

    const auto bak = dbPath.string() + ".pre-encrypt";
    const std::string sentinel = "HISTORICAL-BACKUP-DO-NOT-TOUCH";
    {
        std::ofstream ofs(bak, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs << sentinel;
    }

    rk_win::FaceDatabase db;
    if (!db.load(dbPath)) return false;               // 明文仍可读
    if (!db.hasPerson("ivan-009")) return false;

    // 主库必须仍为明文（迁移被中止，未替换）。
    const std::string file = readFileBytes(dbPath);
    if (file.rfind("RKFDBCRY", 0) == 0) {
        std::cerr << "[migrate_hist_backup] 主库被错误迁移为密文" << std::endl;
        return false;
    }
    if (file.find("ivan-009") == std::string::npos) return false;
    // 历史副本必须原样未动（不覆写、不删）。
    if (readFileBytes(bak) != sentinel) {
        std::cerr << "[migrate_hist_backup] 历史 .pre-encrypt 副本被改动" << std::endl;
        return false;
    }
    if (db.lastMigrationOutcome() != rk_win::MigrationOutcome::HistoricalBackupPresent) {
        std::cerr << "[migrate_hist_backup] lastMigrationOutcome 非 HistoricalBackupPresent" << std::endl;
        return false;
    }
    return true;
}

bool test_face_db_migration_failure_emits_sanitized_warning() {
    const std::string identity = "private_identity_fixture";
    for (bool historical : {false, true}) {
        const auto dir = makeTempDir(historical ? "migration_warning_history" : "migration_warning_abort");
        const auto path = dir / "db.yml";
        writeLegacyYaml(path, identity, {0.1f, 0.2f}, 1);
        const auto original = readFileBytes(path);
        {
            std::ofstream blocker(historical ? path.string() + ".pre-encrypt"
                                             : rk_win::FaceDatabase::keyPathFor(path).string());
            blocker << (historical ? "HISTORICAL-RECOVERY-EVIDENCE" : "%%%INVALID%%%");
        }
        LogCapture capture;
        rk_win::FaceDatabase db;
        if (!db.load(path) || readFileBytes(path) != original) return false;
        const auto expected = historical ? rk_win::MigrationOutcome::HistoricalBackupPresent
                                         : rk_win::MigrationOutcome::Aborted;
        const auto code = historical ? "HistoricalBackupPresent" : "Aborted";
        if (db.lastMigrationOutcome() != expected) return false;
        const std::string warning = capture.lines.str();
        if (warning.find("face_database_migration") == std::string::npos ||
            warning.find(code) == std::string::npos || warning.find(identity) != std::string::npos) {
            std::cerr << "[migration_warning] 迁移失败必须记录结果码且不能输出人员原值" << std::endl;
            return false;
        }
    }
    return true;
}

bool test_face_db_migration_resumes_matching_pending_copy() {
    const auto dir = makeTempDir("migration_pending_resume");
    const auto path = dir / "db.yml";
    writeLegacyYaml(path, "pending_fixture", {0.1f, -0.2f}, 3);
    const auto pending = path.string() + ".pre-encrypt.pending";
    const auto scratch = path.string() + ".enc.tmp";
    // 重建备份完成、原子替换之前崩溃的文件现场；所有数据均为合成夹具。
    std::filesystem::copy_file(path, pending);
    { std::ofstream interrupted(scratch); interrupted << "INTERRUPTED-CIPHER-SCRATCH"; }

    rk_win::FaceDatabase db;
    if (!db.load(path) || db.lastMigrationOutcome() != rk_win::MigrationOutcome::Success) return false;
    if (readFileBytes(path).rfind("RKFDBCRY", 0) != 0 || std::filesystem::exists(pending) ||
        std::filesystem::exists(scratch)) return false;
    rk_win::FaceDatabase reloaded;
    if (!reloaded.load(path)) return false;
    const auto persons = reloaded.persons();
    const auto found = persons.find("pending_fixture");
    return found != persons.end() && found->second.count == 3 &&
           found->second.mean == std::vector<float>({0.1f, -0.2f});
}

bool test_face_db_migration_rejects_mismatched_pending_copy() {
    const auto dir = makeTempDir("migration_pending_mismatch");
    const auto path = dir / "db.yml";
    writeLegacyYaml(path, "pending_fixture", {0.1f, 0.2f}, 1);
    const auto original = readFileBytes(path);
    const auto pending = path.string() + ".pre-encrypt.pending";
    const auto scratch = path.string() + ".enc.tmp";
    { std::ofstream copy(pending); copy << "UNRELATED-RECOVERY-EVIDENCE"; }
    { std::ofstream copy(scratch); copy << "EXISTING-SCRATCH-EVIDENCE"; }
    rk_win::FaceDatabase db;
    if (!db.load(path) || db.lastMigrationOutcome() != rk_win::MigrationOutcome::Aborted) return false;
    return readFileBytes(path) == original && readFileBytes(pending) == "UNRELATED-RECOVERY-EVIDENCE" &&
           readFileBytes(scratch) == "EXISTING-SCRATCH-EVIDENCE";
}

bool test_face_db_migration_retained_pending_is_visible_on_encrypted_load() {
    const auto dir = makeTempDir("migration_pending_after_replace");
    const auto path = dir / "db.yml";
    writeLegacyYaml(path, "pending_fixture", {0.1f, 0.2f}, 1);
    const auto plain = readFileBytes(path);
    rk_win::FaceDatabase migrated;
    if (!migrated.load(path)) return false;
    const auto encrypted = readFileBytes(path);
    const auto pending = path.string() + ".pre-encrypt.pending";
    { std::ofstream copy(pending, std::ios::binary); copy << plain; }

    // 替换后未完成校验/清理的副本不能被静默删除，重启读取须发出可见清理告警。
    LogCapture capture;
    rk_win::FaceDatabase db;
    if (!db.load(path) || db.lastMigrationOutcome() != rk_win::MigrationOutcome::CleanupFailed) return false;
    return readFileBytes(path) == encrypted && readFileBytes(pending) == plain &&
           capture.lines.str().find("face_database_migration") != std::string::npos &&
           capture.lines.str().find("CleanupFailed") != std::string::npos &&
           capture.lines.str().find("pending_fixture") == std::string::npos;
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
    // A11：密钥不可用 ⇒ 迁移中止（Aborted），而非任何「成功/清理」态。
    if (db.lastMigrationOutcome() != rk_win::MigrationOutcome::Aborted) return false;
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
