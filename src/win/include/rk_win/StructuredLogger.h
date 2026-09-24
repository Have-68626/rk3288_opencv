#pragma once

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

private:
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
    static std::string escapeCsv(const std::string& s);

    std::mutex mu_;
    std::filesystem::path dir_;
    std::filesystem::path csvPath_;
    std::filesystem::path jsonlPath_;
    std::uint64_t maxBytes_ = 0;
    int maxRollFiles_ = 0;
    std::ofstream csv_;
    std::ofstream jsonl_;
    bool headerWritten_ = false;
};

std::string nowIso8601Local();
std::string toString(ErrorCategory c);

}  // namespace rk_win

