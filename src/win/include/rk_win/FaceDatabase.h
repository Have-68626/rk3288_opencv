#pragma once

#include "Compat.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <cstdint>

namespace rk_win {

struct PersonEntry {
    std::string id;
    std::vector<float> mean;
    std::int64_t count = 0;
};

// A11：明文 YAML → 加密容器迁移的**可观察结果**。
// 为什么需要它：load() 的返回 bool 只表达「库是否可读」，**不表达**迁移是否成功清理了本次明文副本
//   ⇒ 「密文已成功落盘但清理失败」与「完全成功」在原有的 bool 上不可区分，调用方（装配链）无从知晓。
//   本枚举显式区分迁移与清理状态，供测试与诊断观察（不改动 load() 的既有 bool 契约）。
enum class MigrationOutcome {
    NotAttempted,             // 未发生迁移（库不存在 / 已是加密容器 / 常量路径）
    Success,                  // 加密落盘 + 回读逐项等价验证通过 + 本次明文副本已删除
    VerifyFailed,             // 加密已替换主库，但回读校验不等价 ⇒ 本次副本保留（失败）
    CleanupFailed,            // 密文可读，但本次删除失败或重启发现遗留 pending 副本（清理告警）
    HistoricalBackupPresent,  // 已存在历史同名 .pre-encrypt 副本 ⇒ 迁移中止、未做任何改动（不覆写不删）
    Aborted,                  // 其它中止（密钥不可用 / 加密失败 / 备份失败 / 替换失败），旧明文原样保留
};

// 人脸识别数据库（特征向量 + 身份标签）落盘存储。
//
// 存储层加密（S-SEC-B09-002）：磁盘文件为 AES-256-GCM 加密容器（magic "RKFDBCRY" + 版本 +
// nonce/tag + 密文），明文载荷仅存在于内存。密钥为随机 32 字节 AES 主密钥，DPAPI 保护后
// base64 落盘到 <数据库路径>.key.dpapi（与 WinJsonConfig 的 config.key.dpapi 布局一致）。
// 复用 WinCrypto 公开原语（base64/dpapi/AES-256-GCM/randomBytes），密钥文件管理为本地最小辅助。
//
// 迁移：load() 检测到旧明文 YAML 时自动加密迁移（先写临时文件、备份 <path>.pre-encrypt.pending、
// 原子替换）；迁移任一步失败即中止，旧明文保持原样可回滚。错误密钥/损坏密文 → GCM tag
// 校验失败 → load 受控返回 false（不崩溃、不写明文）。密钥文件丢失/损坏 = 数据库不可恢复。
class FaceDatabase {
public:
    // 从 path 装载库。
    //
    // ⚠️ 语义（FACE-DB-001，**有意为之，勿当作缺陷**）：`path` **不存在**时返回 `true` 并置为**空库**
    //    —— 「首次运行 / 库尚未创建」必须算成功，装配链依赖它（`RuntimeBootstrap` 用不存在的
    //       databasePath 表达「空库、初始化成功」，见 tests/win/test_runtime_bootstrap.cpp 的同名注释）；
    //       且 `save()` 会 `create_directories(path.parent_path())`，故父目录缺失也不需在 load 期拦。
    //    真正的失败面是「路径**存在**但读不开 / 解密失败（错密钥、密文被篡改）/ 反序列化失败」
    //    ⇒ 一律返回 `false` 并清空内存库。⇒ 因此**不能**把「返回 true」当成「库文件存在且已装载」的证据。
    //    ⚠️ **例外已修（2026-09-26，原登记为待办）**：**旧明文 YAML** 分支的解析是**抛出式**的 ——
    //    实测（本机 OpenCV 4.10.0 原样复现，两位验收代理各用独立探针复核）：`mean` 为**标量**时
    //    `(*it)["mean"] >> meanMat` 内部先取 `node["dt"]` ⇒ 对非 map 节点触发
    //    `cv::FileNode::operator[]` 的 `(-215:Assertion failed) isMap()`（`persistence.cpp:2171`）；
    //    而 `mean` 为 **CV_64F 矩阵**（结构合法、类型不符）时不抛，落到**显式校验**。
    //    ★ 注意（勿写成「序列元素不是 map」—— 该说法经实测**不成立**：元素 `isMap=1`）：
    //    交错的抛出面是「**对非 map 的 `mean` 节点做 Mat 读取**」，两条畸形臂**都栽在 `mean` 字段上**。
    //    修法：整段加 `try/catch(cv::Exception)`（异常 ⇒ 等价「库不可读」）+ 把 `CV_Assert` 改成
    //    **显式校验返回 false**。⇒ `load()` 现在对**文件内容导致的** cv 失败一律**受控返回 false**。
    //    ⚠️ **不得**写成「对一切可预期的坏库都是受控返回」：该 catch **只兜 `cv::Exception`** ——
    //    `std::bad_alloc`/`std::length_error`（`e.mean.resize`、`tmp.emplace`）与 `std::filesystem`
    //    异常**仍会抛穿**（修前「全都不兜」，故非回归）；且它也覆盖 `OutOfMemoryError` ⇒ 真 OOM
    //    会被报成「库不可读」，而本函数无日志依赖 ⇒ 坏库与错密钥诊断上不可区分（见 `.cpp` 处登记）。
    //    （注：两半在可观测行为上等价——见
    //    `tests/win/test_face_database_encryption.cpp` 那条用例登记的判别力边界。）
    //    判别对照与钉住用例：tests/win/test_face_database_encryption.cpp 的
    //    `face_db_load_nonexistent_path_is_intentionally_ok`。
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    void clear();
    bool hasPerson(const std::string& id) const;
    std::vector<std::string> listPersonIds() const;

    bool updateMean(const std::string& id, const std::vector<float>& embedding);
    std::unordered_map<std::string, PersonEntry> persons() const;

    // DPAPI 保护的 AES 主密钥文件路径（<数据库路径>.key.dpapi），供工具/测试计算。
    static std::filesystem::path keyPathFor(const std::filesystem::path& dbPath);

    // A11：最近一次 load() 触发的明文→加密迁移结果（可观察通道，见 MigrationOutcome 说明）。
    //   线程安全：与 persons() 同受 mu_ 保护。
    MigrationOutcome lastMigrationOutcome() const;
    // 迁移结果的补充说明（失败原因 / 保留的副本路径线索）；成功或未迁移时为空串。**不含**人员身份明文。
    std::string lastMigrationDetail() const;

private:
    // 旧明文 YAML → 加密容器迁移（内部持 keyMu_）；失败/中止保留旧库，可回滚。
    // A11：返回迁移结果（而非 bool），以便 load() 把「密文可用但清理失败」与「完全成功」区分上报。
    MigrationOutcome migratePlaintextToEncrypted(const std::filesystem::path& path,
                                                 const std::unordered_map<std::string, PersonEntry>& persons,
                                                 std::string& err);

    mutable std::mutex mu_;
    mutable std::mutex keyMu_;  // 保护密钥文件"读取或创建"，避免并发生成/覆盖竞态
    std::unordered_map<std::string, PersonEntry> persons_;
    // A11：最近一次迁移结果（受 mu_ 保护）。
    MigrationOutcome migrationOutcome_ = MigrationOutcome::NotAttempted;
    std::string migrationDetail_;
};

}  // namespace rk_win
