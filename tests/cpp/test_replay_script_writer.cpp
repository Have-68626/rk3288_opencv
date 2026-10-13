#include "pipeline/ReplayScriptWriter.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// 任务 C1：检测序列**序列化器**的契约测试
//
// 序列化器（pipeline::writeReplayScript）把「视频 → 逐帧检测」的结果写成文本，供
// tests/cpp/test_track_replay.cpp 的 parseReplayScript 消费。因此本文件的判据分两层：
//   1. **精确文本（golden）**：逐字节锁定输出，防止字段顺序/空格/换行漂移；
//   2. **独立 oracle 解析**：下方 parseOracle 复刻 test_track_replay.cpp 中解析器的
//      全部强制规则，用来证明「序列化器的输出确实能被那份解析器接受」——而不只是
//      「看起来像」。
//
// ⚠️ parseOracle 是 test_track_replay.cpp:parseReplayScript 的**测试镜像**，二者必须同步：
//    改动任一侧的格式规则时，另一侧要一起改，否则本层兼容性证明会失真。
// ══════════════════════════════════════════════════════════════════════════════

namespace {

using pipeline::ReplayDetection;
using pipeline::ReplayFrame;
using pipeline::ReplayScriptData;
using pipeline::writeReplayScript;

ReplayDetection det(int x, int y, int w, int h, bool auth, const std::string& id = "") {
    ReplayDetection d;
    d.x = x;
    d.y = y;
    d.width = w;
    d.height = h;
    d.authenticated = auth;
    d.identityId = id;
    return d;
}

ReplayFrame frame(int idx, long long ts, std::vector<ReplayDetection> dets, bool ran = true) {
    ReplayFrame f;
    f.frameIndex = idx;
    f.tsMs = ts;
    f.detectionRan = ran;
    f.detections = std::move(dets);
    return f;
}

std::string serialize(const ReplayScriptData& s, const std::string& header = "") {
    std::ostringstream os;
    std::string err;
    const bool ok = writeReplayScript(os, s, header, err);
    EXPECT_TRUE(ok) << "序列化应成功，err=" << err;
    EXPECT_TRUE(err.empty());
    return os.str();
}

// ── oracle：test_track_replay.cpp 解析器规则的独立复刻（仅本测试用）────────────
struct OracleDet {
    int x = 0, y = 0, w = 0, h = 0;
    bool auth = false;
    std::string id;
};
struct OracleFrame {
    int idx = 0;
    long long ts = 0;
    bool ran = true;
    std::vector<OracleDet> dets;
};

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool parseOracle(std::istream& in, std::vector<OracleFrame>& out, std::string& err) {
    out.clear();
    err.clear();
    std::string line;
    int lineNo = 0;
    std::size_t cur = 0;
    bool hasFrame = false;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#') continue;
        std::istringstream ls(text);
        char tag = 0;
        ls >> tag;
        if (tag == 'F') {
            OracleFrame f;
            int ran = 1;
            if (!(ls >> f.idx >> f.ts >> ran)) {
                err = "F 格式错误 @" + std::to_string(lineNo);
                return false;
            }
            if (ran != 0 && ran != 1) {
                err = "detectionRan 非 0/1 @" + std::to_string(lineNo);
                return false;
            }
            std::string extra;
            if (ls >> extra) {
                err = "F 多余字段 @" + std::to_string(lineNo);
                return false;
            }
            f.ran = (ran == 1);
            out.push_back(f);
            cur = out.size() - 1;
            hasFrame = true;
        } else if (tag == 'D') {
            if (!hasFrame) {
                err = "D 在 F 之前 @" + std::to_string(lineNo);
                return false;
            }
            OracleDet d;
            int auth = 0;
            if (!(ls >> d.x >> d.y >> d.w >> d.h >> auth >> d.id)) {
                err = "D 格式错误 @" + std::to_string(lineNo);
                return false;
            }
            if (auth != 0 && auth != 1) {
                err = "authenticated 非 0/1 @" + std::to_string(lineNo);
                return false;
            }
            std::string extra;
            if (ls >> extra) {
                err = "D 多余字段 @" + std::to_string(lineNo);
                return false;
            }
            d.auth = (auth == 1);
            if (d.id == "-") d.id.clear();
            if (d.auth && d.id.empty()) {
                err = "已认证却无身份 @" + std::to_string(lineNo);
                return false;
            }
            if (!d.auth && !d.id.empty()) {
                err = "未认证却带身份 @" + std::to_string(lineNo);
                return false;
            }
            for (const auto& p : out[cur].dets) {
                if (p.x == d.x && p.y == d.y && p.w == d.w && p.h == d.h) {
                    err = "同帧重复 bbox @" + std::to_string(lineNo);
                    return false;
                }
            }
            out[cur].dets.push_back(d);
        } else {
            err = "未知记录类型 @" + std::to_string(lineNo);
            return false;
        }
    }
    if (out.empty()) {
        err = "空序列";
        return false;
    }
    return true;
}

}  // namespace

// ── 空帧序列：只有 F 行、无 D 行，仍是合法序列 ────────────────────────────────
TEST(ReplayScriptWriter, EmptyFrameWritesOnlyFLine) {
    const ReplayScriptData s{frame(0, 1000, {})};
    EXPECT_EQ(serialize(s), "F 0 1000 1\n");
}

// ── 单帧多检测：字段顺序、已认证/未认证编码 ──────────────────────────────────
TEST(ReplayScriptWriter, SingleFrameMultipleDetectionsGolden) {
    const ReplayScriptData s{frame(3, 1234, {det(10, 20, 30, 40, true, "alice"),
                                             det(50, 60, 70, 80, false)})};
    EXPECT_EQ(serialize(s),
              "F 3 1234 1\n"
              "D 10 20 30 40 1 alice\n"
              "D 50 60 70 80 0 -\n");
}

// ── 多帧 + detectionRan=false 的编码 ────────────────────────────────────────
TEST(ReplayScriptWriter, MultipleFramesGolden) {
    const ReplayScriptData s{
        frame(0, 1000, {det(1, 2, 3, 4, true, "bob")}),
        frame(1, 1100, {}, /*ran=*/false),
        frame(2, 1200, {det(-5, -6, 7, 8, false)}),
    };
    EXPECT_EQ(serialize(s),
              "F 0 1000 1\n"
              "D 1 2 3 4 1 bob\n"
              "F 1 1100 0\n"
              "F 2 1200 1\n"
              "D -5 -6 7 8 0 -\n");
}

// ── 时间戳/帧号：大数值与稀疏步进必须原样透传（解析器按 long long 读 tsMs）──────
TEST(ReplayScriptWriter, LargeTimestampAndSparseFrameIndexPreserved) {
    const ReplayScriptData s{frame(99999, 1234567890123LL, {det(0, 0, 1, 1, false)})};
    EXPECT_EQ(serialize(s), "F 99999 1234567890123 1\nD 0 0 1 1 0 -\n");
}

// ── 注释头：以 '#' 起始、不干扰正文，且被 oracle 忽略 ─────────────────────────
TEST(ReplayScriptWriter, CommentHeaderPrefixesBody) {
    const ReplayScriptData s{frame(0, 40, {det(7, 8, 9, 10, true, "carol")})};
    const std::string header = "source=demo.mp4\nstride=2";
    const std::string text = serialize(s, header);
    EXPECT_EQ(text,
              "# source=demo.mp4\n"
              "# stride=2\n"
              "F 0 40 1\n"
              "D 7 8 9 10 1 carol\n");

    std::istringstream in(text);
    std::vector<OracleFrame> parsed;
    std::string err;
    ASSERT_TRUE(parseOracle(in, parsed, err)) << err;
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].idx, 0);
    EXPECT_EQ(parsed[0].ts, 40);
    EXPECT_EQ(parsed[0].dets.size(), 1u);
    EXPECT_TRUE(parsed[0].dets[0].auth);
    EXPECT_EQ(parsed[0].dets[0].id, "carol");
}

// ── 确定性：交错输入后，同一输入序列化的输出仍逐字节相同（无跨调用隐藏状态/静态缓冲）──
// 为什么必须**交错**：连续两次对**同一**输入调用同一个纯函数几乎必然相等（无参差输入），
// 该断言结构上接近恒真、判别力弱。真正的风险是**跨调用隐藏状态**（static 缓冲/缓存未复位）。
// 故在两次对 s 的序列化之间插入一次**不同输入** s2 的序列化，再比对两次 s 的输出——
// 只有「无跨调用状态」才能通过。golden 比对（CommentHeaderPrefixesBody）抓不到这一维度。
TEST(ReplayScriptWriter, SerializationIsDeterministic) {
    const ReplayScriptData s{
        frame(0, 100, {det(1, 2, 3, 4, true, "x"), det(5, 6, 7, 8, false)}),
        frame(1, 200, {}),
    };
    // 交错用的**不同**输入（帧号/时间戳/检测/ran 均不同）。
    const ReplayScriptData s2{
        frame(9, 9000, {det(100, 110, 120, 130, true, "other"), det(0, 0, 1, 1, false)}),
        frame(10, 9100, {}, /*ran=*/false),
        frame(11, 9200, {det(7, 7, 7, 7, true, "y")}),
    };
    const std::string a = serialize(s);
    const std::string s2out = serialize(s2);  // 交错：不同输入夹在两次 s 之间
    const std::string b = serialize(s);
    EXPECT_EQ(a, b) << "交错不同输入后，两次对 s 的输出必须逐字节一致（无跨调用隐藏状态）";
    EXPECT_NE(a, s2out) << "参照非空：s2 必须是**不同**输入，交错才有意义";
}

// ── 兼容性：序列化器输出必须能被（镜像的）解析器无损接受 ──────────────────────
TEST(ReplayScriptWriter, OutputIsAcceptedByOracleParser) {
    const ReplayScriptData s{
        frame(0, 1000, {det(200, 150, 160, 200, true, "alice"),
                        det(400, 300, 120, 120, false)}),
        frame(1, 1100, {}, false),
    };
    const std::string text = serialize(s, "provenance: unit-test");

    std::istringstream in(text);
    std::vector<OracleFrame> parsed;
    std::string err;
    ASSERT_TRUE(parseOracle(in, parsed, err)) << err;
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0].dets.size(), 2u);
    EXPECT_EQ(parsed[0].dets[0].id, "alice");
    EXPECT_TRUE(parsed[0].dets[0].auth);
    EXPECT_FALSE(parsed[0].dets[1].auth);
    EXPECT_TRUE(parsed[0].dets[1].id.empty());
    EXPECT_FALSE(parsed[1].ran);
}

// ── 承重断言 1（RED 判据）：已认证却无身份必须在**写出前**被拒绝 ───────────────
TEST(ReplayScriptWriter, RejectsAuthenticatedWithoutIdentity) {
    const ReplayScriptData s{frame(0, 1000, {det(1, 2, 3, 4, true, "")})};
    std::ostringstream os;
    std::string err;
    EXPECT_FALSE(writeReplayScript(os, s, err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(os.str(), "") << "校验先于写出：拒绝时不得留下半成品";

    // 显式的 "-" 同样是拒绝①（解析器会把 '-' 归一成空身份）。
    const ReplayScriptData s2{frame(0, 1000, {det(1, 2, 3, 4, true, "-")})};
    std::ostringstream os2;
    std::string err2;
    EXPECT_FALSE(writeReplayScript(os2, s2, err2));
}

// ── 承重断言 2（RED 判据）：未认证却携带身份串必须被拒绝 ──────────────────────
TEST(ReplayScriptWriter, RejectsUnauthenticatedWithIdentity) {
    const ReplayScriptData s{frame(0, 1000, {det(1, 2, 3, 4, false, "alice")})};
    std::ostringstream os;
    std::string err;
    EXPECT_FALSE(writeReplayScript(os, s, err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(os.str(), "");

    // 规范编码 "-" 与空串仍是允许的（等价于「无身份」）。
    const ReplayScriptData ok{frame(0, 1000, {det(1, 2, 3, 4, false, "-"),
                                              det(5, 6, 7, 8, false, "")})};
    std::ostringstream osk;
    std::string errk;
    EXPECT_TRUE(writeReplayScript(osk, ok, errk)) << errk;
}

// ── 同帧重复 bbox 必须被拒绝（真值身份归属未定义）──────────────────────────────
TEST(ReplayScriptWriter, RejectsDuplicateBboxInSameFrame) {
    const ReplayScriptData s{frame(0, 1000, {det(1, 2, 3, 4, true, "a"),
                                             det(1, 2, 3, 4, true, "b")})};
    std::ostringstream os;
    std::string err;
    EXPECT_FALSE(writeReplayScript(os, s, err));
    EXPECT_FALSE(err.empty());
}

// ── 身份串含空白会被解析器拆成多余字段，须在写出前拦截 ────────────────────────
TEST(ReplayScriptWriter, RejectsWhitespaceInIdentity) {
    const ReplayScriptData s{frame(0, 1000, {det(1, 2, 3, 4, true, "a b")})};
    std::ostringstream os;
    std::string err;
    EXPECT_FALSE(writeReplayScript(os, s, err));
    EXPECT_FALSE(err.empty());
}
