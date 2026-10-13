// 判别性测试：pipeline::LogRateLimiter（逐脸 embed 失败日志的时间窗限频闸门）。
//
// 背景：Engine::noteEmbedError 的限频闸门一度被写成「文本与上一条相同 **且** 在窗内才抑制」，
// 于是逐帧 × 每脸路径上多张脸给出**不同** err 文本时（一张维度不符、一张推理失败）两段文本
// 逐帧交替 ⇒ 每次都记 ⇒ 日志量**无上界**刷屏。修复为**纯时间窗**判据。该修复此前无任何测试
// 承重（变异扫掠：把闸门改回修复前形态，全仓测试全绿无红）——本文件正是补上这份承重。
//
// 为使其可测，闸门已抽成 pipeline::LogRateLimiter（src/cpp/include/pipeline/LogRateLimiter.h，
// 本文件**只读不改**）。该类刻意**不接收**站点名与文本参数——这正是「文本比较在结构上不可再
// 写出来」的保证：下方 SuppressesWithinWindowRegardlessOfText 因此同时是对该结构保证的断言。
//
// 契约（逐条来自该头文件，是本文件全部断言的依据）：
//   · 唯一判据是**时间**；首次调用恒放行；判定用 `<`（nowMs-lastMs_ < windowMs_ ⇒ 抑制），
//     故「恰好等于窗口」放行；windowMs_==0 ⇒ 恒放行；调用方必须传单调钟；**非线程安全**。
#include "pipeline/LogRateLimiter.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

// ── 用例 1：首次调用恒放行（未 armed 时不看时间）────────────────────────────────
//
// 判别点 A：首个时刻取 0——若把实现里 `if (armed_ && nowMs - lastMs_ < windowMs_)` 的
//           `armed_ &&` 去掉，判据退化为 `0 - 0 < 5000`（真）⇒ 首次即被抑制，本条翻红。
// 判别点 B：首个时刻取一个很大的值——证明放行与 nowMs 的大小**无关**（未 armed 时根本不咨询时间）。
TEST(LogRateLimiter, FirstCallAlwaysAllowedRegardlessOfTimestamp) {
    {
        pipeline::LogRateLimiter r(pipeline::kEmbedErrorLogWindowMs);
        EXPECT_TRUE(r.tryAcquire(0)) << "首次调用（t=0）必须放行：未 armed 时不得咨询时间";
    }
    {
        pipeline::LogRateLimiter r(pipeline::kEmbedErrorLogWindowMs);
        const std::int64_t huge = std::numeric_limits<std::int64_t>::max() / 2;
        EXPECT_TRUE(r.tryAcquire(huge)) << "首次调用（t=极大）必须放行：未 armed 时不看时间";
    }
}

// ── 用例 2（★本组最核心）：窗内抑制，且**与文本无关** ──────────────────────────
//
// 同一窗口内连续多次调用只有第一次为 true。这条是修复前形态（要「文本与上一条相同」才抑制）
// 的**反例**：在修复前形态下，多张脸逐帧交替给出**不同** err 文本会每次都记；本类接口里
// **根本没有**站点名 / 文本参数 ⇒ 文本比较在结构上不可表达，故这些连续 EXPECT_FALSE 成立
// **与文本如何变化无关**。换言之，本条同时是对「抑制与文本无关」这一结构保证的断言。
//
// 判别点：把判据改回「文本相同才抑制」——在**当前接口下无法编译**（无文本形参可比较）；
//        等价地，若在**调用方**（Engine）重新引入文本比较，则由文件末的
//        LogRateLimiterStructuralGuard 结构用例兜住；调用点另有**行为级**承重用例
//        `EngineTest.NoteEmbedErrorLimiterSuppressesSecondDistinctTextWithinWindow`
//        （tests/cpp/test_engine.cpp，编入 `core_gtest_tests` target）：同一 processFrameOnce 内
//        两张脸各产出一条**文本不同**的 err，断言只放行一条（total==1），回退即确定 RED。
TEST(LogRateLimiter, SuppressesWithinWindowRegardlessOfText) {
    pipeline::LogRateLimiter r(pipeline::kEmbedErrorLogWindowMs);
    EXPECT_TRUE(r.tryAcquire(1000));   // 首次：放行
    EXPECT_FALSE(r.tryAcquire(1000));  // 同刻：0 - 0 < 5000 ⇒ 抑制
    EXPECT_FALSE(r.tryAcquire(1001));  // 窗内
    EXPECT_FALSE(r.tryAcquire(5999));  // 窗内（4999 < 5000）
    EXPECT_FALSE(r.tryAcquire(5999));  // 窗内、同刻，仍抑制——窗内最多放行一次
}

// ── 用例 3：边界「恰好等于窗口」放行（判据用 `<` 而非 `<=`），且随后**起点被推进** ──
//
// 判别点 A：把 `<` 改成 `<=` ⇒ `6000 - 1000 = 5000 <= 5000` 为真 ⇒ 本次被抑制，
//           紧随其后的 EXPECT_TRUE 翻红。
// 判别点 B（起点推进）：放行分支必须更新 lastMs_。若放行分支忘了更新（起点仍为 1000），
//           则紧接着的同刻调用相对旧起点是 5000（不小于窗口）⇒ 会**错误放行**，末条翻红。
TEST(LogRateLimiter, ExactlyWindowElapsedIsAllowedAndAdvancesStart) {
    pipeline::LogRateLimiter r(pipeline::kEmbedErrorLogWindowMs);
    EXPECT_TRUE(r.tryAcquire(1000));
    EXPECT_FALSE(r.tryAcquire(1000));
    EXPECT_TRUE(r.tryAcquire(6000)) << "恰好等于窗口应放行（判据是 `<`：5000 < 5000 为假）";
    EXPECT_FALSE(r.tryAcquire(6000)) << "放行后窗口起点应推进到 6000：同刻相对新起点是 0 < 5000";
}

// ── 用例 4：窗外放行（now - last == window + 1）────────────────────────────────
TEST(LogRateLimiter, JustPastWindowIsAllowed) {
    pipeline::LogRateLimiter r(pipeline::kEmbedErrorLogWindowMs);
    EXPECT_TRUE(r.tryAcquire(1000));
    EXPECT_FALSE(r.tryAcquire(5999));  // 窗内（4999 < 5000）
    EXPECT_TRUE(r.tryAcquire(6001)) << "刚过窗口 1ms 应放行（5001 不小于 5000）";
}

// ── 用例 5：窗口推进的链式语义（抓住「放行分支忘了推进起点」类错）───────────────
//
// 序列：t0 放行 → t0+1000 抑制 → t0+4000 抑制 → t0+5000 放行 → t0+9000 抑制。
// 判别点（反例推导）：若 t0+5000 的**放行**分支忘了推进窗口起点，起点仍是 t0=1000，
//   则 t0+9000=10000 相对 t0 是 9000 ≥ 5000 ⇒ 会**错误放行**，末条 EXPECT_FALSE 翻红。
//
// 注：本用例针对的是「放行分支漏推进」（用例 3 的第二半同型）；而「抑制分支误推进」由
//     NonMonotonicInputIsSuppressedAndDoesNotAdvanceStart 覆盖——两者是不同代码行，须分别钉。
TEST(LogRateLimiter, WindowStartAdvancesOnEveryAllow) {
    pipeline::LogRateLimiter r(pipeline::kEmbedErrorLogWindowMs);
    EXPECT_TRUE(r.tryAcquire(1000));    // t0：放行，起点=1000
    EXPECT_FALSE(r.tryAcquire(2000));   // t0+1000：1000 < 5000 ⇒ 抑制（起点不变）
    EXPECT_FALSE(r.tryAcquire(5000));   // t0+4000：4000 < 5000 ⇒ 抑制（起点不变）
    EXPECT_TRUE(r.tryAcquire(6000));    // t0+5000：5000 不小于 5000 ⇒ 放行，**起点推进到 6000**
    EXPECT_FALSE(r.tryAcquire(10000))   // t0+9000：距新起点 4000 < 5000 ⇒ 抑制
            << "起点未随放行推进：t0+9000 相对旧起点 t0 已达 9000，会错误放行";
}

// ── 用例 6：非单调输入 ⇒ 抑制，且**不推进**起点 ────────────────────────────────
//
// 本类对非单调输入（nowMs < lastMs_）的行为是「抑制」，这是**刻意的失败安全方向**
//（宁少记、不刷屏），不是缺陷——契约第 4 条。
//
// 现实触发：调用方若误传墙钟（system_clock），NTP / 手动校时**回拨**后 `nowMs - lastMs_` 恒为负
// ⇒ 恒小于窗口 ⇒ 在输入追平前一条都不打，与「禁止静默」相反。**这正是 Engine 必须用单调钟
//（steady_clock）的原因**（Engine.cpp 里的 steadyNowMs() 注释同此口径）。
//
// 判别点：抑制分支**不得**更新 lastMs_。若误推进（把起点改成 9000），则 14000 相对 9000 是 5000
//   （不小于窗口）⇒ 会**错误放行**；起点未被污染时 14000 相对 10000 是 4000 < 5000 ⇒ 正确抑制。
TEST(LogRateLimiter, NonMonotonicInputIsSuppressedAndDoesNotAdvanceStart) {
    pipeline::LogRateLimiter r(pipeline::kEmbedErrorLogWindowMs);
    EXPECT_TRUE(r.tryAcquire(10000));   // 放行，起点=10000
    EXPECT_FALSE(r.tryAcquire(9000))    // 非单调：9000 - 10000 = -1000 < 5000 ⇒ 抑制
            << "非单调输入应抑制（刻意的失败安全方向，非缺陷）";
    EXPECT_FALSE(r.tryAcquire(14000))   // 起点仍为 10000：4000 < 5000 ⇒ 抑制
            << "抑制分支误推进了起点：14000 相对被污染的 9000 会错误放行";
    EXPECT_TRUE(r.tryAcquire(15000))    // 恢复单调：15000 - 10000 = 5000 ⇒ 按原起点正常放行
            << "起点应保持原值，恢复单调后按原起点判定";
}

// ── 用例 7：windowMs == 0 ⇒ 恒放行（含连续同刻调用）────────────────────────────
//
// 判别点：判据 `<` 改成 `<=` ⇒ 同刻 0 <= 0 为真 ⇒ 第 2 次起被抑制，第 2/3 条 EXPECT_TRUE 翻红。
TEST(LogRateLimiter, ZeroWindowAlwaysAllows) {
    pipeline::LogRateLimiter r(0);
    EXPECT_TRUE(r.tryAcquire(1000));
    EXPECT_TRUE(r.tryAcquire(1000));  // 连续同刻：0 < 0 为假 ⇒ 放行
    EXPECT_TRUE(r.tryAcquire(1000));
    EXPECT_TRUE(r.tryAcquire(0));
}

// ── 用例 8：windowMs() getter 返回构造时传入的值（含 0）────────────────────────
//
// 判别点：把 getter 改成返回常量（如 kEmbedErrorLogWindowMs）⇒ 第 2、3 条（构造 0 / 12345）翻红。
TEST(LogRateLimiter, WindowMsGetterReturnsConstructedValue) {
    EXPECT_EQ(pipeline::LogRateLimiter(pipeline::kEmbedErrorLogWindowMs).windowMs(),
              pipeline::kEmbedErrorLogWindowMs);
    EXPECT_EQ(pipeline::LogRateLimiter(0).windowMs(), 0);
    EXPECT_EQ(pipeline::LogRateLimiter(12345).windowMs(), 12345);
}

// ── 用例 9：大批量不变量（随机相位递增序列，逐次比对独立参考实现）─────────────
//
// 序列从 0 起按 137ms 递增 200 次。137 与窗口 5000 互质 ⇒ 序列会走遍各种相位
//（紧邻窗内、恰在边界附近等），比「固定几个手挑时刻」更能暴露判定式的离界错误。
//
// 参考实现是对**契约文本**的朴素直译（3 行核心循环），**不复用** LogRateLimiter。逐次比对
//（不只比总数）能抓住「判定式被改坏但恰好计数对了」的形态——例如把 `<` 改成 `<=` 时总放行数
// 可能接近，但某些相位上的逐次结果必有一处不同。
TEST(LogRateLimiter, MirrorsNaiveReferenceOverLongSequence) {
    std::vector<std::int64_t> times;
    times.reserve(200);
    for (int i = 0; i < 200; ++i) {
        times.push_back(137LL * i);
    }

    // 独立参考实现：契约的朴素直译（首次放行；抑制当且仅当 armed 且窗内；放行时推进起点）。
    std::vector<int> expected;
    expected.reserve(times.size());
    {
        bool armed = false;
        std::int64_t last = 0;
        for (std::int64_t t : times) {
            const bool ok = !(armed && (t - last) < pipeline::kEmbedErrorLogWindowMs);
            if (ok) {
                armed = true;
                last = t;
            }
            expected.push_back(ok ? 1 : 0);
        }
    }

    pipeline::LogRateLimiter r(pipeline::kEmbedErrorLogWindowMs);
    int allowed = 0;
    for (std::size_t i = 0; i < times.size(); ++i) {
        const int got = r.tryAcquire(times[i]) ? 1 : 0;
        EXPECT_EQ(got, expected[i])
                << "第 " << i << " 次调用 (t=" << times[i] << ") 的判别与参考实现不一致";
        allowed += got;
    }
    // 防「空转通过」：序列长度 200、窗宽 5000、步长 137 ⇒ 参考放行数应为 6 量级，
    // 既非 0（全抑制 ⇒ 参考写错或窗口恒抑制）也非 200（全放行 ⇒ 抑制逻辑失效）。
    EXPECT_GT(allowed, 2) << "整段序列放行过少，参考实现或窗口语义可疑";
    EXPECT_LT(allowed, static_cast<int>(times.size())) << "全部放行说明抑制逻辑已失效";
}

// ── 结构不变量（读源码文本）：调用点不得重新引入「文本比较」形态 ─────────────────
//
// ⚠️ **为什么放在本文件**：读源码文本需要 RK_TEST_SOURCE_ROOT；该宏现对 core_unit_tests、
//    core_gtest_tests、win_unit_tests 三个 target 定义（分别见 cmake/core.cmake、CMakeLists.txt
//    与 cmake/windows.cmake 的注入块）。而 behavior 用例覆盖不到「Engine 的**调用点**是否又引入了
//    文本比较」——Engine.cpp 不在 core_unit_tests 的编译面内（不被本 target 编译）。
//
// ⚠️ **判据的局限（如实登记，勿读成强保证）**：本段是**形态判据**（对去注释、去空白后的源码
//    做子串匹配），**挡不住**等价改写、极性反转、以及「把条件写成恒假」这类把守卫架空的改法——
//    例如把比较挪进一个名字不含被禁子串的辅助函数、用宏包一层、或比较后立即把结果丢弃。
//    它与仓内既有结构测试（test_checked_arith.cpp / test_track_coordinator.cpp /
//    test_event_manager.cpp）同一量级，是**行为用例之外的补充**，**不**等价于行为覆盖，
//    **不**主张更强。
// ⚠️ 脆弱面（如实登记）：函数体切片靠纯字符计数（matchBrace）。Engine.cpp 的
//    noteEmbedError 体内若出现字面量 '{' / '}'（字符串/字符常量）会使计数失准——多计 ⇒ 切片
//    变大（最坏把相邻函数纳入，可能假绿）；少计 ⇒ 切片早截（可能把不该有的匹配截掉，亦可能
//    让负向断言在残缺切片上假绿）。故本用例设了**正向锚**（必须出现 tryAcquire）与**切片长度
//    下限**两道防线，使「切片为空/被误截」无法伪装成「全部合规」。
#ifdef RK_TEST_SOURCE_ROOT
namespace {

// 读取源码根下相对路径文件；失败（或路径写错）返回空串——调用方**必须**显式断言非空，
// 否则「扫描集合为空」会静默地让所有负向断言都通过（空 == 空）。
std::string readRepoFile(const std::string& rel) {
    std::ifstream in(std::string(RK_TEST_SOURCE_ROOT) + "/" + rel, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// 剥离 `//` 行注释与 `/* */` 块注释（块注释状态**跨行**保留），与 test_track_coordinator.cpp
// 的 stripComments 同口径：源码里**讨论**这些符号的说明性注释不应造成误报。
std::string stripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool inBlock = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (inBlock) {
            if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/') {
                inBlock = false;
                ++i;
            }
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size()) {
            if (src[i + 1] == '/') {  // 行注释：跳到行尾（保留换行）
                while (i < src.size() && src[i] != '\n') ++i;
                out.push_back('\n');
                continue;
            }
            if (src[i + 1] == '*') {
                inBlock = true;
                ++i;
                continue;
            }
        }
        out.push_back(src[i]);
    }
    return out;
}

// 去掉全部空白：使断言对格式漂移免疫（`err == last` 与 `err==last` 等价）。
std::string squeeze(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

// 取 `open`（指向 '{'）所属**块**的收尾 '}' 之后一个下标；未配对到时返回 npos。
// 用途：把结构断言的切片限制在**单个函数体**内，避免越界读到相邻函数而让「本函数缺守卫」
// 被邻函数掩盖（假通过）。
std::size_t matchBrace(const std::string& s, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == '{') {
            ++depth;
        } else if (s[i] == '}') {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return std::string::npos;
}

}  // namespace

TEST(LogRateLimiterStructuralGuard, NoteEmbedErrorUsesLimiterWithoutTextComparison) {
    const std::string raw = readRepoFile("src/cpp/src/Engine.cpp");
    ASSERT_FALSE(raw.empty()) << "无法读取 src/cpp/src/Engine.cpp（RK_TEST_SOURCE_ROOT="
                              << RK_TEST_SOURCE_ROOT << "）";
    const std::string body = squeeze(stripComments(raw));
    ASSERT_GT(body.size(), 10000u) << "Engine.cpp 文本异常短，疑似路径写错";

    // 定位 Engine::noteEmbedError 的**定义体**。去空白后形如
    // `voidEngine::noteEmbedError(constchar*site,conststd::string&err){` —— 形参表内无 '{'，
    // 故其后首个 '{' 即函数体起始。
    const std::size_t at = body.find("voidEngine::noteEmbedError(");
    ASSERT_NE(at, std::string::npos) << "Engine.cpp 中找不到 Engine::noteEmbedError 定义"
                                        "（符号改名或路径变化？须同步本用例）";
    const std::size_t brace = body.find('{', at);
    ASSERT_NE(brace, std::string::npos) << "找不到 noteEmbedError 函数体起始 '{'";
    const std::size_t end = matchBrace(body, brace);
    ASSERT_NE(end, std::string::npos) << "noteEmbedError 的函数体花括号未配对（格式变化？）";
    const std::string fnBody = body.substr(brace, end - brace);
    // 切片长度下限：防「切片为空/被误截」让下方负向断言在残缺切片上假绿。
    ASSERT_GT(fnBody.size(), 120u) << "noteEmbedError 切片过短，勿让空/残缺切片伪装成通过";

    // ① 正向锚：限频闸门仍由 LogRateLimiter::tryAcquire 承担。
    //    判别点：删掉调用点或改回内联时间比较 ⇒ 本断言翻红（限频器未被接线）。
    ASSERT_NE(fnBody.find("tryAcquire"), std::string::npos)
            << "noteEmbedError 未调用 tryAcquire：限频闸门未接线到 LogRateLimiter"
               "（或已被改回内联实现）";

    // ② 负向：函数体内**不得**把 err 文本与「上一条」做相等/不等比较。
    //    判别点：重新引入 `err == last…` / `lastEmbedErr…` / `err.compare(…)` 之类形态 ⇒ 翻红。
    //    ⚠️ `lastEmbedErr` 是**前缀匹配**：若将来合法地保留一个**时间戳**成员却取名为
    //       `lastEmbedErrMs_`（而非把状态放进 LogRateLimiter），本断言会假红——届时须确认它
    //       是否真的与 err 文本比较无关，再调整白名单，而不是直接删断言。
    const char* const kForbidden[] = {
        "lastEmbedErr",   // 「上一条」错误（文本或时间戳）命名的前置形态
        "prevErr",        // 同义的「上一条错误」变量
        "err==",          // err 与某物相等比较（去空白后形态）
        "==err",          // 反向相等比较
        "err!=",          // err 不等比较
        "!=err",          // 反向不等比较
        "err.compare(",   // 字符串比较成员调用
    };
    for (const char* tok : kForbidden) {
        EXPECT_EQ(fnBody.find(tok), std::string::npos)
                << "noteEmbedError 内出现文本比较形态 `" << tok << "`：闸门退化为「文本相同才抑制」，"
                   "多张脸给出不同 err 文本时会逐帧（×每脸）刷屏、日志量无上界";
    }
}

#else
// 这条 `#error` 是**故意**的（同 test_track_coordinator.cpp / test_event_manager.cpp 的范式）：
// 若本文件被编进一个未定义 RK_TEST_SOURCE_ROOT 的 target，上方整段结构式守卫会**静默消失**
//（既不注册、也不 skip、也不 fail，零信号）。宁可让构建当场失败，也不要留一个看起来在守、
// 实际不存在的守卫。本文件当前唯一编入的 target 是 core_unit_tests，它定义了该宏。
#error "test_log_rate_limiter.cpp 的结构式守卫依赖 RK_TEST_SOURCE_ROOT；缺失它会让守卫静默消失"
#endif  // RK_TEST_SOURCE_ROOT
