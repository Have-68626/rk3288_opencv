#pragma once

#include "Compat.h"
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace rk_win {

// Windows 事件日志：JSONL（一行一事件，append-only）。当前**无生产读取端**（无 HTTP 端点消费它；
// 全仓 `event_log` 字面量只出现在本类与 tests/win/test_event_logger.cpp），消费方是运维/排查时的文本工具。
// 轮转命名与 StructuredLogger 一致：`event_log.jsonl.1` … `event_log.jsonl.N`，**最新在 `.1`**，
// 让两套日志共用同一套运维习惯（找「上一轮」= 看 `.1`）。
// ✅ 代数收缩清理（原 `(a)` 残留）**已修复**：`maxRollFiles` 由大调小时，索引大于新值的既有档位
//   （`.6` … `.N`）由 `pruneShrunkGenerations`（见 LogRotation.h）在 `open()` 中删除。轮转循环只从
//   `maxRollFiles_ - 1` 往下走、**不触及**这些高代数档位，故清理交给 `open()`承担——收缩只可能发生在
//   两次 open 之间（`maxRollFiles_` 在一次 open 的生命周期内是常量），无需在每次轮转时重复扫目录。
//   `StructuredLogger` 曾有同形态，其清理由该类的 `open()` 自行承担。
// ⚠️ 一条**已登记的残留**（本次只登记，**不改行为**）。注意：本批把 event_log.jsonl 从硬编码
//   10 MiB / 5 改为随 `cfg.log.maxFileBytes` / `cfg.log.maxRollFiles` 接线 ⇒ 下述残留的可达性随之
//   **扩大**（运维现在能把这两个上限调到 schema 允许的 1 GiB / 1000 档，从而真正把它触发到）：
//   (b) `maxBytes == 0`（或 `maxRollFiles <= 0`）⇒ **关闭轮转**、该日志无界增长。JSON 侧 schema 有
//       下限兜住（maxFileBytes ≥ 1024、maxRollFiles ≥ 1），但 **INI 路径没有**：
//       `loadConfigFromIniOrDefault` 只钳上界、不抬下界（见 clampLogMaxFileBytes /
//       clampLogMaxRollFiles 的「只压上界」）⇒ 手工把 INI 写成 0 会被原样带入 cfg。
class EventLogger {
public:
    // maxBytes / maxRollFiles 语义与 `StructuredLogger::open` 一致：`maxBytes == 0` 或
    // `maxRollFiles <= 0` ⇒ 关闭轮转（文件无限增长）。到达上限即轮转。
    // ✅ 接线现状（本次修复订正，原注释**失实**）：两个调用点**均已**传满 3 个实参，取自
    //    `cfg.log.maxFileBytes` / `cfg.log.maxRollFiles` —— `win_local_service_main.cpp:122` 与
    //    `win_camera_face_recognition_main.cpp:853`（均为
    //    `events.open(cfg.log.logDir, cfg.log.maxFileBytes, cfg.log.maxRollFiles)`）。
    //    也就是说：运维在 config.json 里改这两键**会**影响事件日志轮转（原注释称「二者仍以默认实参
    //    调用、接线需改上述两处」是改线之前的旧口径）。
    //    下方默认实参（10 MiB / 5）保留：它与 LogConfig 的默认值同值，且使「仅传 dir」的调用点源
    //    兼容——当前全仓**没有**这种调用点（两个 main 与 tests/win/test_event_logger.cpp 都传满 3 个
    //    实参），保留默认实参只为形态一致与后续可用。
    bool open(const std::filesystem::path& dir,
              std::uint64_t maxBytes = 10ULL * 1024ULL * 1024ULL,
              int maxRollFiles = 5);
    void close();

    void append(const std::string& eventType, const std::string& message);

private:
    // 到达上限即轮转。**先 close、再改名、再以 app 重开**——理由见 .cpp 内注释（Windows 上对仍被
    // 本进程打开的文件改名会以共享冲突失败，只 rename 不 close 的写法会让轮转静默失效）。调用方须已持 mu_。
    // 本函数只做代数**迁移**（循环起点 maxRollFiles_ - 1 ⇒ 不触及更高的既有档位）；代数收缩清理**不在
    // 本函数**，而由 open() 经 `pruneShrunkGenerations` 承担 —— 原因见类级注释。另一条边界残留
    // （maxBytes == 0 关闭轮转）见类级注释与 .cpp 内的登记。
    void rollIfNeededLocked(const std::filesystem::path& path);

    std::mutex mu_;
    std::filesystem::path path_;
    std::uint64_t maxBytes_ = 0;
    int maxRollFiles_ = 0;
    std::ofstream out_;
};

}  // namespace rk_win
