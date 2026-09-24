#include "rk_win/StructuredLogger.h"

#include "rk_win/LogRotation.h"
#include "rk_win/WinStringUtil.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace rk_win {
namespace {

std::uint64_t fileSizeOr0(const std::filesystem::path& p) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(sz);
}

// 先删目标再改名（Windows 上 rename 不覆盖已存在目标）。返回**改名是否成功**。
// ⚠️ 返回值不是装饰：`rollIfNeededLocked` 用它回答「当前文件**是否真的**被轮转走」，
//   而那个答案决定调用方要不要复位 `headerWritten_`。若失败时仍回 true，调用方会以为当前文件
//   已清空，于是往**仍带旧表头**的同一文件里再写一行表头（CSV 中段出现第二行表头）。**吞错的代价
//   必须由返回值如实上报**，不能只靠「轮转是尽力而为」这句话带过。
bool renameBestEffort(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::remove(to, ec);
    ec.clear();
    std::filesystem::rename(from, to, ec);
    return !ec;
}

}  // namespace

std::string nowIso8601Local() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    char buf[64];
    std::size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    if (n > 0) {
        char msBuf[8];
        std::snprintf(msBuf, sizeof(msBuf), ".%03d", static_cast<int>(ms));
        std::string s(buf, n);
        s += msBuf;
        return s;
    }
    return "";
}

std::string toString(ErrorCategory c) {
    switch (c) {
        case ErrorCategory::None:
            return "none";
        case ErrorCategory::PrivacyDenied:
            return "privacy_denied";
        case ErrorCategory::DeviceNotFound:
            return "device_not_found";
        case ErrorCategory::DeviceBusy:
            return "device_busy";
        case ErrorCategory::FormatNotSupported:
            return "format_not_supported";
        case ErrorCategory::BackendFailure:
            return "backend_failure";
        case ErrorCategory::Unknown:
        default:
            return "unknown";
    }
}

bool StructuredLogger::open(const std::filesystem::path& dir, std::uint64_t maxBytes, int maxRollFiles) {
    std::lock_guard<std::mutex> lock(mu_);
    dir_ = dir;
    maxBytes_ = maxBytes;
    maxRollFiles_ = maxRollFiles;

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);

    csvPath_ = dir_ / "recognition.csv";
    jsonlPath_ = dir_ / "recognition.jsonl";

    // 幂等：重复 open() 必须先断开旧句柄（close 顺带把缓冲刷盘），否则旧句柄会让后续轮转的改名
    // 吃共享冲突（同 EventLogger::open）。
    if (csv_.is_open()) csv_.close();
    if (jsonl_.is_open()) jsonl_.close();

    // 代数收缩清理：`maxRollFiles` 由大调小后，索引超过新值的既有档位再也不会被任何一次轮转触及，
    // 故在 open 时扫一次目录删除之。刻意只在 open 做——`maxRollFiles_` 在一次 open 内是常量，收缩
    // 只可能发生在两次 open 之间，每次轮转都扫目录纯属浪费（推导见 LogRotation.h）。
    pruneShrunkGenerations(csvPath_, maxRollFiles_);
    pruneShrunkGenerations(jsonlPath_, maxRollFiles_);

    // 冷启动即判一次：否则「上次退出时已超限」的文件要等到首次 append 才轮转（尺寸判据滞后一轮）。
    // 此处两个句柄均未打开 ⇒ 轮转内的改名不会吃共享冲突（见 rollIfNeededLocked）。
    // 返回值（是否发生轮转）**刻意忽略**：紧随其后就按文件大小重算 headerWritten_，无需另行复位。
    rollIfNeededLocked(csvPath_, csv_);
    rollIfNeededLocked(jsonlPath_, jsonl_);

    if (!csv_.is_open()) csv_.open(csvPath_, std::ios::out | std::ios::app);
    if (!jsonl_.is_open()) jsonl_.open(jsonlPath_, std::ios::out | std::ios::app);

    // ⚠️ headerWritten_ 必须在轮转**之后**再算：冷启动轮转会把当前文件改名走、重开成新空文件，
    //    此时表头必须重写（这正是 CSV 表头不能丢的原因）。若在轮转前计算，恢复出来的会是一个
    //    「有表头判定但文件已空」的错状态。
    headerWritten_ = (fileSizeOr0(csvPath_) > 0);
    return csv_.is_open() && jsonl_.is_open();
}

void StructuredLogger::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (csv_.is_open()) csv_.close();
    if (jsonl_.is_open()) jsonl_.close();
}

std::string StructuredLogger::escapeCsv(const std::string& s) {
    bool needQuote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) return s;
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool StructuredLogger::rollIfNeededLocked(const std::filesystem::path& path, std::ofstream& stream) {
    // 轮转关闭时（`maxBytes_ == 0` 或 `maxRollFiles_ <= 0`，语义与头文件声明一致）直接返回，
    // 不改变任何打开状态。
    if (maxBytes_ == 0 || maxRollFiles_ <= 0) return false;

    // ★ 尺寸判据必须建立在**已落盘**的字节上：ofstream 有用户态缓冲，未 flush 的字节对
    //   std::filesystem::file_size 不可见 ⇒ 不先 flush 会让判据长期偏小、轮转被静默推迟甚至永不触发。
    if (stream.is_open()) stream.flush();
    if (fileSizeOr0(path) <= maxBytes_) return false;

    // ★★ 必须按「close → 轮转 → 以 app 重开」执行，不能只 rename（与 EventLogger::rollIfNeededLocked
    //   同范式）：Windows 上 std::ofstream 打开的文件默认不带 FILE_SHARE_DELETE，对**自己仍打开**的
    //   文件改名通常以 ERROR_SHARING_VIOLATION 失败，而 renameBestEffort 会把该错误吞掉 ⇒ 轮转静默
    //   失效、recognition.csv / recognition.jsonl 无限增长。即便改名侥幸成功，写句柄仍指向被改名的旧
    //   文件 ⇒ 之后所有日志写进 `.1`，当前文件永远空着。
    if (stream.is_open()) stream.close();

    // 代数收缩清理**不在这里做**：`maxRollFiles_` 在一次 open 内是常量，收缩只可能发生在两次 open
    // 之间，在每次轮转里扫目录是浪费——清理固定放在 open() 内（见 LogRotation.h 的推导）。
    //
    // 代数迁移：`path.(i)` → `path.(i+1)`，最后 `path` → `path.1`（最新在 `.1`，与 EventLogger 一致）。
    // 只迁移**存在**的档位。代数上限由**循环起点** `maxRollFiles_ - 1` 决定：目标最高只写到
    // `path.maxRollFiles_`，从不产生 `.maxRollFiles_ + 1`（原有的 `.maxRollFiles_` 恰在
    // i = maxRollFiles_ - 1 这一步被 renameBestEffort 的「先删目标」覆盖丢弃）。
    // ⚠️ 迁移各步的成败**不影响**本函数的返回值语义（返回值只说「**当前**文件是否被轮转走」）；
    //   某一步失败造成的后果是「少了一代历史数据」，属 renameBestEffort 的既有取舍，另行登记。
    for (int i = maxRollFiles_ - 1; i >= 1; --i) {
        const std::filesystem::path older = path.string() + "." + std::to_string(i);
        const std::filesystem::path newer = path.string() + "." + std::to_string(i + 1);
        if (std::filesystem::exists(older)) {
            renameBestEffort(older, newer);
        }
    }
    // ★ 只有这一步**成功**，当前文件才算真的被轮转走（内容已挪到 `.1`、`path` 即将被重开成空文件）。
    //   失败（`.1` 被别的进程独占 / 源文件被 AV 或文本工具占用 —— Windows 上在轮转点很常见）时
    //   `path` **仍带着旧表头与旧数据**，此时若上报「已轮转」，调用方会复位 `headerWritten_` 而写出
    //   第二行表头。故此处如实上报成败。
    const bool rotatedAway = renameBestEffort(path, path.string() + ".1");

    // 轮转后立即重开：后续 append 继续写当前文件。重开失败（目录被删/无权限）时流保持关闭，
    // append 会因 `!csv_.is_open()` / `!jsonl_.is_open()` 而安全跳过，不写坏任何文件。
    // ⚠️ 无论改名成败都要重开——否则 append 会因流已关闭而永久停写。
    //   ⚠️ 既有残留（本批登记，未修）：重开失败会让**该路日志永久静默到进程重启**（连健康的那一路
    //   也会因 append 的联合检查而一起停写），且没有任何计数/告警信号。此前不存在这种持久态
    //   （旧实现的 rollIfNeededLocked 从不 close 自己的流）。修它需要日志器持有可上报的通道。
    stream.open(path, std::ios::out | std::ios::app);
    // 返回「**当前文件是否真的被轮转走**」：调用方据此处理轮转带来的副作用（CSV 的表头状态，
    // 见 append 内的 headerWritten_ 复位）。返回 void 会让那个副作用无处安放；无条件返回 true
    // 则会在改名失败时写出重复表头。
    return rotatedAway;
}

void StructuredLogger::append(const FrameLogEntry& e) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!csv_.is_open() || !jsonl_.is_open()) return;

    // CSV 轮转会把当前文件改名走、重开成**空**文件 ⇒ 先前写过的表头随之失效，必须复位。
    // ⚠️ 不复位会让轮转后的新 `recognition.csv` **永久缺表头**。这条在本批之前不可达：那时轮转
    //    因「只 rename 不 close」在 Windows 上静默失效、文件从不改名，故表头一直留着。是「让轮转
    //    真正生效」把这条缺口暴露出来的（第 2 个副作用点见下方 is_open 复检）。
    // JSONL 没有表头概念，无需对应处理。
    if (rollIfNeededLocked(csvPath_, csv_)) headerWritten_ = false;
    rollIfNeededLocked(jsonlPath_, jsonl_);
    // 轮转可能在重开失败后把对应流关掉（见 rollIfNeededLocked 末尾）：此处必须**再查一次**
    // is_open()，否则会往已关闭的流里写、静默丢日志；函数开头的检查只覆盖进入时的状态。
    if (!csv_.is_open() || !jsonl_.is_open()) return;

    if (!headerWritten_) {
        csv_ << "ts,camera_name,camera_id,frame_index,frame_w,frame_h,fps,face_count,faces_json,error_category,error_code,error_message\n";
        headerWritten_ = true;
    }

    /*
     * [Performance Optimization - string formatting]
     * Why: Replace std::ostringstream with std::string concatenation to avoid virtual calls and locale overhead per frame log.
     * Impact: Lower CPU usage during JSON logging in the processing loop.
     * Rollback: Revert back to using std::ostringstream.
     */
    std::string facesJson;
    facesJson.reserve(2 + e.faces.size() * 128);
    facesJson += "[";
    for (size_t i = 0; i < e.faces.size(); i++) {
        if (i) facesJson += ",";
        const auto& f = e.faces[i];
        facesJson += "{";
        facesJson += "\"x\":";
        facesJson += std::to_string(f.x);
        facesJson += ",\"y\":";
        facesJson += std::to_string(f.y);
        facesJson += ",\"w\":";
        facesJson += std::to_string(f.w);
        facesJson += ",\"h\":";
        facesJson += std::to_string(f.h);
        facesJson += ",\"person_id\":\"";
        facesJson += escapeJson(f.personId);
        facesJson += "\",\"distance\":";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", f.distance);
        facesJson += buf;
        facesJson += ",\"confidence\":";
        std::snprintf(buf, sizeof(buf), "%g", f.confidence);
        facesJson += buf;
        facesJson += "}";
    }
    facesJson += "]";

    csv_ << escapeCsv(e.tsIso8601) << "," << escapeCsv(e.cameraName) << "," << escapeCsv(e.cameraId) << ","
         << e.frameIndex << "," << e.frameWidth << "," << e.frameHeight << "," << e.fps << ","
         << static_cast<int>(e.faces.size()) << "," << escapeCsv(facesJson) << ","
         << escapeCsv(toString(e.errorCategory)) << "," << escapeCsv(e.errorCode) << "," << escapeCsv(e.errorMessage) << "\n";

    jsonl_ << "{"
           << "\"ts\":\"" << escapeJson(e.tsIso8601) << "\","
           << "\"camera_name\":\"" << escapeJson(e.cameraName) << "\","
           << "\"camera_id\":\"" << escapeJson(e.cameraId) << "\","
           << "\"frame_index\":" << e.frameIndex << ","
           << "\"frame_w\":" << e.frameWidth << ","
           << "\"frame_h\":" << e.frameHeight << ","
           << "\"fps\":" << e.fps << ","
           << "\"face_count\":" << static_cast<int>(e.faces.size()) << ","
           << "\"faces\":" << facesJson << ","
           << "\"error_category\":\"" << escapeJson(toString(e.errorCategory)) << "\","
           << "\"error_code\":\"" << escapeJson(e.errorCode) << "\","
           << "\"error_message\":\"" << escapeJson(e.errorMessage) << "\""
            << "}\n";
    csv_.flush();
    jsonl_.flush();
}

}  // namespace rk_win

