#pragma once

#include "WinConfig.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rk_win {

// Windows JSON 配置存储（落盘 + 校验 + 热重载 + 回滚）
//
// 目标路径：%APPDATA%\<ProductName>\config.json
// - 为什么放到 APPDATA：可写、按用户隔离、不需要管理员权限；
// - 坑：多实例同时写会冲突；因此写入必须“先校验再原子替换”，并保留 .bak 用于回滚。
//
// ── 承重不变量（FACE-CONFIG-013）：任何**写入 config.json 内容**的路径都必须持 mu_ ──
// 射程（**字面**，勿扩读）：本不变量约束的是「把 config.json 的**内容**写下去」的路径。
//   ⚠️ **改名 / 删除类变更不在本不变量射程**：quarantineConfigFile（std::filesystem::rename 把
//      config.json 改名另存为 .corrupt.<ts>）与 initialize 的残留清扫（std::filesystem::remove）
//      都不产生新内容、也不做合并，故不适用「持 mu_」。**不要**据本不变量要求它们在 mu_ 下运行。
//      ⚠️ 但正因它们不受 mu_ 保护，存在一条**既有**（非本批引入、**尚未修**）的可达竞态：
//      reloadFromDisk 在 watch 线程上「读坏文件 → **释放 mu_** → 解析被拒 → 调 quarantineConfigFile
//      改名」；此窗口内 HTTP 线程 PUT 成功、写盘合法配置 B 并提交 cfg_=B，随后 watch 线程把**这份
//      刚写好的合法 B 改名成 .corrupt.<ts>**（伪造一条 corrupt 证据、outErr 谎称隔离了坏配置），
//      再持锁把 cfg_(=B) 写回——在「内存序列化不可写 ⇒ 回退 .bak」那条臂里，磁盘会退成更旧的 .bak
//      而内存是 B，正是 FACE-CONFIG-010 刚消除的「磁盘/内存分歧」。**本轮不修**：修锁结构需并发判别
//      用例与测试接缝（本类尚未提供），用户已决定「并发与不变量专项」计入后续任务。见 reloadFromDisk
//      的 quarantine 调用点同因注释。
// 为什么这是**结构性**保证、而非「在锁内写句注释就够」：真正落盘的
//   buildSettingsJson / writeAtomicallyWithBackup / writeFileAll / moveReplaceWriteThrough
// **都是不取锁的函数**（自由函数与无状态成员函数同理，见 WinJsonConfig.cpp 逐字核实）。谁调用它们，
// 谁负责持锁。故「必须持 mu_」这条约束**只能**由调用点（本类的写盘入口）表达：写盘调用点是否位于
// 同一个 mu_ 临界区内，是审阅时唯一的可见判据；把约束写进被调函数内部既拦不住新调用点、也不可见。
// 保护对象：杜绝「回滚线程取旧快照 A → 并发 PUT 写盘 B 并提交 cfg_=B → 回滚线程把旧 A 覆盖回磁盘」
// 这类「用旧快照覆盖已提交新配置」的竞态（即 FACE-CONFIG-010 的失败模式，只是从确定性版变竞态版）。
// 覆盖范围（**5 处调用点注释 / 6 个写盘调用**；reloadFromDisk 回滚段的内存写回与 .bak 写回共用一条）：
//   updateFromJsonBody（PUT）、rotateKeyAndReencrypt、reloadFromDisk 的回滚写回（2 个调用），
//   **以及 initialize 的两处写盘**（迁移落盘 / 默认值落盘）。initialize 虽在启动阶段、尚无并发消费者
//   （两个生产入口均在 initialize 之后才 startWatching / 进入轮询循环），仍统一持 mu_——这是**幂等加固**，
//   目的是让「写盘必须持锁」在每个调用点都一致可见、后人新增写盘路径时会照抄该形态。
//   initialize 的残留清扫虽**不在本射程**（见上），本轮也请其一并持 mu_（见 FACE-CONFIG-014 处注释），
//   使「对配置目录的变更统一在 mu_ 下」这条口径**可机械复核**。
// ⚠️ 新增写 config.json 内容的路径时：把写盘调用放进 mu_ 临界区，且**不得**在持锁期间调用任何再取 mu_ 的
//   成员（会自死锁）。本类所有写盘调用链全程不取 mu_，故「写盘 + 内存提交同处一个 mu_ 临界区」安全。
class WinJsonConfigStore {
public:
    struct UpdateResult {
        bool ok = false;
        int httpStatus = 500;  // 用于 API 返回（400/409/500 等）
        std::string code;      // 机器可读错误码
        std::string message;   // 人类可读说明
        std::vector<std::string> details;
    };

    WinJsonConfigStore();
    ~WinJsonConfigStore();

    // 初始化：从 config.json 读取；不存在则尝试从旧 ini 迁移并以默认值落盘。
    // 若 config.json 存在却无法装载（不可读 / 解析或 schema 校验被拒）：先将其改名另存
    // （config.json.corrupt.<毫秒时间戳>，保全证据），再以默认值启动；隔离失败则保留原文件、本次不落盘。
    // 装载失败的原因经 warnOut 与下列三个诊断访问器暴露给独立可见通道。
    bool initialize(std::string& warnOut);

    // 当前生效配置（线程安全复制）
    AppConfig current() const;

    // 配置变更代数：PUT（updateFromJsonBody）或外部写入（reloadFromDisk 成功）各 +1。
    // 消费者记录已消费代数，代数变化即"恰好消费一次"（两来源无重复/遗漏）；
    // PUT 落盘会自更新 lastWriteTime_，pollReloadOnce 无法据此感知，需 generation 判定。
    uint64_t generation() const;

    // 供 API 输出：返回脱敏后的 settings JSON（不包含密钥/敏感明文）
    std::string currentRedactedJsonPretty() const;

    // PUT settings：body 为 JSON 对象（局部更新）。成功时会落盘并更新内存。
    UpdateResult updateFromJsonBody(const std::string& bodyUtf8);

    // 敏感字段密钥轮换：生成新 AES 主密钥（DPAPI 保护）并重新加密敏感字段落盘。
    UpdateResult rotateKeyAndReencrypt();

    // 热重载：启动后台线程监控 config.json 变更；变更后自动 reload+校验；失败时回滚。
    // - outApplied=true 代表确实更新了内存配置（调用方可据此决定是否重启模块）
    bool startWatching();
    void stopWatching();
    bool pollReloadOnce(bool& outApplied, std::string& outErr);  // 便于测试/无线程环境

    std::filesystem::path configPath() const { return configPath_; }

    // ── FACE-CONFIG-001 启动装载诊断（供独立可见通道读取）──
    // initWarning：initialize 的告警文本副本；空串 = 本次启动无告警。
    std::string initWarning() const;
    // configRejectedOnLoad：既有 config.json 存在但**无法装载**（不可读 / 解析或 schema 校验被拒），
    // 因而本次以默认值运行（降级启动）。
    bool configRejectedOnLoad() const;
    // initQuarantineFileName：被改名另存的坏配置**文件名**（非全路径）。空串 = 本次未发生隔离，
    //   涵盖三种情形：装载成功、config.json 本就不存在、以及**隔离失败**。
    //   configRejectedOnLoad()==true 且本项为空 ⇒ 即「隔离失败」，此时原文件**未被覆盖**——
    //   但该保全**仅限本次 initialize**（后续 PUT / 热重载会按各自既有语义重写 config.json）。
    std::string initQuarantineFileName() const;

private:
    bool reloadFromDisk(bool& outApplied, std::string& outErr);
    bool writeAtomicallyWithBackup(const std::string& jsonPretty, std::string& outErr);

    // 加解密相关：仅用于“敏感字段”落盘形态（cipher object），内存中仍为明文（便于业务使用）。
    bool ensureOrLoadKey(std::vector<std::uint8_t>& keyOut, std::string& outErr) const;

    // JSON <-> AppConfig
    bool parseAndValidateSettingsDoc(const std::string& jsonText, AppConfig& cfgOut, std::string& outErr) const;
    std::string buildSettingsJson(const AppConfig& cfg, bool redacted, bool encryptSensitive) const;

    static std::filesystem::path defaultConfigPath();
    static std::filesystem::path defaultKeyPath(const std::filesystem::path& cfgPath);
    static std::filesystem::path defaultBakPath(const std::filesystem::path& cfgPath);

    // watch state
    std::atomic<bool> watching_{false};
    std::thread watchThread_;

    std::filesystem::path configPath_;
    std::filesystem::path keyPath_;
    std::filesystem::path bakPath_;
    std::filesystem::file_time_type lastWriteTime_{};

    mutable std::mutex mu_;
    AppConfig cfg_{};
    std::string lastGoodJsonPretty_;  // 用于回滚/调试输出（不含密钥）
    std::atomic<std::uint64_t> generation_{0};

    // FACE-CONFIG-001 启动装载诊断，由 initialize 在 mu_ 保护下写入，由三个 const 访问器读取。
    bool configRejectedOnLoad_ = false;   // 既有配置无法装载 ⇒ 本次降级启动
    std::string initWarning_;             // initialize 的告警文本副本；空串 = 无告警
    std::string initQuarantineFileName_;  // 坏配置被另存的文件名；空串 = 未隔离（非降级启动）或隔离失败
};

}  // namespace rk_win

