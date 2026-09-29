#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include "Compat.h"
#include <fstream>
#include <mutex>
#if __has_include(<optional>)
#include <optional>
#endif
#if !defined(__cpp_lib_optional)
namespace std {
template <class T>
class optional;
}
#endif
#include <string>
#include <vector>

namespace rk_win {

enum class ErrorCategory {
    None = 0,
    PrivacyDenied,
    DeviceNotFound,
    DeviceBusy,
    FormatNotSupported,
    BackendFailure,
    Unknown,
};

struct FaceLogEntry {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::string personId;
    double distance = 0.0;
    double confidence = 0.0;
};

struct FrameLogEntry {
    std::string tsIso8601;
    std::string cameraName;
    std::string cameraId;
    std::uint64_t frameIndex = 0;
    int frameWidth = 0;
    int frameHeight = 0;
    double fps = 0.0;
    std::vector<FaceLogEntry> faces;
    ErrorCategory errorCategory = ErrorCategory::None;
    std::string errorCode;
    std::string errorMessage;
};

// 结构化帧日志：CSV（`recognition.csv`）+ JSONL（`recognition.jsonl`），各由**独立的 ofstream**
// 轮转，命名 `<name>.1` … `<name>.N`、**最新在 `.1`**，与 EventLogger 共用同一套运维习惯。
// 轮转范式为「**flush → close → 代数迁移 → rename → 以 app 重开**」：Windows 上对仍被本进程打开的
// 文件改名会以共享冲突失败，只 rename 不 close 会让轮转静默失效（与 `EventLogger` 同范式，理由见
// `EventLogger.h` 的类级注释）。
// 本类的**代数收缩清理**（`maxRollFiles` 由大调小后删除超出的既有档位）固定放在 `open()` 内，
// 实现见 `LogRotation.h`；`EventLogger` 侧同形态清理亦已在其 `open()` 中落地。
// `maxBytes == 0` / `maxRollFiles <= 0` 仍表示**关闭轮转**（日志无界增长），与 `EventLogger` 语义一致。
class StructuredLogger {
public:
    StructuredLogger() = default;

    bool open(const std::filesystem::path& dir, std::uint64_t maxBytes, int maxRollFiles);
    void close();

    void append(const FrameLogEntry& e);

    // ★ 2026-09-28 新增：让「日志已停写」**可被程序观测**（此前失败完全无信号，见 .cpp 内注释）。
    //   返回值 = 累计「重开尝试失败」次数（csv / jsonl **两路合计**）。>0 表示至少有一路曾重开失败；
    //   此时该路暂时停写，但会按时间窗限频**自动重试**，目录恢复后自行续写（无需重启进程）。
    //   ⚠️ 已知未做（如实登记，勿当已完成）：**尚未**把它接进 `FramePipeline` 的 `render_.status`
    //   （需跨层管道，属后续）；目前的可观测面 = 首失败时的一行 stderr + 本计数。
    std::size_t reopenFailCount() const;

    // ★ 2026-09-28 新增（对抗性代理 A2 复核后补）：`reopenFailCount()` 只覆盖「重开失败」，
    //    **覆盖不到「流开着、但写入失败」**（典型：写盘中途磁盘满/配额用尽）—— 那条路径上
    //    `ofstream` 只置 badbit、`is_open()` 仍为 true，`ensureOpenLocked` 对已打开的流只能
    //    `clear()`、无法预判写入成败（见其内注释），于是初版会出现「每帧静默丢日志、计数恒 0」。
    //    返回值 = 累计「写入失败」次数（csv / jsonl **两路合计**），配合首失败的一行 stderr
    //    使该失效模式可观测。两路各自的「已告警」标志在恢复（再次 good()）时复位。
    //   ⚠️ 与 `reopenFailCount()` 一样，**尚未**接入 `render_.status`（如实登记，勿当已完成）。
    std::size_t writeFailCount() const;

private:
    // 确保 `stream` 处于打开态；未打开则按时间窗限频尝试重开。返回是否已打开。调用方须已持 `mu_`。
    // ★ 为什么需要它（2026-09-28 修，外部审查 [P2]「日志文件重开失败后不会自行恢复」）：
    //   ① 轮转末尾的重开若失败，该路会**永久停写**，而原实现没有任何路径能把它重新打开
    //      （唯一恢复途径是进程重启 / 重新 initialize）；
    //   ② `append` 原用**联合**判据（`!csv_.is_open() || !jsonl_.is_open()`）⇒ 一路坏会把
    //      **健康的那一路**也一起停写。
    //   ⚠️ 重试**必须**落在本函数、而**不能**寄望 `rollIfNeededLocked`：后者在「尺寸未超限」时
    //      会**直接 return、根本不打开流**（见其内注释），⇒ 只把联合判据拆成两路，会「看着修好了、
    //      实际仍每帧静默」，而且没有用例会红（缺的正是那条用例）。这是本修复最容易踩空的一点。
    //   ⚠️ `nextAttempt` 是**调用方持有的每路独立窗口**（csv / jsonl 各一个成员）：本批初版把它
    //      写成「两路共用的单个成员」，于是先失败的那一路会把**健康但已关闭**的另一路也挡回最多 5s
    //      —— 与「两路各自独立」的自述不符（验收代理指出）。改为按路传入。
    bool ensureOpenLocked(const std::filesystem::path& path, std::ofstream& stream,
                          bool& warnedFlag, std::chrono::steady_clock::time_point& nextAttempt,
                          const char* which);

    // 到达上限即轮转。**先 close、再改名、再以 app 重开**——理由见 .cpp 内注释（Windows 上对仍被
    // 本进程打开的文件改名会以共享冲突失败，只 rename 不 close 的写法会让轮转静默失效）。
    // `path` 与其对应的 `stream` 成对传入（csv / jsonl 是两个独立句柄）。调用方须已持 `mu_`。
    // 代数收缩清理不在此处，而在 `open()`（见 .cpp 与 `LogRotation.h`）。
    // **返回值 = 「`path` 指向的当前文件是否**真的**被轮转走」**，三种情形为 false：
    // ① 关闭轮转（`maxBytes_ == 0` / `maxRollFiles_ <= 0`）；② 尺寸未超限；③ **超限了但改名失败**
    // （`.1` 被占用、源文件被 AV/文本工具占用等）——此时当前文件仍带旧内容。
    // 调用方必须用它处理轮转的副作用：当前文件被改名走、重开成空文件 ⇒ 落在该文件上的「已写过」
    // 状态随之失效。CSV 的 `headerWritten_` 正是此类状态（见 `append`）：不复位会让轮转后的新文件
    // **永久缺表头**，而**无条件复位**（把 ③ 也当成功）会在改名失败时往同一文件写出**第二行表头**。
    // 两个方向都是缺陷，故返回值必须如实区分 ③。
    bool rollIfNeededLocked(const std::filesystem::path& path, std::ofstream& stream);

    // ★ 2026-09-28 新增：**写入失败**的可观测面（与 `reopenFailCount_` 正交，见上）。调用方须已持 `mu_`。
    //   在写入 + `flush()` 之后调用；`stream.good()` 时复位该路告警位（恢复后仍能再报一次）。
    void noteWriteFailureLocked(std::ofstream& stream, bool& warnedFlag,
                                const std::filesystem::path& path, const char* which);
    static std::string escapeCsv(const std::string& s);

    // `mutable`：使 `reopenFailCount()` 这个 **const 只读接口**也能加锁取计数（与
    // `WinJsonConfig.h` 的 `mutable std::mutex mu_` 同一写法）。仅影响 const 方法能否加锁，
    // 不改变任何既有语义。
    mutable std::mutex mu_;
    std::filesystem::path dir_;
    std::filesystem::path csvPath_;
    std::filesystem::path jsonlPath_;
    std::uint64_t maxBytes_ = 0;
    int maxRollFiles_ = 0;
    std::ofstream csv_;
    std::ofstream jsonl_;
    bool headerWritten_ = false;
    // ★ 2026-09-28 新增：重开失败的可观测面 + 限频。
    //   限频理由（与仓内既有口径一致）：`append` 在**逐帧**路径上，目录长期不可用时会变成每帧一次
    //   失败 `open()`。仓内成文约定是「**失败的代价要限定在失败态、不得变成每帧无谓 I/O**」
    //   （见 `LogRateLimiter.h` 与各逐帧路径的「只记一次」写法）。
    std::size_t reopenFailCount_ = 0;
    bool csvOpenWarned_ = false;
    bool jsonlOpenWarned_ = false;
    // ★ 2026-09-28 新增：**写入**失败的计数与每路告警位（与上面的「重开失败」正交）。
    //   每路一枚告警位，理由同 `nextOpenAttempt*`：共用会让一路的写入失败压掉另一路的告警。
    std::size_t writeFailCount_ = 0;
    bool csvWriteWarned_ = false;
    bool jsonlWriteWarned_ = false;
    // 每路**独立**的重试窗口（下次允许尝试重开的时间点；`{}` = 立即可试）。
    // ⚠️ 刻意不共用一个：共用会让一路的失败窗口把另一路也挡住（见 ensureOpenLocked 的说明）。
    std::chrono::steady_clock::time_point nextOpenAttemptCsv_{};
    std::chrono::steady_clock::time_point nextOpenAttemptJsonl_{};
};

std::string nowIso8601Local();
std::string toString(ErrorCategory c);

}  // namespace rk_win

