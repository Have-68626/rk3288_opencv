#include "rk_win/EventLogger.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

// 每个用例一个独立的**临时目录**：`std::filesystem::temp_directory_path()` 之下，绝不写 cwd
// （刻意不用 cwd / 相对路径开日志：那会把 `event_log.jsonl` 及其轮转档位落在运行目录里，
//   污染工作树与后续运行；同一子目录名重复使用，故构造时先 remove_all 清残留）。
class TempLogDir {
public:
    explicit TempLogDir(const std::string& tag) {
        std::error_code ec;
        path_ = std::filesystem::temp_directory_path(ec) / ("rk3288_opencv_event_logger_" + tag);
        std::filesystem::remove_all(path_, ec);  // 清掉上次运行残留（含 .1/.2），保证判据不被污染
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

// ⚠️ 刻意不取名 exists()：形参是 std::filesystem::path，非限定调用 exists(p) 会因 ADL 把
// std::filesystem::exists 一并拉进重载集 ⇒ C2668 调用不明确（本批实测踩到过）。
bool pathExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

}  // namespace

// FACE-LOG-001：事件日志必须轮转，且代数有上限（最新在 `.1`，与 StructuredLogger 命名一致）。
// 判别点（每条都能单独打红）：
//   1) 删掉 append/open 里对 rollIfNeededLocked 的调用 ⇒ 单文件无界增长 ⇒ 「当前文件尺寸」断言红；
//   2) 把实现改回 StructuredLogger 的「只 rename、不 close」形态（去掉 out_.close()）⇒
//      Windows 上对自己的打开句柄改名通常以 ERROR_SHARING_VIOLATION 失败，而 renameBestEffort 吞错
//      ⇒ 文件同样无界增长 ⇒ 同一尺寸断言红；**若某些环境/实现下改名恰好成功**（例如 CRT 以
//      FILE_SHARE_DELETE 打开，或 POSIX 语义），则当前文件被改名走、写句柄仍跟随旧文件
//      ⇒ 「当前文件存在且非空」断言红。两种环境行为下都能红，但**红的断言未必是同一条**——
//      此处如实登记该不确定性，不宣称「必然红在共享冲突上」。
bool test_event_logger_rotates_keeps_generations_capped() {
    TempLogDir tmp("rotate");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path cur = dir / "event_log.jsonl";
    const std::filesystem::path gen1 = dir / "event_log.jsonl.1";
    const std::filesystem::path gen2 = dir / "event_log.jsonl.2";
    const std::filesystem::path gen3 = dir / "event_log.jsonl.3";

    const std::uint64_t maxBytes = 100;  // 故意小于单条记录 ⇒ 首条之后每次写入都触发轮转
    const int maxRollFiles = 2;
    rk_win::EventLogger log;
    if (!log.open(dir, maxBytes, maxRollFiles)) return false;

    const std::string message(200, 'x');  // 单行约 255 字节
    log.append("io", message);
    const std::uintmax_t singleLine = sizeOf(cur);  // 首条写入后尚未轮转 ⇒ 即「一条记录」的字节数
    // 前置假设：20 条足以跨越至少两次轮转（否则 .2 的代数断言不成立，属用例前提失效而非实现缺陷）。
    if (singleLine == 0 || singleLine * 19 <= 2 * maxBytes) {
        std::cerr << "[event_logger_rotation] 记录长度 " << singleLine << " 与 maxBytes=" << maxBytes
                  << " 不匹配——用例前置假设不成立" << std::endl;
        return false;
    }

    for (int i = 0; i < 19; ++i) {  // 合计 20 条
        log.append("io", message);
    }

    // 写句柄必须仍指向**当前**文件（「只 rename 不 close」的形态会在此红）。
    if (!pathExists(cur) || sizeOf(cur) == 0) {
        std::cerr << "[event_logger_rotation] 当前文件缺失或为空——写句柄可能跟随了已被改名的旧文件"
                  << std::endl;
        return false;
    }
    if (!pathExists(gen1) || sizeOf(gen1) == 0) {
        std::cerr << "[event_logger_rotation] 未发生轮转：event_log.jsonl.1 缺失或为空" << std::endl;
        return false;
    }
    if (!pathExists(gen2)) {
        std::cerr << "[event_logger_rotation] 代数未推进到 .2（maxRollFiles=" << maxRollFiles << "）"
                  << std::endl;
        return false;
    }
    if (pathExists(gen3)) {
        std::cerr << "[event_logger_rotation] 代数越界：出现 event_log.jsonl.3，超过 maxRollFiles="
                  << maxRollFiles << std::endl;
        return false;
    }
    // 轮转真正封顶：当前文件最大只可能是「上次未超限的尺寸(<= maxBytes) + 一条记录」。
    // 无轮转时此处为 20 * singleLine（约 5100），断言必然红。
    if (sizeOf(cur) > maxBytes + singleLine) {
        std::cerr << "[event_logger_rotation] 当前文件 " << sizeOf(cur)
                  << " 字节，超过上界 " << (maxBytes + singleLine) << "——轮转未生效" << std::endl;
        return false;
    }
    return true;
}

// FACE-LOG-001 的关闭面：maxBytes == 0 时不轮转（语义与 StructuredLogger 一致，0 = 不限）。
// 判别点：把 `maxBytes_ == 0` 的判断从 rollIfNeededLocked 删掉 ⇒ 本用例红（出现 .1）。
// 同时断言总字节 == 20 * 单条长度，证明「不轮转」路径下一条事件都没丢。
// （maxRollFiles <= 0 走同一分支，见实现里的 `|| maxRollFiles_ <= 0`。）
bool test_event_logger_rotation_disabled_when_max_bytes_zero() {
    TempLogDir tmp("norotate");
    const std::filesystem::path dir = tmp.path();
    const std::filesystem::path cur = dir / "event_log.jsonl";
    const std::filesystem::path gen1 = dir / "event_log.jsonl.1";

    rk_win::EventLogger log;
    if (!log.open(dir, 0, 5)) return false;

    const std::string message(200, 'x');
    log.append("io", message);
    const std::uintmax_t singleLine = sizeOf(cur);
    if (singleLine == 0) return false;

    for (int i = 0; i < 19; ++i) {  // 合计 20 条
        log.append("io", message);
    }

    if (pathExists(gen1)) {
        std::cerr << "[event_logger_rotation_disabled] maxBytes=0 仍发生轮转（出现了 event_log.jsonl.1）"
                  << std::endl;
        return false;
    }
    if (sizeOf(cur) != singleLine * 20) {
        std::cerr << "[event_logger_rotation_disabled] 当前文件 " << sizeOf(cur)
                  << " 字节，期望 " << (singleLine * 20) << "（不轮转路径不得丢记录/不得截断）"
                  << std::endl;
        return false;
    }
    return true;
}
