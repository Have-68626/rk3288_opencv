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

// 人脸识别数据库（特征向量 + 身份标签）落盘存储。
//
// 存储层加密（S-SEC-B09-002）：磁盘文件为 AES-256-GCM 加密容器（magic "RKFDBCRY" + 版本 +
// nonce/tag + 密文），明文载荷仅存在于内存。密钥为随机 32 字节 AES 主密钥，DPAPI 保护后
// base64 落盘到 <数据库路径>.key.dpapi（与 WinJsonConfig 的 config.key.dpapi 布局一致）。
// 复用 WinCrypto 公开原语（base64/dpapi/AES-256-GCM/randomBytes），密钥文件管理为本地最小辅助。
//
// 迁移：load() 检测到旧明文 YAML 时自动加密迁移（先写临时文件、备份 <path>.pre-encrypt、
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
    //    ⚠️ **例外（2026-09-26 二次审计读码发现）**：**旧明文 YAML** 分支的条目校验用的是
    //    `cv::CV_Assert`（维度/类型不符即**抛异常**），而 `load()` 内**没有 try/catch**
    //    ⇒ 一份「能打开但内容畸形」的旧明文库会让 `load()` **抛**而不是返回 false，
    //    即该分支的失败**不是**受控返回。已登记为待办（修法二选一：改成显式校验返回 false，或在
    //    `load()` 外层兜住 `cv::Exception`）。
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

private:
    // 旧明文 YAML → 加密容器迁移（内部持 keyMu_）；失败/中止保留旧库，可回滚。
    bool migratePlaintextToEncrypted(const std::filesystem::path& path,
                                     const std::unordered_map<std::string, PersonEntry>& persons,
                                     std::string& err);

    mutable std::mutex mu_;
    mutable std::mutex keyMu_;  // 保护密钥文件"读取或创建"，避免并发生成/覆盖竞态
    std::unordered_map<std::string, PersonEntry> persons_;
};

}  // namespace rk_win
