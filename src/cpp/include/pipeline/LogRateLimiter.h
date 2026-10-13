#pragma once

#include <cstdint>

namespace pipeline {

// 逐脸 embed / 图库检索失败日志的**时间窗**（毫秒）：窗内最多记录一条，**不分站点、不看文本**。
// 唯一使用者是 `Engine::noteEmbedError`。
inline constexpr std::int64_t kEmbedErrorLogWindowMs = 5000;

// 日志限频器：把「逐帧 × 每脸」路径上的失败日志压到**每窗一条**。
//
// ── 关键契约（每一条都是判别性用例的断言对象）──
// 1. **唯一判据是时间**。本类**不接收**站点名、也不接收日志文本 —— 这不是省略，而是刻意的接口设计：
//    原实现曾把闸门写成「文本与上一条相同 **且** 在窗内才抑制」，于是逐帧路径上多张脸给出**不同**
//    err 文本时（一张维度不符、一张推理失败），两段文本逐帧交替 ⇒ **每次都记** ⇒ 逐帧×每脸刷屏、
//    日志量**无上界**。把文本比较从接口里拿掉后，该形态在**结构上不可再写出来**。
// 2. **首次调用恒放行**（`armed_` 为假时）。
// 3. 判定用 `<`：`nowMs - lastMs_ < windowMs_` ⇒ 抑制；`>=` ⇒ 放行。故「恰好等于窗口」放行。
//    `windowMs_ <= 0` ⇒ **关闭限频、恒放行**（与「0 = 不限」的本仓惯例一致：EventLogger /
//    StructuredLogger 的 `maxBytes == 0` 即关闭轮转）。⚠️ 这一条**必须**是显式分支，不能指望从
//    判定式里推出来：非单调输入（`nowMs < lastMs_`）下差值为负，`负 < 0` 为真 ⇒ 会被抑制，与
//    「关闭限频」相反。**此处曾漏写该分支**，正由用例 `ZeroWindowAlwaysAllows` 抓出。
// 4. **调用方必须传单调钟**（如 `steady_clock`）。本类**不做**时钟选择，也无法代为校验：
//    若传墙钟（`system_clock`）而系统被 NTP / 手动校时**回拨**，`nowMs < lastMs_` ⇒ 差值为负
//    ⇒ 恒小于窗口 ⇒ 在输入追平之前**一条都不打**，与「禁止静默」相反。非单调输入下本类的行为
//    是「抑制」，属**刻意的失败安全方向**（宁少记、不刷屏），不是缺陷。
// 5. **非线程安全**：调用方须自行串行化。`Engine` 用 `embedErrMu_` 保护（判据与窗口推进必须
//    在同一临界区内完成，否则两个线程会各自读到未推进的 `lastMs_` 而同时放行）。
class LogRateLimiter {
public:
    explicit LogRateLimiter(std::int64_t windowMs) : windowMs_(windowMs) {}

    // 传入单调钟毫秒。返回 true = 本次可记录（并已推进窗口起点）；false = 窗内，调用方应抑制。
    bool tryAcquire(std::int64_t nowMs) {
        // 关闭限频：恒放行，且不推进窗口起点（契约第 3 条的显式分支，理由见该条）。
        if (windowMs_ <= 0) return true;
        // 唯一判据是时间。窗口起点只由**放行**分支推进；抑制分支绝不改它（否则窗口会从抑制点滑动，
        // 与「以距上一条**记录**时刻度量」的语义不符）。
        if (armed_ && nowMs - lastMs_ < windowMs_) return false;
        armed_ = true;
        lastMs_ = nowMs;
        return true;
    }

    std::int64_t windowMs() const { return windowMs_; }

private:
    std::int64_t windowMs_ = 0;
    std::int64_t lastMs_ = 0;
    bool armed_ = false;
};

}  // namespace pipeline
