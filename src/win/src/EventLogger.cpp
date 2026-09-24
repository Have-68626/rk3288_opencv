#include "rk_win/EventLogger.h"

#include "rk_win/StructuredLogger.h"
#include "rk_win/WinStringUtil.h"

#include <filesystem>
#include <sstream>

namespace rk_win {
namespace {

// 与 StructuredLogger.cpp 中的同名辅助「各一份」：WinStringUtil.h 目前只把 escapeJson 统一了实现
// （其头注释称「替代 EventLogger/StructuredLogger/… 中的副本」，但这两个辅助尚未迁入）。把它们提到
// 共享头需改 WinStringUtil.h，不在本次改动的文件范围内 ⇒ 暂留本地副本（第 2 份，不是第 5 份）。
std::uint64_t fileSizeOr0(const std::filesystem::path& p) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(sz);
}

// 先删目标再改名（Windows 上 rename 不覆盖已存在目标）；错误**有意吞掉**——轮转是尽力而为，
// 失败不得影响事件写入（此时日志继续追加到当前文件）。吞错的代价由 rollIfNeededLocked 的
// 「close → 轮转 → 重开」顺序兜住：即使改名没成功，写句柄也始终指向 path_ 本身。
void renameBestEffort(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::remove(to, ec);
    ec.clear();
    std::filesystem::rename(from, to, ec);
}

}  // namespace

bool EventLogger::open(const std::filesystem::path& dir, std::uint64_t maxBytes, int maxRollFiles) {
    std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    path_ = dir / "event_log.jsonl";
    maxBytes_ = maxBytes;
    maxRollFiles_ = maxRollFiles;
    // 幂等：重复 open() 必须先断开旧句柄，否则旧句柄会让后续轮转的改名吃共享冲突。
    if (out_.is_open()) out_.close();
    // 冷启动即判一次：否则「上次退出时已超限」的文件会在本轮首次写入前一直超限（尺寸判据滞后一轮）。
    rollIfNeededLocked(path_);
    if (!out_.is_open()) {
        out_.open(path_, std::ios::out | std::ios::app);
    }
    return out_.is_open();
}

void EventLogger::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.close();
}

void EventLogger::rollIfNeededLocked(const std::filesystem::path& path) {
    // 轮转关闭时（0 = 不限，语义与 StructuredLogger 一致）直接返回，不改变任何打开状态。
    // ⚠️ 残留登记（b）（本次只登记、**不改行为**；详见头文件类级注释）：maxBytes_ == 0 或
    //   maxRollFiles_ <= 0 即**关闭轮转** ⇒ 该日志无界增长。JSON 侧 schema 的下限（maxFileBytes ≥ 1024、
    //   maxRollFiles ≥ 1）兜住了 JSON 路径，但 **INI 路径没有**——loadConfigFromIniOrDefault 的
    //   clamps 只压上界、不抬下界，手工把 INI 写成 0 会被原样带入 cfg。本批把 event_log.jsonl 从
    //   硬编码 10 MiB / 5 改为随 cfg.log 接线 ⇒ 这条（以及下面的代数收缩残留）的可达性随之扩大。
    if (maxBytes_ == 0 || maxRollFiles_ <= 0) return;

    // ★ 尺寸判据必须建立在**已落盘**的字节上：ofstream 有用户态缓冲，未 flush 的字节对
    //   std::filesystem::file_size 不可见 ⇒ 不先 flush 会让判据长期偏小、轮转被静默推迟甚至永不触发。
    if (out_.is_open()) out_.flush();
    if (fileSizeOr0(path) <= maxBytes_) return;

    // ★★ 与 `StructuredLogger::rollIfNeededLocked` 的**关键差异**（勿照搬那一段）：
    //   那段只 rename、从不 close 自己的 ofstream。Windows 上 std::ofstream 打开的文件默认不带
    //   `FILE_SHARE_DELETE`，对自己**仍打开**的文件改名通常以 `ERROR_SHARING_VIOLATION` 失败；
    //   而 renameBestEffort 会把该错误吞掉 ⇒ 轮转静默失效、文件无限增长。即便改名侥幸成功，写句柄
    //   仍指向已被改名的旧文件 ⇒ 之后的日志全部写进 `.1`，当前文件永远空着。故此处必须按
    //   「close → 轮转 → 以 app 重开」执行：任一环节失败都不会把日志写错文件。
    if (out_.is_open()) out_.close();

    // 代数迁移：`path.(N-1)` → `path.N`，最后 `path` → `path.1`（最新在 `.1`，与 StructuredLogger 一致）。
    // 上限即 maxRollFiles_：`path.maxRollFiles_` 由 renameBestEffort 的「先删目标」覆盖掉，不会越界。
    // ⚠️ 残留登记（a）（本次只登记、**不改行为**）：循环**只覆盖到 maxRollFiles_**，且**从不清理由大
    //   调小时留下的高代数档位**——`maxRollFiles` 若曾为 20、现改为 3，则 `.4` … `.20` 永远留在磁盘上
    //   （轮转不会删除它们）⇒「maxFileBytes × (maxRollFiles + 1)」的预算记账在收缩之后**不再成立**。
    //   StructuredLogger 同形态（既有，非本类引入）；本批把上限改为随配置接线后该形态更易被触发
    //   （运维可先调大再调小）。清理需主动删除超出 maxRollFiles_ 的档位，属行为变更，本批不做。
    for (int i = maxRollFiles_ - 1; i >= 1; --i) {
        const std::filesystem::path older = path.string() + "." + std::to_string(i);
        const std::filesystem::path newer = path.string() + "." + std::to_string(i + 1);
        if (std::filesystem::exists(older)) {
            renameBestEffort(older, newer);
        }
    }
    renameBestEffort(path, path.string() + ".1");

    // 轮转后立即重开：后续 append 继续写当前文件。重开失败（目录被删/无权限）时 out_ 保持关闭，
    // append 会因 `!out_.is_open()` 而安全跳过，不写坏任何文件。
    out_.open(path, std::ios::out | std::ios::app);
}

void EventLogger::append(const std::string& eventType, const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out_.is_open()) return;
    rollIfNeededLocked(path_);
    if (!out_.is_open()) return;  // 轮转后重开失败：本条事件丢弃，不得在错误状态下写
    out_ << "{"
         << "\"ts\":\"" << escapeJson(nowIso8601Local()) << "\","
         << "\"type\":\"" << escapeJson(eventType) << "\","
         << "\"msg\":\"" << escapeJson(message) << "\""
         << "}\n";
    // 每次写入后 flush（口径同 StructuredLogger::append）：轮转判据是磁盘文件尺寸，
    // 缓冲区里的字节对 file_size() 不可见；顺带让事件在进程崩溃时也不丢。
    out_.flush();
}

}  // namespace rk_win
