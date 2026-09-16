#include "pipeline/TrackCoordinator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// 任务 C1：轨迹验收的**离线回放载体**
//
// 要解决的问题：设备验收清单里的「不串人 / 不产生重复 ID」与轨迹阈值 0.8 的验收，在真机
// 链路上**物理不可测**——confirmFrames/missedActual 是私有成员无 getter；含 trackId 的文本
// **只进 UI、不进任何可离线复核的产物**（ResultPublisher 的多脸分支确实输出 `T<trackId>=<stableId>`，
// 但该文本经 native-lib sendRecognitionResult 只送到 Java onNativeResult 更新界面，不落盘）；
// PerfReporter 的 CSV 只有 6 列耗时/RSS，无轨道字段；而全仓没有任何离线回放入口（全部 CLI 目标
// 都是单帧范式）。
//
// 本载体的思路：把「沿途录像回放」换成「**检测序列回放**」——
//
//     检测序列（内存脚本或文本文件） ──► TrackCoordinator::update ──► 逐帧对外视图 ──► 指标
//
// **为什么驱动 TrackCoordinator 而非 Engine（与原方案 C1 的偏离）**：
// 三项指标只取决于「每帧有哪些检测框 + 各自被判成什么身份」，而这恰是 TrackCoordinator 的
// 输入——上游（检测器精度、人脸对齐、图库匹配）只影响**哪些框到达**，不影响**框如何被跟踪**。
//
// **限定条件（勿省略）**：该等价性只在「**静态图库 + 模型常驻**」前提下成立。Engine 另有三处
// 会在检测序列**之后**改写 coordinator 的输入或输出，**本载体不模拟**这三条路径：
//   * `Engine.cpp:1285-1293` 读路径代次作废——把 isAuthenticated 降级、identityId 改 "Unknown"
//     后**再交给** update()。注意这改的是 **coordinator 的输入**，会直接影响
//     firstConfirmSteps / staleIdentityFrames；
//   * `Engine.cpp:853-856` applyGalleryUpdate→resetIdentitiesExcept 就地清 currentTracks_ 的身份；
//   * `Engine.cpp:1101-1102` 模型缺失时 currentTracks_.clear()——与脚本 detectionRan=0 的
//     「保持现状」语义**不同**。
// 另：Engine 成功路径**恒传 detectionRan=true**（`Engine.cpp:1142` 是唯一赋值点，返回 false 的
// 分支都在它之前），故脚本里的 `detectionRan=0` 模拟的是 coordinator 的「调度跳过」语义，
// **产线上没有对应路径**。（`Engine::trackFaces` 注释所称「模型缺失跳过检测时为 false」与
// `:1098-1102` 的实际行为不符，属既有陈旧注释，不在本次改动范围。）
//
// 本层的实利：① 零 OpenCV/ncnn 依赖；② target 是 core_unit_tests → Linux 与 Windows CI 都会跑；
//   ③ 不需要任何图像素材，跑上万帧仍在毫秒级。
// Engine 侧「检测→身份→coordinator」的管道正确性由 test_engine.cpp 既有用例覆盖，本文件不重复。
//
// **素材来源与已知局限（如实声明，勿当作已完成的真机验收）**：
// 本载体消费的是**检测序列**，不是视频。要从真实录像产出素材，必须先有「视频 → 逐帧检测框 +
// 身份」的导出步骤——该步骤在本仓库**尚不存在**（也正因如此，本任务严格意义上的真机验收证据
// 仍需用户提供录像）。`models/calib_images/` 的 172 帧虽在本机，但帧号稀疏（跨 2..1025，
// **非相邻帧**）、逐帧位移远大于真实相邻帧、且未逐帧确认是否为同一目标，**不能代表常规使用**，
// 故**不作为**验收素材。
// ══════════════════════════════════════════════════════════════════════════════

namespace {

// ── 检测序列文本格式（可由外部工具生成，供真实素材导入）──────────────────────
//
//   # 以 '#' 起始的行为注释；空行忽略
//   F <frameIndex> <tsMs> <detectionRan:0|1>              ← 开始一帧
//   D <x> <y> <w> <h> <authenticated:0|1> <identityId>    ← 该帧的一个检测
//
// 约定（解析器逐条强制，畸形输入报错而非静默接受）：
//   * 一个 F 之后、下一个 F 之前的全部 D 都属于该帧；D 出现在任何 F 之前是错误。
//   * 每行**不允许多余字段**（尾随字段即报错，避免拼写错误被静默吞掉）。
//   * identityId 为不含空白的单个 token；**未认证**检测写 `-`。
//   * `authenticated=1` **必须**带真实身份串，写 `-` 一律拒绝——见下方「三类必须拒绝的输入」。
//   * `authenticated=0` **不得**携带身份串，须写 `-`——见下方「三类必须拒绝的输入」。
//   * **同一帧内不允许两个完全相同的 bbox**——见下方「三类必须拒绝的输入」。
//   * 容忍行尾 \r（Windows 换行）。
//
// **三类必须拒绝的输入**（都不是洁癖，每一类都会安静地产出「看似合理但错误」的指标）：
//   1. `D … 1 -`（已认证却无身份）：协调器会给新轨迹置 `stableId = ""`（`TrackCoordinator.cpp:135`），
//      而空串 **≠ "Unknown"**，于是被本文件「按非 Unknown 即真实身份」的各处以空串键接住。
//      **已实测**（构造 6 帧、含两处空身份）：`trackIds` 出现空串键且计数为 2、`rebuilds=1`、
//      `duplicateIdCases=1`。而 `firstConfirmSteps` 与 `neverConfirmed` **不受影响**——因为
//      `firstSeenStep.emplace` 另有 `!identityId.empty()` 守卫，空键根本不进基准表。
//      不写全这条边界是危险的：宁可报错，也不让一个不存在的"人"混进读数。
//   2. `D … 0 alice`（未认证却携带身份串）：反向矛盾同样说明导出器有 bug；静默丢弃只会掩盖它。
//   3. 同帧重复 bbox：真值归属只能取其一，是**未定义**的（哪个算真值？），会让 `staleIdentityFrames`
//      静默偏向某一侧。真实检测器经 NMS 后不会产出这种输入，故拒绝不会误伤。
//
// **「首次确认耗时」以「回放条目步数」计**：每条 F 记录 = 驱动一帧。若导出时对帧做了抽稀，
// 该值即「抽稀后的帧数」而非真实视频帧差——导出时须一并记录抽稀率，否则读数会偏大。
struct ScriptedDetection {
    cv::Rect bbox;
    bool authenticated = false;
    std::string identityId;  // 仅 authenticated 时有效；未认证为空
};

struct ScriptedFrame {
    int frameIndex = 0;
    long long tsMs = 0;
    bool detectionRan = true;
    std::vector<ScriptedDetection> detections;
};

using ReplayScript = std::vector<ScriptedFrame>;

// mock OpenCV 的 cv::Rect_ **没有 operator==**（真实 OpenCV 有），故按字段比较，
// 使本文件能在 RK_SKIP_OPENCV 的 core_unit_tests 下编译。
bool sameRect(const cv::Rect& a, const cv::Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

std::string trimmed(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// 解析失败一律带**行号 + 期望格式**返回 false，绝不部分接受——半解析的序列会产出
// 看似合理但错误的指标，比直接失败更危险。
bool parseReplayScript(std::istream& in, ReplayScript& out, std::string& err) {
    out.clear();
    err.clear();
    std::string line;
    int lineNo = 0;
    std::size_t currentFrame = 0;
    bool hasFrame = false;

    while (std::getline(in, line)) {
        ++lineNo;
        const std::string text = trimmed(line);
        if (text.empty() || text[0] == '#') continue;

        std::istringstream ls(text);
        char tag = 0;
        ls >> tag;

        if (tag == 'F') {
            ScriptedFrame f;
            int ran = 1;
            if (!(ls >> f.frameIndex >> f.tsMs >> ran)) {
                err = "第 " + std::to_string(lineNo) +
                      " 行：F 记录格式应为 'F <frameIndex> <tsMs> <detectionRan:0|1>'";
                return false;
            }
            if (ran != 0 && ran != 1) {
                err = "第 " + std::to_string(lineNo) + " 行：detectionRan 只能是 0 或 1";
                return false;
            }
            std::string extra;
            if (ls >> extra) {
                err = "第 " + std::to_string(lineNo) + " 行：F 记录有多余字段 '" + extra + "'";
                return false;
            }
            f.detectionRan = (ran == 1);
            out.push_back(f);
            currentFrame = out.size() - 1;
            hasFrame = true;
        } else if (tag == 'D') {
            if (!hasFrame) {
                err = "第 " + std::to_string(lineNo) + " 行：D 记录出现在任何 F 记录之前";
                return false;
            }
            ScriptedDetection d;
            int auth = 0;
            if (!(ls >> d.bbox.x >> d.bbox.y >> d.bbox.width >> d.bbox.height >> auth >>
                  d.identityId)) {
                err = "第 " + std::to_string(lineNo) +
                      " 行：D 记录格式应为 'D <x> <y> <w> <h> <authenticated:0|1> <identityId>'";
                return false;
            }
            if (auth != 0 && auth != 1) {
                err = "第 " + std::to_string(lineNo) + " 行：authenticated 只能是 0 或 1";
                return false;
            }
            std::string extra;
            if (ls >> extra) {
                err = "第 " + std::to_string(lineNo) + " 行：D 记录有多余字段 '" + extra + "'";
                return false;
            }
            d.authenticated = (auth == 1);
            if (d.identityId == "-") d.identityId.clear();  // "-" 是「无身份」的规范编码
            // 拒绝①：已认证却无身份。协调器会给新轨迹置 stableId = ""（TrackCoordinator.cpp:135），
            // 空串 != "Unknown" → 会被本文件的指标当成**真实身份**。宁可报错也不让幽灵混进读数。
            if (d.authenticated && d.identityId.empty()) {
                err = "第 " + std::to_string(lineNo) +
                      " 行：authenticated=1 必须带真实身份串（不得写 '-'）";
                return false;
            }
            // 拒绝②：未认证却携带身份串。反向矛盾同样说明导出器有 bug，静默丢弃会掩盖它。
            if (!d.authenticated && !d.identityId.empty()) {
                err = "第 " + std::to_string(lineNo) +
                      " 行：未认证检测（authenticated=0）不得携带身份串，请写 '-'";
                return false;
            }
            // 拒绝③：同帧重复 bbox。真值归属只能取其一，是**未定义**的，会让 staleIdentityFrames
            // 静默偏向某一侧。真实检测器经 NMS 后不会产出这种输入。
            for (const auto& prev : out[currentFrame].detections) {
                if (sameRect(prev.bbox, d.bbox)) {
                    err = "第 " + std::to_string(lineNo) +
                          " 行：同一帧内出现重复 bbox，真值身份归属无法定义";
                    return false;
                }
            }
            out[currentFrame].detections.push_back(d);
        } else {
            err = "第 " + std::to_string(lineNo) + " 行：未知记录类型 '" + std::string(1, tag) +
                  "'（只支持 F / D）";
            return false;
        }
    }

    if (out.empty()) {
        err = "检测序列为空（至少需要一条 F 记录）";
        return false;
    }
    return true;
}

void writeReplayScript(std::ostream& os, const ReplayScript& script) {
    for (const auto& f : script) {
        os << "F " << f.frameIndex << " " << f.tsMs << " " << (f.detectionRan ? 1 : 0) << "\n";
        for (const auto& d : f.detections) {
            os << "D " << d.bbox.x << " " << d.bbox.y << " " << d.bbox.width << " "
               << d.bbox.height << " " << (d.authenticated ? 1 : 0) << " "
               << (d.identityId.empty() ? std::string("-") : d.identityId) << "\n";
        }
    }
}

bool sameScript(const ReplayScript& a, const ReplayScript& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].frameIndex != b[i].frameIndex) return false;
        if (a[i].tsMs != b[i].tsMs) return false;
        if (a[i].detectionRan != b[i].detectionRan) return false;
        if (a[i].detections.size() != b[i].detections.size()) return false;
        for (std::size_t j = 0; j < a[i].detections.size(); ++j) {
            const auto& x = a[i].detections[j];
            const auto& y = b[i].detections[j];
            if (!sameRect(x.bbox, y.bbox)) return false;
            if (x.authenticated != y.authenticated) return false;
            if (x.identityId != y.identityId) return false;
        }
    }
    return true;
}

// ── 指标 ─────────────────────────────────────────────────────────────────────
//
// 原方案把「串轨次数」定义为「同一 trackId 下 stableId 变化次数」。**该定义在当前实现下
// 结构性恒为 0**：TrackCoordinator::update 只在 stableId == "Unknown" 时才写入身份
//（「不同身份不覆盖」），唯一会清掉身份的是 resetIdentitiesExcept（回放不触发）。
// 更关键的是——真实的串人（alice 离场、bob 走到同一位置）**根本不会让 stableId 变化**，
// 因为身份是黏的：框已经框住 bob，报的还是 alice。故「变化次数」既恒零又漏报。
//
// 因此本载体这样处理：
//   * identityRewrites  —— 保留原定义，但按**不变量**使用（断言恒 0，并提供 RED 判据）；
//   * staleIdentityFrames —— 改用**真值口径**度量真正的串人：输出框在当前帧的脚本里若有
//     同位置检测，该检测的身份就是真值；二者不一致即「框下面的人不是报的那个人」。
//     这是离线回放相对真机目视的核心优势——脚本自带 ground truth。
//
// 另需注意：轨迹在「已确认 + 缺失 1 帧」时会续航输出**上一帧的框**，此时本帧脚本里没有
// 同位置检测。这类帧记入 unattributedFrames 而**不计**串人——否则会把续航误报成身份错误。
struct ReplayMetrics {
    int frames = 0;                                // 回放帧数
    std::map<std::string, int> firstConfirmSteps;  // 身份 → 首次对外显示距其首现的步数
    std::map<std::string, int> trackIdsCarried;    // 身份 → 承载过它的不同 trackId 数
    int rebuilds = 0;             // Σ(承载数 − 1)：同一人的轨迹被重建的次数。
                                  // **并发重复轨迹同样计入**（该情形另由 duplicateIdCases 单独度量）。
    int identityRewrites = 0;     // 同一 trackId 的报告身份在两个非 Unknown 值间跳变（不变量）
    int staleIdentityFrames = 0;  // 真值口径的串人帧数
    int duplicateIdCases = 0;     // 同一帧同一非 Unknown 身份出现在多个 trackId 上（重复 ID）
    int unattributedFrames = 0;   // 输出框在本帧无同位置检测（缺失期续航）
    int neverConfirmed = 0;       // 脚本中出现过、但整段回放从未被对外显示的身份数
};

ReplayMetrics runReplay(const ReplayScript& script,
                        const pipeline::TrackConfig& cfg = pipeline::TrackConfig{}) {
    pipeline::TrackCoordinator tc(cfg);
    ReplayMetrics m;
    m.frames = static_cast<int>(script.size());

    std::map<std::string, int> firstSeenStep;
    std::map<int, std::string> lastReportedId;      // trackId → 上一次对外报告的身份
    std::map<std::string, std::set<int>> carried;   // 身份 → 承载过它的 trackId 集合
    std::set<std::string> confirmed;

    for (int step = 0; step < m.frames; ++step) {
        const ScriptedFrame& frame = script[static_cast<std::size_t>(step)];

        std::vector<pipeline::DetectedFace> faces;
        faces.reserve(frame.detections.size());
        for (const auto& d : frame.detections) {
            pipeline::DetectedFace df;
            df.bbox = d.bbox;
            df.isAuthenticated = d.authenticated;
            df.identityId = d.authenticated ? d.identityId : std::string();
            df.confidence = d.authenticated ? 0.9f : 0.0f;
            faces.push_back(df);
            if (d.authenticated && !d.identityId.empty()) {
                firstSeenStep.emplace(d.identityId, step);  // 只记首次
            }
        }

        const auto views = tc.update(faces, frame.tsMs, frame.detectionRan);

        std::map<std::string, int> idOccurrences;
        for (const auto& v : views) {
            if (v.stableId != "Unknown") {
                idOccurrences[v.stableId] += 1;
                carried[v.stableId].insert(v.trackId);
                if (confirmed.insert(v.stableId).second) {
                    // 基准必存在：视图的 stableId 只能来自脚本中的已认证检测。若某天找不到，
                    // 说明实现漂移——此时**不记录**该身份（读数缺键），而不是写入一个有歧义的 0
                    //（0 与「首现即显示」无法区分）。
                    const auto it = firstSeenStep.find(v.stableId);
                    if (it != firstSeenStep.end()) {
                        m.firstConfirmSteps[v.stableId] = step - it->second;
                    }
                }
            }

            // 身份改写（原方案的「串轨」定义）。trackId 永不复用——TrackCoordinator 刻意
            // 不回退 nextTrackId_——故用整段历史比对成立，不会把「新轨迹恰好拿到旧号」误判。
            const auto prev = lastReportedId.find(v.trackId);
            if (prev != lastReportedId.end() && prev->second != "Unknown" &&
                v.stableId != "Unknown" && prev->second != v.stableId) {
                m.identityRewrites += 1;
            }
            lastReportedId[v.trackId] = v.stableId;

            // 串人（真值口径）：未认证检测的真值身份视作 "Unknown"，故「alice 的框盖住一个
            // 陌生人」同样计入，而不是只比对两个已认证身份。
            const ScriptedDetection* truth = nullptr;
            for (const auto& d : frame.detections) {
                if (sameRect(d.bbox, v.bbox)) {
                    truth = &d;
                    break;
                }
            }
            if (truth == nullptr) {
                m.unattributedFrames += 1;
            } else {
                const std::string truthId =
                    truth->authenticated ? truth->identityId : std::string("Unknown");
                if (truthId != v.stableId) m.staleIdentityFrames += 1;
            }
        }
        for (const auto& kv : idOccurrences) {
            if (kv.second >= 2) m.duplicateIdCases += 1;
        }
    }

    for (const auto& kv : carried) {
        m.trackIdsCarried[kv.first] = static_cast<int>(kv.second.size());
        m.rebuilds += static_cast<int>(kv.second.size()) - 1;
    }
    for (const auto& kv : firstSeenStep) {
        if (confirmed.find(kv.first) == confirmed.end()) m.neverConfirmed += 1;
    }
    return m;
}

std::string formatMetrics(const ReplayMetrics& m) {
    std::ostringstream os;
    os << "frames=" << m.frames << " rebuilds=" << m.rebuilds
       << " identityRewrites=" << m.identityRewrites
       << " staleIdentityFrames=" << m.staleIdentityFrames
       << " duplicateIdCases=" << m.duplicateIdCases
       << " unattributedFrames=" << m.unattributedFrames << " neverConfirmed=" << m.neverConfirmed;

    auto appendMap = [&os](const char* label, const std::map<std::string, int>& kv) {
        if (kv.empty()) return;
        os << " " << label << "=";
        bool first = true;
        for (const auto& e : kv) {
            if (!first) os << ",";
            os << e.first << ":" << e.second;
            first = false;
        }
    };
    appendMap("firstConfirm", m.firstConfirmSteps);
    appendMap("trackIds", m.trackIdsCarried);
    return os.str();
}

std::string readWholeFile(const std::string& path, bool& ok) {
    std::ifstream in(path.c_str(), std::ios::binary);
    ok = static_cast<bool>(in);
    if (!ok) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeReplayReport(const std::string& path, const ReplayScript& script,
                       const ReplayMetrics& m, std::string& err) {
    err.clear();
    const std::filesystem::path p(path);
    std::error_code ec;
    if (!p.parent_path().empty()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            err = "无法创建报告目录: " + p.parent_path().string() + "（" + ec.message() + "）";
            return false;
        }
    }
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "无法写入报告: " + path;
        return false;
    }
    out << "# 轨迹回放报告（任务 C1 离线载体）\n\n";
    out << "- 回放帧数: " << script.size() << "\n";
    out << "- 汇总指标: `" << formatMetrics(m) << "`\n\n";
    out << "指标含义与局限见 tests/cpp/test_track_replay.cpp 顶部注释。\n";
    out << "注意：首次确认耗时以**回放条目步数**计；导出时若抽稀过帧，须一并记录抽稀率。\n";
    out.flush();
    if (!out) {
        err = "报告写入失败（磁盘/权限）: " + path;
        return false;
    }
    out.close();
    if (!out) {
        err = "报告关闭失败（写入可能未完整落盘）: " + path;
        return false;
    }
    return true;
}

// 取指标（缺失返回 fallback）。用 find 而非 std::map::at 是为了让「缺键」显式可见：
// 返回 -1 能表达「该身份根本没出现在读数中」，而 at() 会抛异常打断整个用例。
// （注：本 target 并不会被加 -fno-exceptions——core_unit_tests 在 cmake/core.cmake 的
//  RK_SAFETY_EXCLUDED 内，故此处**不是**为规避异常，纯为语义清晰。）
int metricOf(const std::map<std::string, int>& kv, const std::string& key, int fallback = -1) {
    const auto it = kv.find(key);
    return (it == kv.end()) ? fallback : it->second;
}

// ── 脚本构造辅助 ──
ScriptedDetection det(int x, int y, int w, int h, const std::string& id) {
    ScriptedDetection d;
    d.bbox = cv::Rect(x, y, w, h);
    d.authenticated = !id.empty();
    d.identityId = id;
    return d;
}

ScriptedFrame frameAt(int index, long long ts, std::vector<ScriptedDetection> dets,
                      bool ran = true) {
    ScriptedFrame f;
    f.frameIndex = index;
    f.tsMs = ts;
    f.detectionRan = ran;
    f.detections = std::move(dets);
    return f;
}

// 临时目录名含单调时钟读数：曾有审计代理与本会话**同时**运行同一二进制，固定子目录名会互踩。
std::filesystem::path makeTempDir(const std::string& name) {
    const long long stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / "rk3288_opencv_track_replay" /
           (name + "_" + std::to_string(stamp));
}

// 临时目录 RAII：即使断言提前失败也删除，不在系统临时目录留残留。
struct TempDirGuard {
    std::filesystem::path path;
    explicit TempDirGuard(std::filesystem::path p) : path(std::move(p)) {}
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace

// ── 基线：单人静止 ───────────────────────────────────────────────────────────
// stableFrames=3 → 首现后第 2 步才对外显示（首现帧本身算第 1 次命中）。该基线把
// 「首次确认耗时」的 0 点钉死，后续所有场景的读数都以它为参照。
TEST(TrackReplay, StationaryTargetConfirmsOnThirdFrame) {
    ReplayScript s;
    for (int i = 0; i < 3; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100, {det(200, 150, 160, 200, "alice")}));
    }
    const auto m = runReplay(s);

    EXPECT_EQ(m.frames, 3);
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 2) << "stableFrames=3 → 首现后第 2 步显示";
    EXPECT_EQ(metricOf(m.trackIdsCarried, "alice"), 1);
    EXPECT_EQ(m.rebuilds, 0);
    EXPECT_EQ(m.identityRewrites, 0);
    EXPECT_EQ(m.staleIdentityFrames, 0);
    EXPECT_EQ(m.duplicateIdCases, 0);
    EXPECT_EQ(m.unattributedFrames, 0);
    EXPECT_EQ(m.neverConfirmed, 0);
}

// ── 阈值内位移：可持续确认（0.8 的下界证据）────────────────────────────────
// 79px/帧 = 0.79×框宽，恰在 centerMaxRatio=0.8 之内。IoU 仅 0.117（远低于 0.3），
// 故全程只能靠中心距离回退匹配——这正是 0.8 阈值存在的意义。
//
// **与既有用例的关系（A1 审计 Low-3，如实说明）**：本用例与下面那条「超阈值」用例，其**场景**
// 与 test_track_coordinator.cpp 的 ContinuouslyMovingTargetCompletesInitialConfirmation /
// DisplacementBeyondFallbackRatioNeverConfirms 重复（后者还同名，仅 suite 不同）。此处**刻意保留**：
// 既有用例断言的是轨迹数/严格空（跟踪规则本身），而这两条固定的是**指标读数基线**——载体能否
// 正确读出「2 步确认」与「永不确认」，是后续用真实素材做阈值扫描时的对照锚点。不重复验证跟踪规则。
TEST(TrackReplay, ContinuousDisplacementWithinFallbackRatioConfirms) {
    ReplayScript s;
    for (int i = 0; i < 4; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100, {det(i * 79, 0, 100, 100, "alice")}));
    }
    const auto m = runReplay(s);

    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 2) << "0.8 之内应能完成首次确认";
    EXPECT_EQ(metricOf(m.trackIdsCarried, "alice"), 1) << "不应每帧重建轨迹";
    EXPECT_EQ(m.rebuilds, 0);
    EXPECT_EQ(m.neverConfirmed, 0);
    EXPECT_EQ(m.staleIdentityFrames, 0);
}

// ── 超阈值位移：永不显示（0.8 的上界证据）──────────────────────────────────
// 81px/帧 = 0.81×框宽，IoU（0.105）与中心距离回退**双双失配**：每帧新建轨迹，而新轨迹
// 未确认（confirmFrames=1 < 3）在下次实际检测缺失时即删，故永远达不到 stableFrames、
// 对外恒不显示。这是「0.8 在多大单帧位移下失效」的可执行判据。
// 注：这是**条件性**结果（取决于单帧位移与框尺寸之比），不是所有大位移场景的必然结局。
TEST(TrackReplay, DisplacementBeyondFallbackRatioNeverConfirms) {
    ReplayScript s;
    for (int i = 0; i < 6; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100, {det(i * 81, 0, 100, 100, "alice")}));
    }
    const auto m = runReplay(s);

    EXPECT_TRUE(m.firstConfirmSteps.empty()) << "超出回退上限时不应有任何身份被对外显示";
    EXPECT_EQ(m.neverConfirmed, 1) << "alice 出现 6 帧却全程未被确认";
    EXPECT_TRUE(m.trackIdsCarried.empty()) << "从未显示 → 没有任何身份被轨迹承载过";
    EXPECT_EQ(m.rebuilds, 0);
    EXPECT_EQ(m.staleIdentityFrames, 0);
    EXPECT_EQ(m.duplicateIdCases, 0);
}

// ── 同位置换人：旧身份黏住、新人永不被显示（串人的真值口径）───────────────
// alice 先在 P 处被确认，随后 bob 出现在**同一位置**。三条读数合起来给出设备验收清单
// 「不串人」条款在离线回放下的可量化证据：
//   * identityRewrites == 0 —— 轨迹身份不因新检测而改写（只允许 Unknown → 身份 的赋值）。
//     **这条是刻意钉住的不变量**：RED 判据是把 TrackCoordinator.cpp 里
//     `if (t.view.stableId == "Unknown")` 的守卫换成无条件赋值，本例立即变红。
//   * staleIdentityFrames == 3 —— 代价是：bob 站在框里，报告的身份**仍是 alice**。
//   * neverConfirmed == 1 —— bob 因此**永远得不到自己的轨迹**，也就永远不会被显示。
// 说明：这是「不同身份不覆盖」这一**刻意取舍**的既有行为（防单帧误识别抢走已确立的身份），
// 本用例只**度量其代价**，不主张其为缺陷——是否调整属产品决策，需另行确认。
TEST(TrackReplay, SamePositionHandoverKeepsStaleIdentityAndStarvesNewcomer) {
    ReplayScript s;
    for (int i = 0; i < 3; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100, {det(200, 150, 160, 200, "alice")}));
    }
    for (int i = 3; i < 6; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100, {det(200, 150, 160, 200, "bob")}));
    }
    const auto m = runReplay(s);

    EXPECT_EQ(m.identityRewrites, 0) << "身份不得在两个非 Unknown 值之间改写（不变量）";
    EXPECT_EQ(m.staleIdentityFrames, 3) << "bob 占位 3 帧，报告身份仍为 alice";
    EXPECT_EQ(m.neverConfirmed, 1) << "bob 永远得不到自己的轨迹";
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "bob"), -1) << "bob 不应出现在对外输出中";
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 2);
    EXPECT_EQ(m.duplicateIdCases, 0);
}

// ── 两人并行交错：阈值的区分力（不串轨的另一半）─────────────────────────────
// 两人在**不同 y 带**相向而行，竖直中心间距恒为 200 > centerMaxRatio×100 = 80。
// 无论水平位置如何交错，中心距离判据都不可能把检测配到对面的轨迹上（IoU 因 y 不重叠恒为 0）。
// 这条与「阈值内位移可确认」合起来说明 0.8 的取舍是成立的：**松到足以确认 70px/帧的
// 移动目标，又紧到不会把相距 200px 的两个人并到一条轨迹**。
TEST(TrackReplay, TwoParallelFacesDoNotMergeAcrossFallbackRatio) {
    ReplayScript s;
    for (int i = 0; i < 5; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100,
                            {det(i * 70, 0, 100, 100, "alice"),
                             det(520 - i * 70, 200, 100, 100, "bob")}));
    }
    const auto m = runReplay(s);

    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 2);
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "bob"), 2);
    EXPECT_EQ(metricOf(m.trackIdsCarried, "alice"), 1);
    EXPECT_EQ(metricOf(m.trackIdsCarried, "bob"), 1);
    EXPECT_EQ(m.rebuilds, 0);
    EXPECT_EQ(m.staleIdentityFrames, 0) << "上下两带不得互相串轨";
    EXPECT_EQ(m.duplicateIdCases, 0);
    EXPECT_EQ(m.neverConfirmed, 0);
}

// ── 短暂缺失：续航帧不得被误判为串人 ────────────────────────────────────────
// 已确认轨迹缺 1 帧仍保留并输出**上一帧的框**——该帧脚本里没有同位置检测，故记入
// unattributedFrames（诊断项）而非 staleIdentityFrames。缺 2 帧删除。
// 这条守住的是指标本身的可信度：缺了它，串人计数会把正常续航算进去而虚高。
TEST(TrackReplay, CoastingFramesAreNotCountedAsCrossover) {
    ReplayScript s;
    for (int i = 0; i < 3; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100, {det(200, 150, 160, 200, "alice")}));
    }
    s.push_back(frameAt(3, 1300, {}));  // 实际空检测：缺 1 → 保留
    s.push_back(frameAt(4, 1400, {}));  // 缺 2 → 删除
    const auto m = runReplay(s);

    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 2);
    EXPECT_EQ(m.unattributedFrames, 1) << "缺 1 帧时续航输出，该帧计入未归属";
    EXPECT_EQ(m.staleIdentityFrames, 0) << "续航帧不是串人";
    EXPECT_EQ(m.duplicateIdCases, 0);
    EXPECT_EQ(m.rebuilds, 0);
}

// ── 重复 ID：同一身份在同一帧占据两条轨迹的**非零**分支 ──────────────────────
// A1 审计 Med-2：duplicateIdCases 在本套其余用例里只被断言过 ==0，**非零分支从未触发**——
// 于是「它能不能响」从未被证明过。一个从不响的警报，与一个坏掉的警报，在证据上没有区别。
// 构造：alice 的轨迹先在一处确认；随后**同一帧**又出现一个远处、同样被判为 alice 的检测，
// 后者另建轨迹并累计到 stableFrames，两条轨迹遂在同一帧同时对外报 alice。
TEST(TrackReplay, DuplicateIdentityOnTwoTracksIsCounted) {
    ReplayScript s;
    for (int i = 0; i < 3; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100, {det(0, 0, 100, 100, "alice")}));
    }
    for (int i = 3; i < 6; ++i) {  // 同帧两处 alice：一处续在原轨迹，一处另建
        s.push_back(frameAt(i, 1000 + i * 100,
                            {det(0, 0, 100, 100, "alice"),
                             det(600, 400, 100, 100, "alice")}));
    }
    const auto m = runReplay(s);

    EXPECT_EQ(m.duplicateIdCases, 1) << "第 5 步两条轨迹同时对外报 alice，即一次重复 ID 违规";
    EXPECT_EQ(metricOf(m.trackIdsCarried, "alice"), 2);
    EXPECT_EQ(m.rebuilds, 1) << "同一身份由两条轨迹承载（rebuilds 计数含并发重复）";
    EXPECT_EQ(m.staleIdentityFrames, 0) << "两处检测的真值都是 alice，不构成串人";
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 2);
    EXPECT_EQ(m.neverConfirmed, 0);
}

// ── 轨迹重建：同一人离场后**时序上**重建轨迹的非零分支（A1 Med-2 的另一半）──────
// 与上一条不同，这里是时序重建：alice 的轨迹因连续 2 次实际缺失被删，之后她再出现，
// 协调器**刻意不复用 trackId**（nextTrackId_ 不回退）→ 新轨迹拿到新号。
// 对应验收清单里的「新 ID 合理」：同一个人在一段回放里被分配了几次 ID。
TEST(TrackReplay, SequentialTrackRebuildAfterExpiryIsCounted) {
    ReplayScript s;
    for (int i = 0; i < 3; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100, {det(200, 150, 160, 200, "alice")}));
    }
    s.push_back(frameAt(3, 1300, {}));  // 缺 1：已确认轨迹续航一次
    s.push_back(frameAt(4, 1400, {}));  // 缺 2：删除
    for (int i = 5; i < 8; ++i) {       // alice 回来 → 新轨迹
        s.push_back(frameAt(i, 1000 + i * 100, {det(200, 150, 160, 200, "alice")}));
    }
    const auto m = runReplay(s);

    EXPECT_EQ(metricOf(m.trackIdsCarried, "alice"), 2) << "重建后应使用新 trackId（不复用旧号）";
    EXPECT_EQ(m.rebuilds, 1) << "同一人一次时序重建";
    EXPECT_EQ(m.duplicateIdCases, 0) << "新旧轨迹未同时对外输出，不构成重复 ID";
    EXPECT_EQ(m.unattributedFrames, 1) << "仅缺 1 的那一帧续航输出（缺 2 时轨迹已被删）";
    EXPECT_EQ(m.staleIdentityFrames, 0) << "续航帧不是串人";
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 2) << "首次确认仍为 2 步（重建不重置首现基准）";
    EXPECT_EQ(m.neverConfirmed, 0);
}

// ── 载体可用性：脚本文件往返 + 报告落地 ─────────────────────────────────────
// 证明「外部素材能走同一条回放路径」：内存脚本 → 文本 → 解析 → 与内存脚本逐字段一致
// → 回放指标逐字相同。没有这条，「文件驱动的载体」就只是声称。
TEST(TrackReplay, ScriptFileRoundTripsAndReplaysIdentically) {
    ReplayScript s;
    for (int i = 0; i < 4; ++i) {
        s.push_back(frameAt(2 + i * 3, 1000 + i * 100,  // 稀疏帧号：载体按条目步数计，不受影响
                            {det(i * 70, 0, 100, 100, "alice"),
                             det(400, 300 - i * 40, 120, 120, "")}));  // 未认证
    }

    const std::filesystem::path dir = makeTempDir("roundtrip");
    TempDirGuard guard(dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const std::string scriptPath = (dir / "sequence.txt").string();
    {
        std::ofstream out(scriptPath.c_str(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out);
        writeReplayScript(out, s);
    }

    std::ifstream in(scriptPath.c_str());
    ASSERT_TRUE(in);
    ReplayScript parsed;
    std::string err;
    ASSERT_TRUE(parseReplayScript(in, parsed, err)) << err;
    EXPECT_TRUE(sameScript(s, parsed)) << "序列化/解析必须无损";

    // 指标须与内存脚本逐字相同（formatMetrics 是该结构的全量文本投影）。
    const auto mMemory = runReplay(s);
    const auto mFile = runReplay(parsed);
    EXPECT_EQ(formatMetrics(mMemory), formatMetrics(mFile));

    // 报告可落盘且内容含汇总指标。
    const std::string reportPath = (dir / "report.md").string();
    ASSERT_TRUE(writeReplayReport(reportPath, parsed, mFile, err)) << err;
    bool ok = false;
    const std::string body = readWholeFile(reportPath, ok);
    ASSERT_TRUE(ok) << "报告应可读回";
    EXPECT_NE(body.find(formatMetrics(mFile)), std::string::npos) << body;
    EXPECT_NE(body.find("回放帧数: 4"), std::string::npos) << body;
}

// ── 解析器必须拒绝畸形输入（否则半解析的序列会产出看似合理的错误指标）──────
TEST(TrackReplay, ParserRejectsMalformedInput) {
    auto expectReject = [](const std::string& text, const char* why) {
        std::istringstream in(text);
        ReplayScript s;
        std::string err;
        EXPECT_FALSE(parseReplayScript(in, s, err)) << "应拒绝：" << why;
        EXPECT_FALSE(err.empty()) << "拒绝时必须给出原因：" << why;
    };

    expectReject("D 1 2 3 4 1 alice\n", "D 出现在 F 之前");
    expectReject("F 0 1000\n", "F 字段不足");
    expectReject("F 0 1000 2\n", "detectionRan 非 0/1");
    expectReject("F 0 1000 1 x\n", "F 多余字段");
    expectReject("F 0 1000 1\nD 1 2 3 4 alice\n", "D 字段不足");
    expectReject("F 0 1000 1\nD 1 2 3 4 1 alice extra\n", "D 多余字段");
    expectReject("F 0 1000 1\nD 1 2 3 4 7 alice\n", "authenticated 非 0/1");
    // 下面三类是「会安静产出错误指标」的输入，必须拒绝而非静默规范化（见文件头契约）
    expectReject("F 0 1000 1\nD 1 2 3 4 1 -\n", "已认证却无身份（会被当作真实身份 stableId=\"\"）");
    expectReject("F 0 1000 1\nD 1 2 3 4 0 alice\n", "未认证却携带身份串");
    expectReject("F 0 1000 1\nD 1 2 3 4 1 alice\nD 1 2 3 4 1 bob\n", "同帧重复 bbox");
    expectReject("X 1 2 3\n", "未知记录类型");
    expectReject("# 只有注释\n\n", "空序列");
    expectReject("", "空输入");

    // 合法输入（含 CRLF 与注释/空行）必须被接受——避免解析器严苛到不可用。
    std::istringstream good(
        "# 注释行\r\n"
        "\r\n"
        "F 0 1000 1\r\n"
        "D 10 20 30 40 1 alice\r\n"
        "  # 缩进的注释\r\n"
        "D 50 60 70 80 0 -\r\n"
        "F 1 1100 0\r\n");
    ReplayScript s;
    std::string err;
    ASSERT_TRUE(parseReplayScript(good, s, err)) << err;
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].detections.size(), 2u);
    EXPECT_TRUE(s[0].detections[0].authenticated);
    EXPECT_EQ(s[0].detections[0].identityId, "alice");
    EXPECT_FALSE(s[0].detections[1].authenticated);
    EXPECT_TRUE(s[0].detections[1].identityId.empty());
    EXPECT_TRUE(s[0].detectionRan);
    EXPECT_FALSE(s[1].detectionRan);
}

// ── 载体入口：把真实素材交给同一条回放路径并落报告 ──────────────────────────
// 用法：导出检测序列后，设 RK_TRACK_REPLAY_FILE 指向它并运行 core_unit_tests，
// 报告写到 tests/reports/track_replay_external.md。
// 未设环境变量时**显式 SKIP**（而非静默通过）——避免「零执行的用例显示为绿」。代价是
// 「已设置」那一分支在 CI 中不被执行；该分支的全部实质逻辑（解析→回放→落盘）已由
// ScriptFileRoundTripsAndReplaysIdentically 覆盖，此处仅剩 getenv 与路径拼接。
TEST(TrackReplay, ExternalSequenceFileProducesReport) {
    const char* path = std::getenv("RK_TRACK_REPLAY_FILE");
    if (path == nullptr || *path == '\0') {
        GTEST_SKIP() << "未设置 RK_TRACK_REPLAY_FILE（真实素材的检测序列），跳过外部回放";
    }

    std::ifstream in(path);
    ASSERT_TRUE(in) << "无法打开检测序列文件: " << path;
    ReplayScript script;
    std::string err;
    ASSERT_TRUE(parseReplayScript(in, script, err)) << err;

    const auto m = runReplay(script);

#if defined(RK_TEST_SOURCE_ROOT)
    const std::string reportPath =
        std::string(RK_TEST_SOURCE_ROOT) + "/tests/reports/track_replay_external.md";
#else
    const std::string reportPath = "track_replay_external.md";
#endif
    ASSERT_TRUE(writeReplayReport(reportPath, script, m, err)) << err;
    std::cout << "[track-replay] " << path << "\n  " << formatMetrics(m) << "\n  → " << reportPath
              << std::endl;
}
