// tests/win/test_log_rotation.cpp
//
// 本批（日志轮转三件套）的判别用例。三组互相独立的被测面：
//   A. rk_win::pruneShrunkGenerations —— 纯文件系统原语（src/win/include/rk_win/LogRotation.h）
//   B. rk_win::StructuredLogger 轮转（src/win/src/StructuredLogger.cpp）
//   C. rk_win::RenderMetricsLogger 轮转（src/win/src/RenderMetricsLogger.cpp）
// EventLogger 的**代数收缩清理**面在 tests/win/test_event_logger.cpp 内追加（复用其 TempLogDir /
// pathExists，勿在此重复定义）。
//
// 纪律：每个用例一个独立的**系统临时目录**（绝不在 cwd / 工作树落文件）；失败时用 std::cerr
// 打期望值与实际值，随后一律 return false；「前置假设不成立」按既有 test_event_logger.cpp 的
// 口径处理（打说明并 return false），不静默放过。
//
// ⚠️ 注释里的判别点全部以**符号名**指代加坏点（不写行号），且只登记**代码事实**——不得声称
//    「必然红在某条共享冲突上」（见 B3 的双面登记的成因分析）。

#include "rk_win/LogRotation.h"
#include "rk_win/RenderMetricsLogger.h"
#include "rk_win/StructuredLogger.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// 每个用例一个独立的**系统临时目录**：与 tests/win/test_event_logger.cpp 的 TempLogDir 同款
// （匿名命名空间 ⇒ 内部链接，两份互不冲突）。构造时先 remove_all 清残留（含上次留下的 .1/.2），
// 析构时再清理；**绝不**用 cwd / 相对路径落文件——否则会把日志与轮转档位写进运行目录。
class TempLogDir {
public:
    explicit TempLogDir(const std::string& tag) {
        std::error_code ec;
        path_ = std::filesystem::temp_directory_path(ec) / ("rk3288_opencv_log_rotation_" + tag);
        std::filesystem::remove_all(path_, ec);  // 清掉上次运行残留，保证判据不被污染
        std::filesystem::create_directories(path_, ec);
    }
    ~TempLogDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempLogDir(const TempLogDir&) = delete;
    TempLogDir& operator=(const TempLogDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::uintmax_t sizeOf(const std::filesystem::path& p) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    return ec ? 0 : sz;
}

// ⚠️ 刻意不取名 exists()：形参是 std::filesystem::path，非限定 exists(p) 会因 ADL 把
// std::filesystem::exists 一并拉进重载集 ⇒ C2668（与 test_event_logger.cpp 同一坑，沿用其命名）。
bool pathExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

// 直接写文件（不经任何日志器）：用于预置「当前文件」与「既有轮转档位」。
bool writeFile(const std::filesystem::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::in | std::ios::binary);
    if (!f.is_open()) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string firstLineOf(const std::string& content) {
    const auto pos = content.find('\n');
    return pos == std::string::npos ? content : content.substr(0, pos);
}

// 造一条可控尺寸的帧日志：以 errorMessage（长串）把单条记录撑到 maxBytes 之上，使「每次写入
// 都触发轮转」这一前提成立；cameraName 承载 PRE/POST 标记（见 B3），用于区分轮转前后内容。
rk_win::FrameLogEntry makeFrameEntry(const std::string& cameraName, const std::string& errorMessage) {
    rk_win::FrameLogEntry e;
    e.tsIso8601 = "2026-01-01T00:00:00.000";
    e.cameraName = cameraName;
    e.cameraId = "0";
    e.frameIndex = 1;
    e.frameWidth = 640;
    e.frameHeight = 480;
    e.fps = 30.0;
    e.errorMessage = errorMessage;
    return e;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════
// A. pruneShrunkGenerations（纯文件系统，不碰任何日志器）
// ═════════════════════════════════════════════════════════════════════

// A1 正常收缩：预置 x.log.1 … x.log.6，以 maxRollFiles=3 调用 ⇒ 删 .4/.5/.6、留 .1/.2/.3、返回 3。
// 判别点：删掉本原语里真正的删除分支（对匹配项 `std::filesystem::remove` 那一段）⇒「返回 3」与
//   「.4/.5/.6 消失」两条断言同时红；把 `gen <= maxRollFiles continue` 这条边界放宽（误删 <= 上限的
//   档位）⇒「.1/.2/.3 必须保留」那条红。
bool test_prune_shrunk_generations_removes_excess() {
    TempLogDir tmp("prune_removes_excess");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path cur = dir / "x.log";
    for (int i = 1; i <= 6; ++i) {
        writeFile(dir / ("x.log." + std::to_string(i)), "g" + std::to_string(i));
    }

    const int removed = rk_win::pruneShrunkGenerations(cur, 3);
    if (removed != 3) {
        std::cerr << "[prune_removes_excess] 返回 " << removed << "，期望 3" << std::endl;
        return false;
    }
    for (int i = 1; i <= 3; ++i) {
        if (!pathExists(dir / ("x.log." + std::to_string(i)))) {
            std::cerr << "[prune_removes_excess] 档位 x.log." << i
                      << " 被误删（N <= maxRollFiles 的档位属轮转循环管辖，必须保留）" << std::endl;
            return false;
        }
    }
    for (int i = 4; i <= 6; ++i) {
        if (pathExists(dir / ("x.log." + std::to_string(i)))) {
            std::cerr << "[prune_removes_excess] 档位 x.log." << i
                      << " 未被删（N > maxRollFiles 的档位必须清掉）" << std::endl;
            return false;
        }
    }
    return true;
}

// A2 不碰 current 自身：预置 x.log（当前文件）与 x.log.1 ⇒ 以 maxRollFiles=1 调用后两者仍在、返回 0。
// 判别点：把「current 自身」也算进可删集合（例如判据写成 `name.size() >= prefix.size()` 或直接拿
//   current 去 remove）⇒ 返回 0 / x.log 仍在 两条断言红。
bool test_prune_shrunk_generations_spares_current() {
    TempLogDir tmp("prune_spares_current");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path cur = dir / "x.log";
    if (!writeFile(cur, "current-body")) return false;
    if (!writeFile(dir / "x.log.1", "g1")) return false;

    const int removed = rk_win::pruneShrunkGenerations(cur, 1);
    if (removed != 0) {
        std::cerr << "[prune_spares_current] 返回 " << removed << "，期望 0（无 N > 1 的档位）" << std::endl;
        return false;
    }
    if (!pathExists(cur)) {
        std::cerr << "[prune_spares_current] 当前文件 x.log 被误删（文件名不含 .N 后缀，天然不应匹配）"
                  << std::endl;
        return false;
    }
    if (!pathExists(dir / "x.log.1")) {
        std::cerr << "[prune_spares_current] x.log.1 被误删（N <= maxRollFiles=1 必须保留）" << std::endl;
        return false;
    }
    return true;
}

// A3 干扰项不删（★）：预置 x.log.bak / x.log.corrupt.<ts> / x.log.tmp / x.log.01abc / x.logger
// ⇒ 以 maxRollFiles=0（轮转关闭）与 =1 两种调用后，这些文件全部仍在。
// 判别点：删掉「后缀必须是纯十进制数字」的判据（`suffix.find_first_not_of("0123456789")`）⇒ 带非数字
//   后缀的 .bak / .corrupt.<ts> / .tmp / .01abc 会被 remove 掉 ⇒ 红。注释要求：对它们调 remove 会
//   **误删别的功能的数据**（.bak 是配置备份、.corrupt.<ts> 是隔离档、.tmp 是写入中转）。
// x.logger 是**前缀相似但不同基名**（不含 "x.log." 前缀）⇒ 前缀比较不匹配、天然不删。
bool test_prune_shrunk_generations_spares_foreign_suffixes() {
    TempLogDir tmp("prune_spares_foreign");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path cur = dir / "x.log";
    const std::vector<std::string> foreign = {
        "x.log.bak",
        "x.log.corrupt.1737000000000",
        "x.log.tmp",
        "x.log.01abc",
        "x.logger",
    };
    for (const std::string& n : foreign) {
        if (!writeFile(dir / n, "keep-me")) return false;
    }

    const int caps[] = {0, 1};
    for (int cap : caps) {
        rk_win::pruneShrunkGenerations(cur, cap);
        for (const std::string& n : foreign) {
            if (!pathExists(dir / n)) {
                std::cerr << "[prune_spares_foreign] maxRollFiles=" << cap << " 误删干扰项 " << n
                          << "（后缀非纯十进制 / 前缀不符者一律不得删除）" << std::endl;
                return false;
            }
        }
    }
    return true;
}

// A4 maxRollFiles <= 0（轮转显式关闭）⇒ 什么都不删：预置 .1/.5，以 0 调用返回 0 且两者仍在；
// 以 -1 调用同样。
// 判别点：删掉本原语首行 `if (maxRollFiles <= 0) return 0;` ⇒ 负值/零值下会按 `gen <= 0` 的荒谬边界
//   把 .1/.5 全部清掉 ⇒「返回 0」与「两者仍在」两条断言红。
bool test_prune_shrunk_generations_noop_when_disabled() {
    TempLogDir tmp("prune_disabled");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path cur = dir / "x.log";
    writeFile(dir / "x.log.1", "g1");
    writeFile(dir / "x.log.5", "g5");

    const int caps[] = {0, -1};
    for (int cap : caps) {
        const int removed = rk_win::pruneShrunkGenerations(cur, cap);
        if (removed != 0) {
            std::cerr << "[prune_disabled] maxRollFiles=" << cap << " 返回 " << removed
                      << "，期望 0（关闭轮转 ⇒ 不删任何既有档位）" << std::endl;
            return false;
        }
        if (!pathExists(dir / "x.log.1") || !pathExists(dir / "x.log.5")) {
            std::cerr << "[prune_disabled] maxRollFiles=" << cap
                      << " 误删档位（关闭轮转时既有档位不是本类产物，删除属破坏性操作）" << std::endl;
            return false;
        }
    }
    return true;
}

// A5 目录不存在 / 路径无父目录：不崩、返回 0。
// 判别点（(i) 段）：把实现里 `directory_iterator` 的 `error_code` 判据删掉（改为无条件构造）⇒
//   目录不存在时会抛 filesystem_error ⇒ 本用例的 try/catch 命中 ⇒ 红。
// ⚠️ (ii) 段**不构成判别点**（首版注释称「把 `dir.empty()` 分支删掉同理」，**不成立**，已订正）：
//   删掉 `.` 兜底后，`directory_iterator(path(""))` 无论「以 error_code 失败」还是「按 `.` 迭代」，
//   最终都落到「无匹配项 ⇒ 返回 0」，与保留兜底的读数**完全相同** ⇒ 该段只验证「不崩、返回 0」，
//   是**契约面护栏**而非判别器。要真正判别 `.` 兜底需构造「cwd 下存在匹配档位」的环境，
//   而本用例刻意**不在 cwd 落任何文件**（纪律见文件头），故如实登记为覆盖缺口。
bool test_prune_shrunk_generations_noop_on_missing_dir() {
    TempLogDir tmp("prune_missing_dir");
    const std::filesystem::path dir = tmp.path();

    // (i) 目录不存在 ⇒ directory_iterator 以 error_code 失败 ⇒ 返回 0、不抛。
    const std::filesystem::path missing = dir / "no_such_subdir" / "x.log";
    int r1 = -1;
    try {
        r1 = rk_win::pruneShrunkGenerations(missing, 1);
    } catch (const std::exception& ex) {
        std::cerr << "[prune_missing_dir] 目录不存在时抛异常：" << ex.what() << std::endl;
        return false;
    }
    if (r1 != 0) {
        std::cerr << "[prune_missing_dir] 目录不存在时返回 " << r1 << "，期望 0" << std::endl;
        return false;
    }

    // (ii) 路径无父目录：实现以 "." 兜底 ⇒ 只读扫描 cwd、无匹配即返回 0。用**确保不存在**的唯一基名，
    //      以免在 cwd 里误删同名档位（本用例只断言「不崩、返回 0」，不制造任何匹配文件）。
    int r2 = -1;
    try {
        r2 = rk_win::pruneShrunkGenerations(std::filesystem::path("rk3288_prune_noparent_probe.log"), 3);
    } catch (const std::exception& ex) {
        std::cerr << "[prune_missing_dir] 无父目录路径时抛异常：" << ex.what() << std::endl;
        return false;
    }
    if (r2 != 0) {
        std::cerr << "[prune_missing_dir] 无父目录路径返回 " << r2 << "，期望 0（唯一基名无匹配档位）"
                  << std::endl;
        return false;
    }
    return true;
}

// A6 返回计数准确：预置 5 个超界档位 ⇒ 返回值恰为 5。
// 判别点：把 `++removed` 挪出「删除成功」分支（例如每个匹配项都计数、或删失败也计数）⇒ 计数失真 ⇒ 红。
bool test_prune_shrunk_generations_count_is_exact() {
    TempLogDir tmp("prune_count");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path cur = dir / "x.log";
    for (int i = 2; i <= 6; ++i) {  // .2/.3/.4/.5/.6 —— 共 5 个，全部 > maxRollFiles=1
        writeFile(dir / ("x.log." + std::to_string(i)), "g");
    }

    const int removed = rk_win::pruneShrunkGenerations(cur, 1);
    if (removed != 5) {
        std::cerr << "[prune_count] 返回 " << removed << "，期望 5（.2 … .6 共 5 个超界档位）" << std::endl;
        return false;
    }
    return true;
}

// A7 超大后缀（20 位十进制）：被删且**不抛异常**。
// 判别点：把实现里手动截断的溢出判定换回 `std::stoi(suffix)` ⇒ 20 位后缀触发 out_of_range ⇒ 本用例
//   捕获到异常而红（同时整轮清理被中断）。
bool test_prune_shrunk_generations_huge_suffix_no_throw() {
    TempLogDir tmp("prune_huge_suffix");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path cur = dir / "x.log";
    const std::filesystem::path huge = dir / "x.log.99999999999999999999";  // 20 位，远超 int
    if (!writeFile(huge, "g")) return false;

    int removed = -1;
    try {
        removed = rk_win::pruneShrunkGenerations(cur, 1);
    } catch (const std::exception& ex) {
        std::cerr << "[prune_huge_suffix] 抛异常：" << ex.what()
                  << "（实现刻意回避 std::stoi 的 out_of_range）" << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[prune_huge_suffix] 抛出非 std::exception 异常" << std::endl;
        return false;
    }
    if (removed != 1 || pathExists(huge)) {
        std::cerr << "[prune_huge_suffix] 返回 " << removed << "、存在=" << pathExists(huge)
                  << "；期望返回 1 且已删除（超大后缀应视为远超上限、删除而非抛异常）" << std::endl;
        return false;
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════
// B. StructuredLogger 轮转（recognition.csv / recognition.jsonl）
// ═════════════════════════════════════════════════════════════════════

// B1 轮转真正生效：maxBytes 故意小于单条记录 ⇒ 写 20 条后 .1/.2 存在、当前文件存在且非空、
// 当前文件尺寸有上界（<= maxBytes + 单条）、代数不超过 maxRollFiles（无 .3）。
// 判别点：删掉 StructuredLogger::append 里对 rollIfNeededLocked 的两次调用 ⇒ 无轮转 ⇒「.1 存在」与
//   「尺寸 <= maxBytes + 单条」两条断言红（无轮转时当前文件为 20 条之和，远超大界）。
// 另一侧（jsonl.1）断言：轮转须对**两个**输出文件都生效——若只对 csv 调 rollIfNeededLocked，则
//   recognition.jsonl.1 缺失 ⇒ 红。
bool test_structured_logger_rotates_and_caps_generations() {
    TempLogDir tmp("structured_rotate");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path csv = dir / "recognition.csv";
    const std::filesystem::path csv1 = dir / "recognition.csv.1";
    const std::filesystem::path csv2 = dir / "recognition.csv.2";
    const std::filesystem::path csv3 = dir / "recognition.csv.3";
    const std::filesystem::path jsonl1 = dir / "recognition.jsonl.1";

    const std::uint64_t maxBytes = 100;  // 故意小于单条记录 ⇒ 首条之后每次写入都触发轮转
    const int maxRollFiles = 2;
    rk_win::StructuredLogger log;
    if (!log.open(dir, maxBytes, maxRollFiles)) return false;

    const std::string marker(200, 'x');
    log.append(makeFrameEntry("cam", marker));
    const std::uintmax_t singleLine = sizeOf(csv);  // 首条后尚未轮转 ⇒ 即「表头 + 一行」的字节数
    // 前置假设：单条（含表头）必须大于 maxBytes，且 20 条足以跨越至少两次轮转。否则代数断言不成立，
    // 属用例前提失效而非实现缺陷（照 test_event_logger.cpp 的处理：打说明并 return false）。
    if (singleLine == 0 || singleLine * 19 <= 2 * maxBytes) {
        std::cerr << "[structured_rotate] 记录长度 " << singleLine << " 与 maxBytes=" << maxBytes
                  << " 不匹配——用例前置假设不成立（单条须 > maxBytes 才能每次写入都触发轮转）"
                  << std::endl;
        return false;
    }
    for (int i = 0; i < 19; ++i) {  // 合计 20 条
        log.append(makeFrameEntry("cam", marker));
    }

    if (!pathExists(csv) || sizeOf(csv) == 0) {
        std::cerr << "[structured_rotate] 当前 recognition.csv 缺失或为空——写句柄可能跟随了被改名的旧文件"
                  << std::endl;
        return false;
    }
    if (!pathExists(csv1) || sizeOf(csv1) == 0) {
        std::cerr << "[structured_rotate] 未发生轮转：recognition.csv.1 缺失或为空" << std::endl;
        return false;
    }
    if (!pathExists(csv2)) {
        std::cerr << "[structured_rotate] 代数未推进到 .2（maxRollFiles=" << maxRollFiles << "）" << std::endl;
        return false;
    }
    if (pathExists(csv3)) {
        std::cerr << "[structured_rotate] 代数越界：出现 recognition.csv.3，超过 maxRollFiles="
                  << maxRollFiles << std::endl;
        return false;
    }
    if (sizeOf(csv) > maxBytes + singleLine) {
        std::cerr << "[structured_rotate] 当前 recognition.csv " << sizeOf(csv) << " 字节，超过上界 "
                  << (maxBytes + singleLine) << "——轮转未生效" << std::endl;
        return false;
    }
    if (!pathExists(jsonl1)) {
        std::cerr << "[structured_rotate] recognition.jsonl.1 缺失——轮转未对 jsonl 输出生效" << std::endl;
        return false;
    }
    return true;
}

// B2 maxBytes == 0 ⇒ 不轮转（0 = 不限）：无 .1 出现，且 JSONL 当前文件总字节 == 单条 × 条数
// （证明一条未丢、未截断；口径照 test_event_logger.cpp 的同款用例——JSONL 无表头，故可直接乘条数）。
// 判别点：删掉 rollIfNeededLocked 里 `maxBytes_ == 0` 的判断 ⇒ 出现 .1 ⇒ 红。
bool test_structured_logger_no_rotation_when_max_bytes_zero() {
    TempLogDir tmp("structured_norotate");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path jsonl = dir / "recognition.jsonl";

    rk_win::StructuredLogger log;
    if (!log.open(dir, 0, 5)) return false;

    const std::string marker(200, 'x');
    log.append(makeFrameEntry("cam", marker));
    const std::uintmax_t singleLine = sizeOf(jsonl);  // JSONL 无表头 ⇒ 即单条记录的字节数
    if (singleLine == 0) {
        std::cerr << "[structured_norotate] 首条写入后 recognition.jsonl 为空——append 未落盘" << std::endl;
        return false;
    }
    for (int i = 0; i < 19; ++i) {  // 合计 20 条
        log.append(makeFrameEntry("cam", marker));
    }

    if (pathExists(dir / "recognition.jsonl.1") || pathExists(dir / "recognition.csv.1")) {
        std::cerr << "[structured_norotate] maxBytes=0 仍发生轮转（出现了 .1 档位）" << std::endl;
        return false;
    }
    if (sizeOf(jsonl) != singleLine * 20) {
        std::cerr << "[structured_norotate] recognition.jsonl " << sizeOf(jsonl) << " 字节，期望 "
                  << (singleLine * 20) << "（不轮转路径不得丢记录/不得截断）" << std::endl;
        return false;
    }
    return true;
}

// B3 ★「只 rename 不 close」形态必然被打红（本批的核心判别点）。断言是**双面**的：
//   ① 当前文件存在且**非空**；② 当前文件内**只含**轮转后写入的 POST 记录（不含轮转前的 PRE），
//   且 .1 里是轮转前的 PRE 内容；另有「当前文件尺寸 <= maxBytes + 单条」的上界断言。
// 前 4 条写 PRE、第 5 条写 POST：正确实现下「每次写入先轮转再写」，故末态 current 恰含唯一的 POST、
//   .1 恰含最后一条 PRE（代数迁移：PRE4→.1）。
// 注释如实说明该断言在两种环境行为下分别红在哪条（**不声称必然红在共享冲突上**）：
//   · 若改名因共享冲突失败（本机 Windows 上 std::ofstream 默认不带 FILE_SHARE_DELETE 的常见形态）
//     ⇒ renameBestEffort 吞错、轮转静默失效 ⇒ 当前文件累积全部 5 条 ⇒ **尺寸上界断言**红（且 current
//     仍含 PRE ⇒ **②** 也红）。
//   · 若改名侥幸成功（如 CRT 以 FILE_SHARE_DELETE 打开、或 POSIX 语义）⇒ 写句柄仍跟随被改名走的旧
//     文件、当前路径不再被新数据写 ⇒ 当前文件缺失/为空 ⇒ **①** 红。
//   ⇒ 两种行为都必红，但红的断言未必是同一条。
bool test_structured_logger_write_handle_follows_current() {
    TempLogDir tmp("structured_handle");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path csv = dir / "recognition.csv";
    const std::filesystem::path csv1 = dir / "recognition.csv.1";

    const std::uint64_t maxBytes = 100;  // 小于单条 ⇒ 每次写入都触发轮转
    const int maxRollFiles = 3;
    rk_win::StructuredLogger log;
    if (!log.open(dir, maxBytes, maxRollFiles)) return false;

    const std::string pad(200, 'x');
    log.append(makeFrameEntry("PRE_MARKER", pad));
    const std::uintmax_t singleLine = sizeOf(csv);
    if (singleLine == 0 || singleLine <= maxBytes) {
        std::cerr << "[structured_handle] 单条 " << singleLine << " 字节，未超过 maxBytes=" << maxBytes
                  << "——用例前置假设不成立（每次写入都须触发轮转）" << std::endl;
        return false;
    }
    for (int i = 0; i < 3; ++i) {  // 合计 4 条 PRE
        log.append(makeFrameEntry("PRE_MARKER", pad));
    }
    if (!pathExists(csv1) || sizeOf(csv1) == 0) {
        std::cerr << "[structured_handle] 4 条 PRE 后仍未轮转出 recognition.csv.1——用例前置假设不成立"
                  << std::endl;
        return false;
    }

    log.append(makeFrameEntry("POST_MARKER", pad));  // 第 5 条：轮转后写入的唯一 POST

    // ① 当前文件必须存在且非空。
    if (!pathExists(csv) || sizeOf(csv) == 0) {
        std::cerr << "[structured_handle] 当前文件缺失或为空——写句柄可能跟随了被改名走的旧文件"
                     "（“只 rename 不 close”形态在改名侥幸成功时的红点）" << std::endl;
        return false;
    }
    const std::string cur = readFile(csv);
    const std::string gen1 = readFile(csv1);
    // ② 当前文件只含轮转后写入的 POST，PRE 内容必须已在 .1。
    const bool curHasPre = cur.find("PRE_MARKER") != std::string::npos;
    const bool curHasPost = cur.find("POST_MARKER") != std::string::npos;
    if (!curHasPost || curHasPre) {
        std::cerr << "[structured_handle] 当前文件内容错：应只含 POST_MARKER（实际 PRE 出现=" << curHasPre
                  << "、POST 出现=" << curHasPost << "）" << std::endl;
        return false;
    }
    if (gen1.find("PRE_MARKER") == std::string::npos) {
        std::cerr << "[structured_handle] .1 未包含轮转前的 PRE 内容——轮转可能未真正发生" << std::endl;
        return false;
    }
    // 尺寸上界：轮转静默失效时当前文件会累积全部 5 条 ⇒ 红。
    if (sizeOf(csv) > maxBytes + singleLine) {
        std::cerr << "[structured_handle] 当前 recognition.csv " << sizeOf(csv) << " 字节，超过上界 "
                  << (maxBytes + singleLine) << "——轮转未生效" << std::endl;
        return false;
    }
    return true;
}

// B4 冷启动即判一次尺寸（只有 open() 里的那一行能保证）：先直接写文件造出「已超过 maxBytes」的
// 当前文件（不经日志器），然后**只调 open 不调 append** ⇒ 断言 .1 已出现、当前文件为空。
// 判别点：删掉 StructuredLogger::open 里对 rollIfNeededLocked 的调用 ⇒ 冷启动不轮转 ⇒ .1 缺失、当前
//   文件仍为 500 字节 ⇒ 红。csv 与 jsonl 都要断言（open 须对两个路径都做冷启动判尺寸）。
bool test_structured_logger_open_rolls_oversized_cold_start() {
    TempLogDir tmp("structured_coldstart");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path csv = dir / "recognition.csv";
    const std::filesystem::path jsonl = dir / "recognition.jsonl";
    // 直接写文件（不经日志器）造出「上次退出时已超限」的当前文件。
    if (!writeFile(csv, std::string(500, 'c'))) return false;
    if (!writeFile(jsonl, std::string(500, 'j'))) return false;

    rk_win::StructuredLogger log;
    if (!log.open(dir, 100, 2)) return false;  // 只 open，不 append

    const std::filesystem::path csv1 = dir / "recognition.csv.1";
    const std::filesystem::path jsonl1 = dir / "recognition.jsonl.1";
    if (!pathExists(csv1) || sizeOf(csv1) == 0) {
        std::cerr << "[structured_coldstart] 冷启动未轮转：recognition.csv.1 缺失或为空" << std::endl;
        return false;
    }
    if (sizeOf(csv) != 0) {
        std::cerr << "[structured_coldstart] 冷启动轮转后当前 recognition.csv 应为空，实际 "
                  << sizeOf(csv) << " 字节" << std::endl;
        return false;
    }
    if (!pathExists(jsonl1) || sizeOf(jsonl1) == 0) {
        std::cerr << "[structured_coldstart] 冷启动未轮转：recognition.jsonl.1 缺失或为空" << std::endl;
        return false;
    }
    if (sizeOf(jsonl) != 0) {
        std::cerr << "[structured_coldstart] 冷启动轮转后当前 recognition.jsonl 应为空，实际 "
                  << sizeOf(jsonl) << " 字节" << std::endl;
        return false;
    }
    return true;
}

// B5 幂等 open：连续两次 open(同一目录) 后，日志器仍**真正可写**、且仍能正常轮转。
// 判别点（⚠️ **必须成对改动才会红**——这是首版注释的失真处，由对抗性审计抓出并在此如实登记）：
//   · **只**去掉 `open()` 里「先断开旧句柄」的 close()（csv_/jsonl_ 两处）而保留重开前的
//     `if (!csv_.is_open())` 守卫 ⇒ 第二次 open **根本不会**对已打开的流调 open()（被守卫拦住）
//     ⇒ 无任何副作用 ⇒ **绿**（首版注释称此处会红，不成立）。
//   · **只**去掉守卫而保留 close() ⇒ 先 close 再 open，正常 ⇒ 绿。
//   · ★ **两者同时去掉** ⇒ 第二次 open 对已打开的 ofstream 调 open() ⇒ 置 **failbit（粘滞）** ⇒
//     此后所有 `csv_ << …` 静默变成空操作、而 `is_open()` 仍返回 true ⇒ 日志一条都不落
//     ⇒ 末尾「当前 recognition.csv 存在且非空」与「`.1` 存在」两条断言同时红。
//   结论：守卫存在时 close() 是**冗余防御**（与 `EventLogger::open` 同款口径，保留它是为了与
//   兄弟实现形态一致 + 万一守卫被删时仍有防线）；本用例的真实价值是**同时拆掉两道防线时必须红**。
bool test_structured_logger_open_is_idempotent() {
    TempLogDir tmp("structured_idempotent");
    const std::filesystem::path dir = tmp.path();
    rk_win::StructuredLogger log;
    if (!log.open(dir, 100, 2)) return false;
    if (!log.open(dir, 100, 2)) {
        std::cerr << "[structured_idempotent] 第二次 open(同一目录) 返回 false——旧句柄未断开"
                     "（Windows 上未 close 的 ofstream 会令改名吃共享冲突 ⇒ 轮转失效）" << std::endl;
        return false;
    }

    const std::string pad(200, 'x');
    for (int i = 0; i < 20; ++i) {
        log.append(makeFrameEntry("cam", pad));
    }
    const std::filesystem::path csv1 = dir / "recognition.csv.1";
    const std::filesystem::path csv = dir / "recognition.csv";
    if (!pathExists(csv1) || sizeOf(csv1) == 0) {
        std::cerr << "[structured_idempotent] 重复 open 后仍无法轮转：recognition.csv.1 缺失或为空"
                  << std::endl;
        return false;
    }
    if (!pathExists(csv) || sizeOf(csv) == 0) {
        std::cerr << "[structured_idempotent] 重复 open 后当前 recognition.csv 缺失或为空" << std::endl;
        return false;
    }
    return true;
}

// B6 CSV 表头不丢（★ 与冷启动轮转耦合）：先造超限文件再 open（触发冷启动轮转、当前 CSV 清空），
// 然后 append 一条 ⇒ 当前 CSV 的**首行必须是表头**。
// 判别点：把 open() 里 `headerWritten_` 的赋值挪到轮转**之前** ⇒ 冷启动前文件非空 ⇒ headerWritten_
//   被置 true ⇒ 轮转清空后本条写入不再补表头 ⇒ 首行是数据行（以条目的 tsIso8601 开头）⇒ 红。
bool test_structured_logger_header_preserved_across_open_rotation() {
    TempLogDir tmp("structured_header");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path csv = dir / "recognition.csv";
    if (!writeFile(csv, std::string(500, 'c'))) return false;  // 超限文件 ⇒ open 时冷启动轮转

    rk_win::StructuredLogger log;
    if (!log.open(dir, 100, 2)) return false;

    rk_win::FrameLogEntry e = makeFrameEntry("cam", std::string(200, 'x'));
    e.tsIso8601 = "TS_MARKER";  // 数据行首列；若表头丢了，首行会以它开头
    log.append(e);

    const std::string first = firstLineOf(readFile(csv));
    if (first.rfind("ts,camera_name,", 0) != 0) {
        std::cerr << "[structured_header] 轮转后当前 CSV 首行不是表头，实际首行=\"" << first
                  << "\"（把 headerWritten_ 的赋值挪到轮转之前即会红）" << std::endl;
        return false;
    }
    return true;
}

// B7 ★ CSV 表头不丢（**append 侧触发的轮转**，与上面的 B6 互补）。B6 覆盖 open 冷启动轮转，
// 本条覆盖运行期轮转——两者是**不同**的代码路径：B6 由 open() 之后重算 headerWritten_ 兜住，
// 本条只能由 append() 依据 rollIfNeededLocked 的**返回值**复位 headerWritten_ 兜住。
//
// 判别点：把 append 里的 `if (rollIfNeededLocked(csvPath_, csv_)) headerWritten_ = false;` 退回
//   无返回值的调用（即不复位）⇒ 轮转把当前 CSV 改名走、重开成空文件，而 headerWritten_ 仍为 true
//   ⇒ 第二条 append 不再补表头 ⇒ 当前 CSV 首行是**数据行**（以条目的 tsIso8601 开头）⇒ 红。
// ⚠️ 本条在本批之前**不可达**：那时「只 rename 不 close」让轮转在 Windows 上静默失效、文件从不改名，
//   故表头一直留着——是「让轮转真正生效」把这条缺口暴露出来的。
bool test_structured_logger_header_preserved_across_append_rotation() {
    TempLogDir tmp("structured_header_append");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path csv = dir / "recognition.csv";

    rk_win::StructuredLogger log;
    if (!log.open(dir, 100, 2)) return false;  // 上限 100B ⇒ 第一条写入后必然超限

    // 第 1 条：open 后文件为空 ⇒ headerWritten_ 为 false ⇒ 本条写入时补表头。
    log.append(makeFrameEntry("cam", std::string(200, 'x')));
    // 前提校验：轮转判据在 **append 的开头**（写前判尺寸），故第 1 条之后磁盘上已超限、但 `.1` **尚未**
    // 出现——`.1` 只会在第 2 条进入时才产生。此处必须按**尺寸**判前提；用「`.1` 是否存在」判会永不
    // 成立（那是本条用例第一版的构造错误，已由首跑抓出）。
    if (sizeOf(csv) <= 100) {
        std::cerr << "[structured_header_append] 前提不成立：第 1 条写入后当前 CSV 仅 " << sizeOf(csv)
                  << " 字节（未超过 maxBytes=100），第 2 条不会触发轮转" << std::endl;
        return false;
    }

    // 第 2 条：进入 append 时已超限 ⇒ 触发轮转（当前文件改名走、重开成空文件）⇒ 必须重新补表头。
    rk_win::FrameLogEntry e = makeFrameEntry("cam", std::string(200, 'x'));
    e.tsIso8601 = "TS_MARKER";  // 数据行首列；若表头丢了，首行会以它开头
    log.append(e);

    const std::string first = firstLineOf(readFile(csv));
    if (first.rfind("ts,camera_name,", 0) != 0) {
        std::cerr << "[structured_header_append] 运行期轮转后当前 CSV 首行不是表头，实际首行=\"" << first
                  << "\"（append 未据 rollIfNeededLocked 的返回值复位 headerWritten_ 即会红）" << std::endl;
        return false;
    }
    return true;
}
// B8 代数收缩清理在 open 生效：预置 .1 … .6，以 maxRollFiles=2 open ⇒ .3 … .6 被清掉、.1/.2 仍在。
// 判别点：删掉 open() 里对 pruneShrunkGenerations 的调用 ⇒ .3 … .6 永久滞留 ⇒ 红。csv 与 jsonl 两个
//   输出路径都须清理（同一 open 里逐路径调用本原语）。
bool test_structured_logger_open_prunes_shrunk_generations() {
    TempLogDir tmp("structured_prune");
    const std::filesystem::path dir = tmp.path();
    for (int i = 1; i <= 6; ++i) {
        writeFile(dir / ("recognition.csv." + std::to_string(i)), "g");
        writeFile(dir / ("recognition.jsonl." + std::to_string(i)), "g");
    }

    rk_win::StructuredLogger log;
    if (!log.open(dir, 1ULL << 30, 2)) return false;  // maxBytes 极大 ⇒ 本用例只观察代数收缩

    for (int i = 1; i <= 2; ++i) {
        const std::string n = std::to_string(i);
        if (!pathExists(dir / ("recognition.csv." + n)) || !pathExists(dir / ("recognition.jsonl." + n))) {
            std::cerr << "[structured_prune] 档位 ." << i << " 被误删（N <= maxRollFiles=2 必须保留）"
                      << std::endl;
            return false;
        }
    }
    for (int i = 3; i <= 6; ++i) {
        const std::string n = std::to_string(i);
        if (pathExists(dir / ("recognition.csv." + n)) || pathExists(dir / ("recognition.jsonl." + n))) {
            std::cerr << "[structured_prune] 档位 ." << i << " 未删（N > maxRollFiles=2 必须清掉）"
                      << std::endl;
            return false;
        }
    }
    return true;
}

// B9 ★ 改名失败时**不得**当作「已轮转」（与 B7 互补的另一半，本批第二处「修复自己挖的洞」）。
// 语义：`rollIfNeededLocked` 的返回值必须区分「超限了但**改名失败**」与「真的把当前文件轮转走」。
//   前者当前文件仍带着旧内容与旧表头，若此时复位 `headerWritten_`，紧接着的写入就会往**同一个**
//   文件再写一行表头 ⇒ CSV 中段出现**第二行表头**（朴素解析器行数错乱）。
// 构造：① 先正常写一条，使当前 CSV 恰含**一个**表头且已超限；② 用一个**独占句柄**钉住
//   `recognition.csv.1`，使轮转的 `remove(.1)` 与 `rename(current, .1)` 双双失败
//   （MSVC 的 std::ofstream 不含 FILE_SHARE_DELETE）；③ 再 append 一条 ⇒ 触发一次注定失败的轮转。
// 判别点：把 `return rotatedAway;` 改回无条件 `return true;` ⇒ ③ 复位表头并再写一行 ⇒
//   「当前 CSV 中 `ts,camera_name,` 出现次数 == 1」翻红（实际为 2）。
// ⚠️ 前提依赖：本用例假定「被本进程打开的 `.1` 会令 remove/rename 失败」。若某环境允许（POSIX 语义
//   或 CRT 带 FILE_SHARE_DELETE），轮转会成功 ⇒ 前提不成立，此时**如实报前提不成立**（打说明 + return
//   false），不把它伪装成判据失败，也不声称「必然红在共享冲突上」。
bool test_structured_logger_rename_failure_does_not_reset_header() {
    TempLogDir tmp("structured_rename_fail");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path csv = dir / "recognition.csv";
    const std::filesystem::path gen1 = dir / "recognition.csv.1";

    rk_win::StructuredLogger log;
    if (!log.open(dir, 100, 2)) return false;

    // ① 阶段 A：正常写一条 ⇒ 当前 CSV 含恰一个表头，且已超限（下一条进入时必触发轮转）。
    const std::string pad(200, 'x');
    log.append(makeFrameEntry("cam", pad));
    const std::string afterA = readFile(csv);
    if (afterA.find("ts,camera_name,") == std::string::npos || sizeOf(csv) <= 100) {
        std::cerr << "[structured_rename_fail] 阶段 A 未写出表头或未超限，前提不成立" << std::endl;
        return false;
    }

    // ② 阶段 B：钉住 `.1`（先建文件、再持句柄不关），使轮转的删除/改名失败。
    writeFile(gen1, "OLD_GENERATION");
    std::ofstream lock(gen1, std::ios::out | std::ios::app);
    if (!lock.is_open()) {
        std::cerr << "[structured_rename_fail] 无法持有 .1 以制造改名失败，前提不成立" << std::endl;
        return false;
    }

    // ③ 阶段 C：再写一条 ⇒ 触发注定失败的轮转。
    rk_win::FrameLogEntry e = makeFrameEntry("cam", pad);
    e.tsIso8601 = "TS_MARKER";
    log.append(e);
    lock.close();

    const std::string afterC = readFile(csv);
    // 前提校验：若阶段 A 的内容已不在当前文件里，说明本环境**允许**被打开的 `.1` 被改名/删除
    // ⇒ 轮转真的成功了 ⇒ 本用例的前提不成立。
    if (afterC.find(pad) == std::string::npos) {
        std::cerr << "[structured_rename_fail] 本环境允许被打开的 .1 被改名/删除 ⇒ 轮转成功，前提不成立"
                  << std::endl;
        return false;
    }
    // 主判据：表头在**当前** CSV 中必须恰好出现一次。
    const std::size_t firstHdr = afterC.find("ts,camera_name,");
    const std::size_t secondHdr =
        (firstHdr == std::string::npos) ? std::string::npos : afterC.find("ts,camera_name,", firstHdr + 1);
    if (firstHdr == std::string::npos || secondHdr != std::string::npos) {
        std::cerr << "[structured_rename_fail] 当前 CSV 的表头行数不是 1（first=" << firstHdr
                  << ", second=" << secondHdr
                  << "）——改名失败被当成「已轮转」，headerWritten_ 被复位而写出了重复表头" << std::endl;
        return false;
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════
// C. RenderMetricsLogger 轮转（render_metrics.jsonl）
// ═════════════════════════════════════════════════════════════════════

// C1 轮转真正生效：同 B1（render_metrics.jsonl + .1/.2）。
// 判别点：删掉 append 里对 rollIfNeededLocked 的调用 ⇒ 无轮转 ⇒「.1 存在」与「尺寸 <= maxBytes +
//   单条」两条断言红。
// ★ 2026-09-28 新增（外部审查 [P2]「日志文件重开失败后不会自行恢复」的**判别用例**）。
// 缺陷（修复前，两处）：① 某路重开失败后**永久停写**（唯一恢复路径是进程重启 / 重新 initialize）；
//   ② `append` 用**联合**判据 `!csv_.is_open() || !jsonl_.is_open()` ⇒ **一路坏会把健康的那一路
//   也一起停写**，且全程无任何计数 / 告警信号。
// 修复：`append` 每次进入都对**已关闭的那一路**做限频（5000ms）重开尝试；两路各自独立；
//   失败次数经 `reopenFailCount()` 可观测（另在首失败时打一行 stderr，尽力而为）。
//
// 判别力（把 `append` 的联合判据改回去、或去掉 `ensureOpenLocked()` 调用 ⇒ **第 2 段必红**）：
//   第 2 段断言「CSV 被挡住期间 **jsonl 仍在增长**」—— 旧实现下 jsonl 会一起停写，故必红。
// 构造方式（**确定性**，不依赖文件锁 / 权限 / 杀软）：用**同名目录**占位 `recognition.csv`
//   ⇒ `ofstream::open` 必失败；`jsonl` 路径不同、不受影响。
bool test_structured_logger_reopen_recovers_and_paths_independent() {
    TempLogDir tmp("reopen_recover");
    const auto dir = tmp.path();
    const auto csvPath = dir / "recognition.csv";
    const auto jsonlPath = dir / "recognition.jsonl";

    // 必须先于 open() 用目录占位 —— 之后才建立就删不掉已打开的句柄了。
    std::error_code ec;
    if (!std::filesystem::create_directories(csvPath, ec)) return false;

    rk_win::StructuredLogger logger;
    // open() 会因 csv 打不开而返回 false —— 那是**预期状态**，不是用例失败；jsonl 应当已打开。
    (void)logger.open(dir, /*maxBytes=*/0, /*maxRollFiles=*/0);  // 关闭轮转，聚焦「重开」语义
    if (!pathExists(dir / "recognition.jsonl")) return false;    // 前置锚：jsonl 那一路必须已建立

    // 第 1 段（前置锚）：正常建立后 jsonl 能写，且 csv 的重开确实失败过。
    logger.append(makeFrameEntry("S1", "x"));
    const auto jsonl1 = sizeOf(jsonlPath);
    if (jsonl1 == 0) return false;
    if (logger.reopenFailCount() == 0) return false;  // 前置锚：csv 重开必须失败过一次

    // 第 2 段（★ 判别点）：CSV 仍被挡住，**jsonl 必须继续增长**。
    logger.append(makeFrameEntry("S2", "y"));
    const auto jsonl2 = sizeOf(jsonlPath);
    if (jsonl2 <= jsonl1) return false;               // 旧实现（联合判据）在此必红

    // 第 3 段：恢复 CSV 的可写性，等过限频窗口后应**自动重开**并续写（且表头恰好一行）。
    std::filesystem::remove_all(csvPath, ec);
    if (pathExists(csvPath)) return false;            // 目录必须真的被移除
    std::this_thread::sleep_for(std::chrono::milliseconds(5100));  // > kReopenRetryWindowMs(5000)
    logger.append(makeFrameEntry("S3", "z"));

    const std::string csv = readFile(csvPath);
    if (csv.empty()) return false;                    // 未自动恢复 ⇒ 红
    std::size_t hdrCount = 0;
    for (std::size_t pos = csv.find("ts,camera_name"); pos != std::string::npos;
         pos = csv.find("ts,camera_name", pos + 1)) {
        ++hdrCount;
    }
    if (hdrCount != 1) return false;                  // 表头**恰好一行**（恢复时不得重复写）
    return true;
}

// ★ 2026-09-29 新增：「每路**独立**重试窗口」的**判别用例**（S4 登记：此前零覆盖）。
//
// 缺陷形态（本批初版，已被改掉）：`ensureOpenLocked` 的两路**共用**一个 `nextAttempt` 成员
//   ⇒ 任一路失败都会把**另一路**（健康但已关闭）也挡回最多 5000ms，与「两路各自独立」的自述不符。
// 零覆盖的成因（S4 指出）：既有用例 test_structured_logger_reopen_recovers_and_paths_independent
//   只让 csv 一路失败，**健康的那一路**（jsonl）命中 `ensureOpenLocked` 的 `is_open() && good()`
//   早退、**根本不读窗口** ⇒ 共用窗口与独立窗口在该构造下读数完全相同，无从判别。
//
// 判别构造（**不等 5000ms 窗口过期、不依赖线程交错、不依赖文件锁/权限/杀软**；两段合起来覆盖两个方向）：
//   ① 正向（csv 失败不得挡 jsonl）：把 csv / jsonl **两路都用同名目录占位** ⇒ `open()` 对两路都失败、
//      两流均关闭、两窗口均空（`open()` 不设窗口）；随后**只移除 jsonl 的占位目录**，再 append 一次：
//        · 独立窗口：csv 尝试→失败（设 csv 窗口），jsonl 尝试→**成功**（jsonl 窗口仍空）⇒ jsonl 增长；
//        · 共用窗口：csv 尝试→失败（设**共用**窗口），jsonl 因 now < 共用窗口被挡⇒**不尝试**⇒ jsonl 不增长。
//   ② 反向（jsonl 失败不得挡 csv）：只把 jsonl 用目录占位（csv 可用）⇒ open() 后 csv 开、jsonl 关
//      （jsonl 窗口仍空）；append 一次让 jsonl 失败（设 jsonl 窗口，csv 正常写）；`close()` 关掉两路
//      （**不改窗口**）；移除 jsonl 占位；再 append 一次：
//        · 独立窗口：csv 窗口仍空 ⇒ csv **立即**重开续写（jsonl 被自身窗口挡住，与本断言无关）；
//        · 共用窗口：csv 被 jsonl 的窗口挡住、不尝试 ⇒ csv 不增长。
// ⚠️ 前提锚（各段 return false 处）不可省：必须确证「预期失败的那一路确实处于关闭态且尝试过」，
//    否则会退化成恒真断言。本用例刻意**不引入任何生产侧可注入面**——所需交错恰好落在同一次
//    / 相邻两次 append 内，无需等待窗口过期。
bool test_structured_logger_reopen_windows_are_per_path() {
    TempLogDir tmp("reopen_windows_per_path");
    const auto dir = tmp.path();
    const auto csvPath = dir / "recognition.csv";
    const auto jsonlPath = dir / "recognition.jsonl";
    std::error_code ec;
    bool ok = true;

    // ── 段①：csv 失败不得挡住 jsonl（正向）──
    if (!std::filesystem::create_directories(csvPath, ec)) return false;    // 占位：csv 必打不开
    if (!std::filesystem::create_directories(jsonlPath, ec)) return false;  // 占位：jsonl 必打不开
    {
        rk_win::StructuredLogger logger;
        // 前提锚：两路都打不开 ⇒ open 返回 false ⇒ 两流均关闭、两窗口均空（open 不设窗口）。
        if (logger.open(dir, /*maxBytes=*/0, /*maxRollFiles=*/0)) {
            std::cerr << "[reopen_windows_per_path] 段①前提不成立：两路同名目录占位下 open() 竟返回 true"
                      << std::endl;
            return false;
        }
        std::filesystem::remove_all(jsonlPath, ec);  // 只放开 jsonl（csv 仍被目录挡住）
        if (pathExists(jsonlPath)) return false;     // 占位必须真的移除

        logger.append(makeFrameEntry("FWD", "x"));
        // ★ 主判据：jsonl 必须已被**同一次 append** 重开并写入（csv 的失败窗口不得挡住它）。
        if (readFile(jsonlPath).empty()) {
            std::cerr << "[reopen_windows_per_path] 段①：csv 的重开失败挡住了 jsonl ——"
                         "两路疑似**共用**同一个重试窗口（独立窗口下 jsonl 应在同一次 append 内被重开）"
                      << std::endl;
            ok = false;  // 不短路：段②仍须执行，使两方向都可被观测
        }
    }
    std::filesystem::remove_all(csvPath, ec);    // 清理占位，供段②复用同一目录
    std::filesystem::remove_all(jsonlPath, ec);

    // ── 段②：jsonl 失败不得挡住 csv（反向）──
    if (!std::filesystem::create_directories(jsonlPath, ec)) return false;  // 只占位 jsonl
    {
        rk_win::StructuredLogger logger;
        // csv 可打开、jsonl 打不开（jsonl 窗口仍空）。
        (void)logger.open(dir, /*maxBytes=*/0, /*maxRollFiles=*/0);
        logger.append(makeFrameEntry("REV1", "x"));  // jsonl 尝试→失败（设 jsonl 窗口）；csv 正常写
        // 前提锚：jsonl 必须真的失败过一次（否则共用/独立窗口读数相同，无判别力）。
        if (logger.reopenFailCount() == 0) {
            std::cerr << "[reopen_windows_per_path] 段②前提不成立：jsonl 未发生重开失败（reopenFailCount==0）"
                      << std::endl;
            return false;
        }
        const auto csvSizeBefore = sizeOf(csvPath);
        if (csvSizeBefore == 0) {
            std::cerr << "[reopen_windows_per_path] 段②前提不成立：被占位期间的 csv 未写出任何内容"
                      << std::endl;
            return false;
        }

        logger.close();                              // 关掉两路（**不改窗口**：csv 窗口仍空、jsonl 窗口仍在前 5s 内）
        std::filesystem::remove_all(jsonlPath, ec);  // 放开 jsonl（本段只观察 csv 是否被挡）

        logger.append(makeFrameEntry("REV2", "y"));
        // ★ 主判据：csv 必须已被**同一次 append** 重开并续写（jsonl 的失败窗口不得挡住它）。
        if (sizeOf(csvPath) <= csvSizeBefore) {
            std::cerr << "[reopen_windows_per_path] 段②：jsonl 的重开失败挡住了 csv ——"
                         "两路疑似**共用**同一个重试窗口（独立窗口下 csv 应在同一次 append 内被重开）"
                      << std::endl;
            ok = false;
        }
    }
    return ok;
}

bool test_render_metrics_logger_rotates_and_caps_generations() {
    TempLogDir tmp("render_rotate");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path jsonl = dir / "render_metrics.jsonl";
    const std::filesystem::path gen1 = dir / "render_metrics.jsonl.1";
    const std::filesystem::path gen2 = dir / "render_metrics.jsonl.2";
    const std::filesystem::path gen3 = dir / "render_metrics.jsonl.3";

    const std::uint64_t maxBytes = 100;  // 故意小于单条记录
    const int maxRollFiles = 2;
    rk_win::RenderMetricsLogger m;
    if (!m.open(dir, maxBytes, maxRollFiles)) return false;

    rk_win::RenderMetricsSample s;
    s.tsIso8601 = std::string(200, 'x');  // 撑大单条记录
    m.append(s);
    const std::uintmax_t singleLine = sizeOf(jsonl);  // 首条后尚未轮转 ⇒ 即单条记录的字节数
    // 前置假设：单条须 > maxBytes（每次写入都触发轮转）。为 0 还说明 append 未 flush（尺寸判据靠已
    // 落盘字节，未 flush 的缓冲对 file_size 不可见）——与另外两个日志器「每次写入后 flush」语义不一致。
    if (singleLine == 0 || singleLine * 19 <= 2 * maxBytes) {
        std::cerr << "[render_rotate] 记录长度 " << singleLine << " 与 maxBytes=" << maxBytes
                  << " 不匹配——用例前置假设不成立（单条须 > maxBytes 且 append 须落盘）" << std::endl;
        return false;
    }
    for (int i = 0; i < 19; ++i) {  // 合计 20 条
        m.append(s);
    }

    if (!pathExists(jsonl) || sizeOf(jsonl) == 0) {
        std::cerr << "[render_rotate] 当前 render_metrics.jsonl 缺失或为空——写句柄可能跟随了被改名的旧文件"
                  << std::endl;
        return false;
    }
    if (!pathExists(gen1) || sizeOf(gen1) == 0) {
        std::cerr << "[render_rotate] 未发生轮转：render_metrics.jsonl.1 缺失或为空" << std::endl;
        return false;
    }
    if (!pathExists(gen2)) {
        std::cerr << "[render_rotate] 代数未推进到 .2（maxRollFiles=" << maxRollFiles << "）" << std::endl;
        return false;
    }
    if (pathExists(gen3)) {
        std::cerr << "[render_rotate] 代数越界：出现 render_metrics.jsonl.3，超过 maxRollFiles="
                  << maxRollFiles << std::endl;
        return false;
    }
    if (sizeOf(jsonl) > maxBytes + singleLine) {
        std::cerr << "[render_rotate] 当前 render_metrics.jsonl " << sizeOf(jsonl) << " 字节，超过上界 "
                  << (maxBytes + singleLine) << "——轮转未生效" << std::endl;
        return false;
    }
    return true;
}

// C2 maxBytes == 0 ⇒ 不轮转、一条不丢（口径同 B2）。
// 判别点：删掉 rollIfNeededLocked 里 `maxBytes_ == 0` 的判断 ⇒ 出现 .1 ⇒ 红。
bool test_render_metrics_logger_no_rotation_when_max_bytes_zero() {
    TempLogDir tmp("render_norotate");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path jsonl = dir / "render_metrics.jsonl";

    rk_win::RenderMetricsLogger m;
    if (!m.open(dir, 0, 5)) return false;

    rk_win::RenderMetricsSample s;
    s.tsIso8601 = std::string(200, 'x');
    m.append(s);
    const std::uintmax_t singleLine = sizeOf(jsonl);
    if (singleLine == 0) {
        std::cerr << "[render_norotate] 首条写入后 render_metrics.jsonl 为空——append 未落盘" << std::endl;
        return false;
    }
    for (int i = 0; i < 19; ++i) {  // 合计 20 条
        m.append(s);
    }

    if (pathExists(dir / "render_metrics.jsonl.1")) {
        std::cerr << "[render_norotate] maxBytes=0 仍发生轮转（出现了 render_metrics.jsonl.1）" << std::endl;
        return false;
    }
    if (sizeOf(jsonl) != singleLine * 20) {
        std::cerr << "[render_norotate] render_metrics.jsonl " << sizeOf(jsonl) << " 字节，期望 "
                  << (singleLine * 20) << "（不轮转路径不得丢记录/不得截断）" << std::endl;
        return false;
    }
    return true;
}

// C3 open 默认实参（仅传 dir）仍能工作（源兼容）：maxBytes / maxRollFiles 缺省时 open(dir) 须可编译、
// 返回 true、且能写入。
// 判别点：把 open 的 maxBytes / maxRollFiles 默认实参删掉 ⇒ 本用例**编译期**即红（找不到 1 参重载）；
//   若默认实参存在但内部未接线（0 值误传致轮转/写入异常），append 后文件仍为空 ⇒ 运行期红。
bool test_render_metrics_logger_open_default_arguments() {
    TempLogDir tmp("render_default_args");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path jsonl = dir / "render_metrics.jsonl";

    rk_win::RenderMetricsLogger m;
    if (!m.open(dir)) {  // 仅传 dir —— 依赖 maxBytes / maxRollFiles 的默认实参（源兼容）
        std::cerr << "[render_default_args] open(dir) 失败——默认实参缺失或文件无法创建" << std::endl;
        return false;
    }
    if (!pathExists(jsonl)) {
        std::cerr << "[render_default_args] open(dir) 后 render_metrics.jsonl 不存在" << std::endl;
        return false;
    }
    rk_win::RenderMetricsSample s;
    s.tsIso8601 = "2026-01-01T00:00:00.000";
    m.append(s);
    if (sizeOf(jsonl) == 0) {
        std::cerr << "[render_default_args] append 后 render_metrics.jsonl 仍为空" << std::endl;
        return false;
    }
    return true;
}

// C4 冷启动即判一次尺寸（不经 append 触发轮转——可预置超限文件 + open 触发冷启动轮转）。
// 采用本形态而非 append 的原因：本类亦可经 append 测轮转（见 C1，RenderMetricsSample 可默认构造），
//   但冷启动这一条只有 open() 里的那一行能保证，故单独补测。
// 判别点：删掉 open() 里对 rollIfNeededLocked 的调用 ⇒ 冷启动不轮转 ⇒ .1 缺失、当前文件仍超限 ⇒ 红。
bool test_render_metrics_logger_open_rolls_oversized_cold_start() {
    TempLogDir tmp("render_coldstart");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path jsonl = dir / "render_metrics.jsonl";
    if (!writeFile(jsonl, std::string(500, 'j'))) return false;  // 直接写文件造出超限当前文件

    rk_win::RenderMetricsLogger m;
    if (!m.open(dir, 100, 2)) return false;  // 只 open，不 append

    const std::filesystem::path gen1 = dir / "render_metrics.jsonl.1";
    if (!pathExists(gen1) || sizeOf(gen1) == 0) {
        std::cerr << "[render_coldstart] 冷启动未轮转：render_metrics.jsonl.1 缺失或为空" << std::endl;
        return false;
    }
    if (sizeOf(jsonl) != 0) {
        std::cerr << "[render_coldstart] 冷启动轮转后当前 render_metrics.jsonl 应为空，实际 "
                  << sizeOf(jsonl) << " 字节" << std::endl;
        return false;
    }
    return true;
}
