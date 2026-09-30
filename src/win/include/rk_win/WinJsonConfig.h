#pragma once

#include "WinConfig.h"

#include <atomic>
#include <filesystem>
#include <functional>
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
//      ⚠️ 但正因它们不受 mu_ 保护，**曾**存在一条可达竞态（**已于 2026-09-28 修复**；此处保留原委，
//      是为了说清「为什么这次加锁**不意味着**不变量射程扩大」）：reloadFromDisk 在 watch 线程上
//      「读坏文件 → 释放 mu_ → 解析被拒 → 调 quarantineConfigFile 改名」；该窗口内 HTTP 线程 PUT
//      成功、写盘合法配置 B 并提交 cfg_=B，随后 watch 线程把**这份刚写好的合法 B** 改名成
//      .corrupt.<ts>（凭空伪造一条 corrupt 证据，且 outErr 谎称隔离了坏配置），再持锁把 cfg_(=B) 写回。
//   ✅ **2026-09-28 修法**：把「复核 mtime 未变」与 `quarantineConfigFile` 放进**同一个 mu_ 临界区**
//      （与同函数的成功臂同款写法）—— PUT 的写盘 + 提交持同一把 mu_ ⇒ 复核与 rename 之间无插入点。
//      **但本条不变量本身不变**：改名 / 删除仍**不**在 FACE-CONFIG-013 射程内（它们不产生新内容）
//      —— 那次加锁是为**身份复核**，理由与这条不变量不同。行为变化见 `reloadFromDisk` 的
//      quarantine 调用点注释（并发修改时不再隔离；持续外部写者会反复推迟隔离）。
// 为什么这是**结构性**保证、而非「在锁内写句注释就够」：真正落盘的
//   buildSettingsJson / writeAtomicallyWithBackup / writeFileAll / moveReplaceWriteThrough
// **都是不取锁的函数**（自由函数与无状态成员函数同理，见 WinJsonConfig.cpp 逐字核实）。谁调用它们，
// 谁负责持锁。故「必须持 mu_」这条约束**只能**由调用点（本类的写盘入口）表达：写盘调用点是否位于
// 同一个 mu_ 临界区内，是审阅时唯一的可见判据；把约束写进被调函数内部既拦不住新调用点、也不可见。
// 保护对象：杜绝「回滚线程取旧快照 A → 并发 PUT 写盘 B 并提交 cfg_=B → 回滚线程把旧 A 覆盖回磁盘」
// 这类「用旧快照覆盖已提交新配置」的竞态（即 FACE-CONFIG-010 的失败模式，只是从确定性版变竞态版）。
//   ✅ 2026-09-29 补（项 3）：上面这条「杜绝…」是**全称**声明，而 rotateKeyAndReencrypt 此前**不满足**
//     —— 它的「取快照 + DPAPI 重加密 + 序列化」**在锁外**，只在写盘 + 提交处持 mu_ ⇒ 并发 PUT 成功
//     提交的新配置会被 rotate 的旧快照静默覆盖（磁盘与内存一起退回旧值）⇒ 修复前该全称声明**对 rotate
//     为假**。**修法与理由**：rotate 内改用**代次校验 + 有界重试**（锁内记 generation_、写盘前在同一
//     临界区复核未变；变了则**不触碰任何磁盘状态**、直接重取快照，耗尽次数返回 409 conflict）。
//     generation_ 是可靠信号：PUT 成功与热重载成功都会 fetch_add(1)，rotate 自身不变代数。
//
//   ✅ 2026-09-29 补（项 A，**密钥/密文同源不变量**）：更早的 rotate 版本在**冲突分支**里调
//     `restoreOldKey()` 把**旧密钥**写回磁盘——但「新密钥文件」当时是在 mu_ **之外**写的，而 PUT 的
//     加密（buildSettingsJson → ensureOrLoadKey **每次重读密钥文件**）同样在 mu_ 外 ⇒ 存在交错：
//     某 PUT 在 rotate 写入新密钥**之后**读取密钥并加密 config.json，随后 rotate 的代次复核发现变化
//     ⇒ 走冲突分支把旧密钥写回 ⇒ **磁盘密钥=旧、磁盘密文=新** ⇒ 下次启动解密失败 ⇒ 配置被隔离、
//     用户设置静默丢失。**修法（本批）**：把「密钥文件」与「config.json 内容」**只在同一个 mu_ 临界区内、
//     以同一密钥成对写入**——即 rotate 的「写新密钥 → 用它加密 → 写 config.json → 提交」整段持锁，
//     且**冲突分支不写盘**（新密钥仅存在于内存，直接丢弃，无 restore）；同时把 PUT 的加密也移入其写盘
//     临界区。⇒ 盘上 config.json 恒能被盘上密钥文件解开（并发交错不再产生错配）。
//     ⚠️ **残余（如实登记）**：密钥文件与 config.json 是**两个文件**，无法原子同写 ⇒ 「写密钥之后、
//     写 config.json 之前」若**进程崩溃**，磁盘仍可能错配（下次启动隔离）。该崩溃窗口本批**不消除**，
//     只消除**并发交错**造成的错配。
//     ⚠️ **两个并发 rotate**：二者都不自增 generation_（设计如此，避免密钥轮换触发管线重启），且提交
//     由 mu_ 串行化 ⇒ 各自写入自洽的（密钥，密文）对，不变量保持；又因 rotate 不改配置内容，不存在
//     数据丢失（最终密钥为较晚提交者）。
//     ⇒ 至此三条写盘路径（PUT / 回滚 / rotate）**都**不会用旧快照覆盖已提交的新配置，且**都**满足
//       「密钥/密文同源」不变量，上述声明成立。
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
    // 承重不变量：盘上的 config.json 必须**始终**能用盘上的密钥文件解开（二者同源）——密钥文件与
    // config.json 内容只在同一个 mu_ 临界区内、以同一密钥成对写入（详见类级注释「项 A」）。
    UpdateResult rotateKeyAndReencrypt();

    // 热重载：启动后台线程监控 config.json 变更；变更后自动 reload+校验；失败时回滚。
    // - outApplied=true 代表确实更新了内存配置（调用方可据此决定是否重启模块）
    bool startWatching();
    void stopWatching();
    bool pollReloadOnce(bool& outApplied, std::string& outErr);  // 便于测试/无线程环境

#if defined(RK_ENABLE_TEST_SEAMS)
    // ── 测试接缝（编译期开关 RK_ENABLE_TEST_SEAMS；**本类接缝**的宏由 win_unit_tests 定义，见
    //    cmake/windows.cmake 与 cmake/core.cmake 的 rk_assert_test_seam_boundary。⚠️ 另一 target
    //    `core_gtest_tests` **也**定义同一宏，但用于 Engine 的接缝（onBeforeRenderPublish，见 Engine.h），
    //    与本类无关——故不可据此认为「全仓仅 win_unit_tests 定义该宏」）──
    // 下列钩子只服务「纯黑盒无法确定性观测并发交错」的若干竞态用例（见 tests/win/test_win_json_config.cpp）。
    //   · setReloadTestHookForTesting：在 reloadFromDisk「解析被拒之后、重新取锁复核 mtime 之前」同步调用。
    //     此刻**未持 mu_** ⇒ 钩子内可安全调 updateFromJsonBody（否则会自锁）。
    //   · setRotateTestHookForTesting：在 rotateKeyAndReencrypt「**取快照之后、生成并替换密钥之前**」
    //     同步调用（即竞态窗口的起点）；此刻同样**未持 mu_**。刻意放在密钥替换之前——否则磁盘上的
    //     postUrl 密文仍是旧密钥加密的，钩子内的合法 PUT 会在解析/解密处失败。
    //   · setReloadIsolationTestHookForTesting：在 reloadFromDisk「**复核 mtime 未变之后、
    //     quarantineConfigFile 之前**」同步调用——⚠️ 与前两枚的本质差异：**此刻已持 mu_**（该读取点与
    //     改名同在 reloadFromDisk 的那个 mu_ 临界区内）。故钩子内**绝不可**调用任何会取 mu_ 的公共 API
    //     （mu_ 非递归 ⇒ 自锁死，比断言失败更糟）；只允许「置位 / 通知另一线程」这类**不加锁**的动作。
    //     用途：给「hook 点（**复核与改名之间**）持 mu_」这一性质一个**可判别锚**——钩子唤醒另一线程去
    //     **尝试** PUT，而该 PUT 会被 mu_ 挡在临界区外 ⇒「hook 持锁窗口内 PUT 未提交」可被观测。
    //     ⚠️ 判别面**恰好**是「hook 点持 mu_」：把**复核**单独移出锁、或让复核与改名分属**两把锁**，hook 点
    //     仍可能落在其中某把锁内 ⇒ 本锚**不**判别这两种拆分形态（阴性变异实测为**绿**，详见对应用例的登记）。
    //   · setPutPreDecryptTestHookForTesting：在 updateFromJsonBody「**读父代基底 / 合并 / schema 校验之后，
    //     解密（parseAndValidateSettingsDoc）之前**」同步调用——⚠️ 修复项 2 之后**此刻已持 mu_**（该读取点与
    //     解密 / 加密 / 写盘同在这一个原子段内）。与 isolation 枚同理：钩子内**绝不可**调用任何会取 mu_ 的
    //     公共 API（自锁死）。用途：钉住「**解密**与提交在同一临界区、以同一密钥」——钩子唤醒另一线程去
    //     **尝试** rotate（轮换密钥），该 rotate 被 mu_ 挡在临界区外 ⇒ 解密读到的密钥恒等于基线密文所用的
    //     密钥（若解密落在锁外，rotate 会先换密钥 ⇒ 用新密钥解旧密文 ⇒ 响亮失败；详见对应用例）。
    //     ⚠️ 判别面**恰好**是「**解密**与提交同临界区」：把「**读父代基底** lastGoodJsonPretty_」单独移出锁、
    //     而解密仍在锁内时，hook 点仍在锁内 ⇒ 本锚**不**判别该形态（阴性变异实测为**绿**，详见对应用例的登记）。
    // ⚠️ **接缝回调的运行契约（承重，务必遵守）**：后两枚（reloadIsolation / putPreDecrypt）是在**持有
    //   非递归 `mu_` 时**被调用的 `std::function` ⇒ 钩子内**绝不可**调用任何会获取 `mu_` 的公共 API
    //   （如 store.current() / updateFromJsonBody / rotateKeyAndReencrypt / pollReloadOnce）。违反**不会**
    //   表现为「断言失败」，而是 `mu_` 不可重入导致的**挂起**——且会被 win_unit_tests 的 TIMEOUT 吞成一个
    //   难以定位的 Timeout（而非清晰的测试失败）。此约束**目前仅由注释约定**表达、**无结构强制** ⇒ 是已知的
    //   脆弱点。**已知的更优形态**是**控制反转**：生产侧只在接缝处**点亮一个原子量**，由测试侧**轮询观测**，
    //   从而彻底消除「在锁内执行任意回调」这一风险面。**本轮未改形态**，仅如实登记。
    // 语义与代价（**订正**，原措辞「生产路径行为**逐位不变**」已不准确）：**数据/语义不变**；
    //   ⚠️ 在**定义了本宏的测试构建**中，前两枚读取钩子会**新增一次 mu_ 获取/释放**（rotate 因有界重试
    //     最多 3 次）⇒ 非逐位不变，而是**多一次锁往返**；第三、四枚（isolation / putPreDecrypt）的读取点
    //     **已在 mu_ 内** ⇒ 直接读成员、**不新增**任何锁往返。
    //   ✅ **生产构建不定义本宏 ⇒ 整个接缝块（声明、成员、读取点）不参与编译 ⇒ 生产路径确实逐位不变**。
    // 生产代码**绝不**设置它们；未设置时空 std::function ⇒ `if (hook)` 恒假 ⇒ 无任何副作用。
    void setReloadTestHookForTesting(std::function<void()> hook);
    void setRotateTestHookForTesting(std::function<void()> hook);
    void setReloadIsolationTestHookForTesting(std::function<void()> hook);
    void setPutPreDecryptTestHookForTesting(std::function<void()> hook);
#endif

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

#if defined(RK_ENABLE_TEST_SEAMS)
    // 测试接缝（编译期开关；**本类接缝**的宏由 win_unit_tests 定义 —— 另一 target `core_gtest_tests`
    // 也定义同一宏但用于 Engine 的接缝（见 Engine.h）；生产不编译）。语义/数据不变，但前两枚读钩子
    // （reload / rotate）会新增一次 mu_ 获取/释放（见上方两枚 setter 的声明注释）；后两枚
    // （reloadIsolation / putPreDecrypt）的读取点**已在 mu_ 内**，不新增锁往返。
    std::function<void()> reloadTestHook_;
    std::function<void()> rotateTestHook_;
    std::function<void()> reloadIsolationTestHook_;
    std::function<void()> putPreDecryptTestHook_;
#endif
};

}  // namespace rk_win

