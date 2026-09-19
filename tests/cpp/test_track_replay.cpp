#include "pipeline/ReplayScriptWriter.h"
#include "pipeline/TrackCoordinator.h"

#include <gtest/gtest.h>

#include <algorithm>
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
// 会在检测序列**之后**改写 coordinator 的输入或输出。本载体**已把三种改写显式建模为 `@` 指令**
// （格式见下节），不再假定它们不存在：
//   * `Engine.cpp:1292-1302`（trackFaces 内）读路径**代次作废**——图库在「读库」与「提交轨迹」
//     之间被替换时，把本帧**全部**检测的 isAuthenticated 降级为 false、identityId 改 "Unknown"
//     **后再交给** update()。这改的是 **coordinator 的输入**，故直接影响 firstConfirmSteps。
//     用 `@ stale` 指令建模（作用于当前 F 帧）。
//   * `Engine.cpp:853-869`（revokeTrackIdentitiesLocked，由 applyGalleryUpdate:845 调用）
//     **身份撤销**——`trackCoordinator_->resetIdentitiesExcept(keepIds)` 把不在图库有效集内的轨迹
//     身份就地清为 "Unknown"（**保留轨迹本体/bbox/确认计数**）。用 `@ revoke <keep>` 建模（帧间事件）。
//   * `Engine.cpp:1098-1103`（processFrameRealtime 的 else 分支）**模型缺失**——runRealtimeDetect
//     返回 false 时**根本不调用 trackFaces**，只 `currentTracks_.clear()` 清空**对外视图**，
//     coordinator 内部轨迹**原样保留**（仅受下一次 update 的 TTL 约束）。用 `@ missing <idx> <ts>`
//     建模。**它与 `reset()`（清空内部轨迹）语义不同**，本文件有专门用例区分二者。
// 另添 `@ reset` 指令，仅用于与 `@ missing` 对照——`reset()` 非产线路径（Engine 图库变更统一走
// resetIdentitiesExcept，见 TrackCoordinator.h:75-77）。
// 另：Engine 成功路径**恒传 detectionRan=true**（`Engine.cpp:1142` 是唯一赋值点，返回 false 的
// 分支都在它之前），故脚本里的 `detectionRan=0` 模拟的是 coordinator 的「调度跳过」语义，
// **产线上没有对应路径**。（`Engine::trackFaces` 注释所称「模型缺失跳过检测时为 false」与
// `:1098-1103` 的实际行为不符，属既有陈旧注释，不在本次改动范围。）
//
// 本层的实利：① 零 OpenCV/ncnn 依赖；② target 是 core_unit_tests → Linux 与 Windows CI 都会跑；
//   ③ 不需要任何图像素材，跑上万帧仍在毫秒级。
// Engine 侧「检测→身份→coordinator」的管道正确性由 test_engine.cpp 既有用例覆盖，本文件不重复。
//
// **素材来源与已知局限（如实声明，勿当作已完成的真机验收）**：
// 本载体消费的是**检测序列**，不是视频。要从真实录像产出素材，需先用导出器
// `win_replay_export_cli`（`src/win/tools/win_replay_export_cli.cpp`，经 `pipeline/ReplayScriptWriter`
// 写的就是本文件所用的这套文本格式）把录像转成「逐帧检测框 + 身份」——该步骤**已经存在**。
// ⚠️ 但它**只用合成视频跑过**，尚未用真实人脸录像验证过，故真机验收证据**仍需用户提供录像**。
// `models/calib_images/` 的 172 帧虽在本机，但帧号稀疏（跨 2..1025，**非相邻帧**）、逐帧位移远大于
// 真实相邻帧、且未逐帧确认是否为同一目标，**不能代表常规使用**，故**不作为**验收素材。
// ══════════════════════════════════════════════════════════════════════════════

namespace {

// ── 检测序列文本格式（可由外部工具生成，供真实素材导入）──────────────────────
//
//   # 以 '#' 起始的行为注释；空行忽略
//   F <frameIndex> <tsMs> <detectionRan:0|1>              ← 开始一帧
//   D <x> <y> <w> <h> <authenticated:0|1> <identityId>    ← 该帧的一个检测
//   @ stale                                               ← 帧内代次作废（作用于当前 F 帧，见下）
//   @ revoke <keep>                                       ← 身份撤销（帧间事件，见下）
//   @ missing <frameIndex> <tsMs>                         ← 模型缺失清空对外视图（自成一帧，见下）
//   @ reset                                               ← 全清 coordinator 内部轨迹（仅作对照，见下）
//
// 指令行（`@` 前缀，刻意选一个与 F/D/# 都不冲突的前缀，便于外部导出器区分“检测数据”与“控制”）：
//   * `@ stale` —— **帧内代次作废**，须紧跟在某个 `F`（及其 D）之后，把该帧的全部检测在交给
//     update() 前降级为「未认证 + Unknown」（精确镜像 Engine 读路径代次作废的 1296-1301 行）。
//     **真值口径随之改变**：stale 帧的“真值身份”按**降级后**的检测计（图库已换，旧认证结论在新库
//     下不成立 → 该帧的真值就是 Unknown），故它不会把“保守的 fail-closed 降级”误记成串人。
//   * `@ revoke <keep>` —— **身份撤销**（帧间事件，不产生视图）。`<keep>` 为 `-`（空集，等价于清空
//     全部已报告身份）或逗号分隔的身份列表；对不在 keep 内的轨迹清 stableId（保留轨迹/bbox/确认计数）。
//   * `@ missing <idx> <ts>` —— **模型缺失**：当帧**不调用** update()，对外视图为空，但 coordinator
//     内部轨迹原样保留（下一秒真正检测恢复时若 TTL 未过，轨迹**继续沿用**，不重建）。其后**不得**
//     再接 `D` 行。与 `reset()` 的区别专门有用例证明。
//   * `@ reset` —— 调用 `TrackCoordinator::reset()`（清空内部轨迹本体）。**非产线路径**，只为把
//     「清对外视图（missing）」与「清内部轨迹（reset）」的差异做成可执行对照。
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
    // 帧内代次作废（`@ stale`）：本帧检测在交给 update() 前全部降级为「未认证 + Unknown」。
    bool staleAuth = false;
    std::vector<ScriptedDetection> detections;
};

// 回放步：脚本不再是「纯检测帧序列」，而是「检测帧 / 模型缺失帧 / 帧间控制事件」的序列。
// 这使 Engine 的三条改写路径（代次作废 / 身份撤销 / 模型缺失）能被同一套指标读出。
enum class StepKind {
    Frame,         // 一次 update() 调用（可带 staleAuth）
    ModelMissing,  // 模型缺失：不调用 update()，对外视图为空，内部轨迹保留
    Revoke,        // 身份撤销：帧间事件，不产生视图
    Reset,         // reset()：清空内部轨迹（仅作对照，非产线路径）
};

struct ReplayStep {
    StepKind kind = StepKind::Frame;
    ScriptedFrame frame;               // kind == Frame
    int frameIndex = 0;                // kind == ModelMissing
    long long tsMs = 0;                // kind == ModelMissing
    std::vector<std::string> keepIds;  // kind == Revoke
};

using ReplayScript = std::vector<ReplayStep>;

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
    std::size_t currentFrame = 0;            // 最近一个「帧类」步在 out 中的下标
    bool hasFrame = false;                   // 是否已出现过帧类步
    bool currentAcceptsDetections = false;   // 最近帧类步是否是可接 D 的检测帧（F）
    int frameLikeCount = 0;                  // 帧类步计数（F / @ missing），用于拒绝「只有指令」的空脚本

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
            ReplayStep step;
            step.kind = StepKind::Frame;
            step.frame = std::move(f);
            out.push_back(std::move(step));
            currentFrame = out.size() - 1;
            hasFrame = true;
            currentAcceptsDetections = true;
            ++frameLikeCount;
        } else if (tag == 'D') {
            if (!hasFrame || !currentAcceptsDetections) {
                err = "第 " + std::to_string(lineNo) +
                      " 行：D 记录必须紧跟在一个可承载检测的 F 记录之后"
                      "（不得出现在任何 F 之前，也不得出现在 @ missing 之后）";
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
            for (const auto& prev : out[currentFrame].frame.detections) {
                if (sameRect(prev.bbox, d.bbox)) {
                    err = "第 " + std::to_string(lineNo) +
                          " 行：同一帧内出现重复 bbox，真值身份归属无法定义";
                    return false;
                }
            }
            out[currentFrame].frame.detections.push_back(d);
        } else if (tag == '@') {
            std::string op;
            if (!(ls >> op)) {
                err = "第 " + std::to_string(lineNo) +
                      " 行：@ 指令缺少操作名（支持 stale / revoke / missing / reset）";
                return false;
            }
            if (op == "stale") {
                std::string extra;
                if (ls >> extra) {
                    err = "第 " + std::to_string(lineNo) + " 行：@ stale 不接受参数，发现多余字段 '" +
                          extra + "'";
                    return false;
                }
                // 代次作废作用于「当前检测帧的输入」，故必须有一个可接检测的 F 在前。
                if (!hasFrame || !currentAcceptsDetections) {
                    err = "第 " + std::to_string(lineNo) + " 行：@ stale 必须作用于一个 F 检测帧之后";
                    return false;
                }
                out[currentFrame].frame.staleAuth = true;
            } else if (op == "revoke") {
                std::string keep;
                if (!(ls >> keep)) {
                    err = "第 " + std::to_string(lineNo) +
                          " 行：@ revoke 缺少身份列表（'-' 表示空集）";
                    return false;
                }
                std::string extra;
                if (ls >> extra) {
                    err = "第 " + std::to_string(lineNo) + " 行：@ revoke 有多余字段 '" + extra + "'";
                    return false;
                }
                ReplayStep step;
                step.kind = StepKind::Revoke;
                if (keep != "-") {
                    // 逗号分隔。空元素 / 单独的 "-" 段一律拒绝——身份列表格式非法，宁可报错。
                    std::size_t start = 0;
                    while (true) {
                        const std::size_t comma = keep.find(',', start);
                        const std::string part =
                            keep.substr(start, comma == std::string::npos ? std::string::npos
                                                                          : comma - start);
                        if (part.empty() || part == "-") {
                            err = "第 " + std::to_string(lineNo) +
                                  " 行：@ revoke 身份列表含非法元素（空段或 '-'）";
                            return false;
                        }
                        step.keepIds.push_back(part);
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }
                out.push_back(std::move(step));  // 帧间事件：不改变 currentFrame
            } else if (op == "missing") {
                ReplayStep step;
                step.kind = StepKind::ModelMissing;
                if (!(ls >> step.frameIndex >> step.tsMs)) {
                    err = "第 " + std::to_string(lineNo) +
                          " 行：@ missing 格式应为 '@ missing <frameIndex> <tsMs>'";
                    return false;
                }
                std::string extra;
                if (ls >> extra) {
                    err = "第 " + std::to_string(lineNo) + " 行：@ missing 有多余字段 '" + extra + "'";
                    return false;
                }
                out.push_back(std::move(step));
                currentFrame = out.size() - 1;
                hasFrame = true;
                currentAcceptsDetections = false;  // 其后的 D 一律拒绝
                ++frameLikeCount;
            } else if (op == "reset") {
                std::string extra;
                if (ls >> extra) {
                    err = "第 " + std::to_string(lineNo) + " 行：@ reset 不接受参数，发现多余字段 '" +
                          extra + "'";
                    return false;
                }
                ReplayStep step;
                step.kind = StepKind::Reset;
                out.push_back(std::move(step));
            } else {
                err = "第 " + std::to_string(lineNo) + " 行：未知 @ 指令 '" + op +
                      "'（支持 stale / revoke / missing / reset）";
                return false;
            }
        } else {
            err = "第 " + std::to_string(lineNo) + " 行：未知记录类型 '" + std::string(1, tag) +
                  "'（只支持 F / D / @）";
            return false;
        }
    }

    if (frameLikeCount == 0) {
        err = "检测序列为空（至少需要一条 F 或 @ missing 帧记录）";
        return false;
    }
    return true;
}

// 刻意**不叫** writeReplayScript：库侧已有 `pipeline::writeReplayScript`（只支持 F/D，供
// 导出器 CLI 使用）。两者同名会让本文件内未限定的调用产生「命中哪一个」的读解陷阱
//（曾实测：同一文件里既有限定调用也有非限定调用，指向不同实现）。此处需支持 `@` 指令，
// 故必须是独立的一份；名字上区分开后，二者的关系由 ExporterWriterOutputIsAcceptedByRealParser
// 的逐字节往返断言钉住。
void serializeReplaySteps(std::ostream& os, const ReplayScript& script) {
    for (const auto& s : script) {
        if (s.kind == StepKind::Revoke) {
            os << "@ revoke ";
            if (s.keepIds.empty()) {
                os << "-";
            } else {
                for (std::size_t i = 0; i < s.keepIds.size(); ++i) {
                    if (i) os << ",";
                    os << s.keepIds[i];
                }
            }
            os << "\n";
            continue;
        }
        if (s.kind == StepKind::Reset) {
            os << "@ reset\n";
            continue;
        }
        if (s.kind == StepKind::ModelMissing) {
            os << "@ missing " << s.frameIndex << " " << s.tsMs << "\n";
            continue;
        }
        const ScriptedFrame& f = s.frame;
        os << "F " << f.frameIndex << " " << f.tsMs << " " << (f.detectionRan ? 1 : 0) << "\n";
        for (const auto& d : f.detections) {
            os << "D " << d.bbox.x << " " << d.bbox.y << " " << d.bbox.width << " "
               << d.bbox.height << " " << (d.authenticated ? 1 : 0) << " "
               << (d.identityId.empty() ? std::string("-") : d.identityId) << "\n";
        }
        if (f.staleAuth) os << "@ stale\n";
    }
}

bool sameScript(const ReplayScript& a, const ReplayScript& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind) return false;
        if (a[i].kind == StepKind::Revoke) {
            if (a[i].keepIds != b[i].keepIds) return false;
            continue;
        }
        if (a[i].kind == StepKind::Reset) continue;
        if (a[i].kind == StepKind::ModelMissing) {
            if (a[i].frameIndex != b[i].frameIndex) return false;
            if (a[i].tsMs != b[i].tsMs) return false;
            continue;
        }
        const ScriptedFrame& fa = a[i].frame;
        const ScriptedFrame& fb = b[i].frame;
        if (fa.frameIndex != fb.frameIndex) return false;
        if (fa.tsMs != fb.tsMs) return false;
        if (fa.detectionRan != fb.detectionRan) return false;
        if (fa.staleAuth != fb.staleAuth) return false;
        if (fa.detections.size() != fb.detections.size()) return false;
        for (std::size_t j = 0; j < fa.detections.size(); ++j) {
            const auto& x = fa.detections[j];
            const auto& y = fb.detections[j];
            if (!sameRect(x.bbox, y.bbox)) return false;
            if (x.authenticated != y.authenticated) return false;
            if (x.identityId != y.identityId) return false;
        }
    }
    return true;
}

// ── 指标 ─────────────────────────────────────────────────────────────────────
//
// 原方案把「串轨次数」定义为「同一 trackId 下 stableId 变化次数」。**在只有检测帧的脚本里该定义
// 结构性恒为 0**：TrackCoordinator::update 只在 stableId == "Unknown" 时才写入身份（「不同身份
// 不覆盖」），而唯一的清身份入口 resetIdentitiesExcept 在回放里不触发。更关键的是——真实的串人
// （alice 离场、bob 走到同一位置）**根本不会让 stableId 变化**：身份是黏的，框已框住 bob，
// 报的还是 alice。故「变化次数」既恒零又漏报。
//
// 因此本载体这样处理：
//   * identityRewrites  —— 保留原定义（同一 trackId 下**两个非 Unknown 值间**的跳变）。
//     **本文件既有用例里它仍恒 0**（那是刻意钉住的不变量），但 `@ revoke` 指令使**非零**可达：
//     撤销把已确认身份清为 Unknown 后，该 trackId 随即被**另一个**已认证身份命中时，对外报告
//     从 idA 直接跳到 idB（中途不经一次对外 Unknown），即一次真正的身份改写。见
//     RevocationAllowsIdentityRewriteAcrossSameTrack。
//     ⚠️ 注意：`idA → "Unknown" → idA` 这条路径**不会**让本计数增加——中间那次对外 Unknown 已把
//     lastReportedId 置为 "Unknown"，而计数要求前后**都**非 Unknown。故“撤销后可重新认证”只有在
//     换成**不同身份**时才计入；重新认证为同一身份不计（那是恢复，不是改写）。
//   * staleIdentityFrames —— **真值口径**度量真正的串人：输出框在本帧的**有效检测**（即任何
//     `@ stale` 降级**之后**交给 update() 的那份）里若有同位置检测，该检测的身份即真值；二者
//     不一致即「框下面的人不是报的那个人」。stale 帧的真值因此是 Unknown（图库已换、旧认证结论
//     在新库下不成立），不会把保守的 fail-closed 降级误记成串人。
//     这是离线回放相对真机目视的核心优势——脚本自带 ground truth。
//
// 另需注意：轨迹在「已确认 + 缺失 1 帧」时会续航输出**上一帧的框**，此时本帧脚本里没有
// 同位置检测。这类帧记入 unattributedFrames 而**不计**串人——否则会把续航误报成身份错误。
struct ReplayMetrics {
    int frames = 0;                                // 帧类步数（F 与 @ missing；@ revoke/@ reset 不计）
    std::map<std::string, int> firstConfirmSteps;  // 身份 → 首次对外显示距其首现的步数
    std::map<std::string, int> trackIdsCarried;    // 身份 → 承载过它的不同 trackId 数
    int rebuilds = 0;             // Σ(承载数 − 1)：同一人的轨迹被重建的次数。
                                  // **并发重复轨迹同样计入**（该情形另由 duplicateIdCases 单独度量）。
    int identityRewrites = 0;     // 同一 trackId 报告身份在两个非 Unknown 值间跳变（@ revoke 后可非零）
    int staleIdentityFrames = 0;  // 真值口径的串人帧数
    int duplicateIdCases = 0;     // 同一帧同一非 Unknown 身份出现在多个 trackId 上（重复 ID）
    int unattributedFrames = 0;   // 输出框在本帧无同位置检测（缺失期续航）
    int neverConfirmed = 0;       // 脚本中出现过、但整段回放从未被对外显示的身份数
    // 新增（任务 #2/#5）：三条改写路径的可观测计数，**旧字段语义未变**。
    int clearedFrames = 0;        // 模型缺失（@ missing）帧数：对外视图被清空、未调用 update
    // ⚠️ 刻意**不**带 `Frames` 后缀：本字段不是帧计数（`staleIdentityFrames` /
    // `clearedFrames` / `unattributedFrames` 才是），而是「被撤销的**对外可见**身份」的
    // **累计个数**——一次 `@ revoke` 可以撤销多个身份，按个数累加，不是按步数。
    // 未确认轨迹的内部身份对外不可观测，故不计入（审计 L-9：原名字 revokedIdentityFrames
    // 会让人读成帧计数）。
    int revokedIdentities = 0;
};


ReplayMetrics runReplay(const ReplayScript& script,
                        const pipeline::TrackConfig& cfg = pipeline::TrackConfig{}) {
    pipeline::TrackCoordinator tc(cfg);
    ReplayMetrics m;

    std::map<std::string, int> firstSeenStep;
    std::map<int, std::string> lastReportedId;      // trackId → 上一次对外报告的身份
    std::map<std::string, std::set<int>> carried;   // 身份 → 承载过它的 trackId 集合
    std::set<std::string> confirmed;
    std::set<std::string> lastViewIds;              // 上一帧对外的非 Unknown 身份集（供 @ revoke 计数）

    int step = -1;  // 帧类步序号（F / @ missing）；@ revoke / @ reset 不推进，保持旧「回放条目步数」语义
    for (const auto& s : script) {
        if (s.kind == StepKind::Revoke) {
            // 帧间事件：先把新图库有效身份集交给 coordinator 精准撤销（镜像 Engine 的
            // revokeTrackIdentitiesLocked → resetIdentitiesExcept），再按**撤销前的**对外可见身份集
            // 统计被撤销者，并同步可见集（不在 keep 内的即消失）。
            tc.resetIdentitiesExcept(s.keepIds);
            std::set<std::string> surviving;
            for (const auto& id : lastViewIds) {
                bool keep = false;
                for (const auto& k : s.keepIds) {
                    if (id == k) { keep = true; break; }
                }
                if (keep) surviving.insert(id);
                else m.revokedIdentities += 1;
            }
            lastViewIds.swap(surviving);
            continue;
        }
        if (s.kind == StepKind::Reset) {
            // 清空内部轨迹（非产线路径，仅与 @ missing 对照）。对外视图到下一帧才反映。
            tc.reset();
            lastViewIds.clear();
            continue;
        }

        ++step;
        ++m.frames;

        if (s.kind == StepKind::ModelMissing) {
            // 模型缺失：**不调用 update()**，只清空对外视图；coordinator 内部轨迹原样保留
            //（下一次真正检测恢复且 TTL 未过时，轨迹**继续沿用**，不重建）。
            ++m.clearedFrames;
            lastViewIds.clear();
            continue;
        }

        const ScriptedFrame& frame = s.frame;
        // 有效检测 = 帧内检测；若带 @ stale 则整帧降级为「未认证 + Unknown」，镜像 Engine 1296-1301。
        std::vector<pipeline::DetectedFace> faces;
        faces.reserve(frame.detections.size());
        for (const auto& d : frame.detections) {
            pipeline::DetectedFace df;
            df.bbox = d.bbox;
            df.isAuthenticated = frame.staleAuth ? false : d.authenticated;
            df.identityId = frame.staleAuth ? std::string("Unknown")
                                            : (d.authenticated ? d.identityId : std::string());
            df.confidence = (d.authenticated && !frame.staleAuth) ? 0.9f : 0.0f;
            faces.push_back(df);
            // firstSeenStep 用**原始**检测（脚本即现实：该身份确实出现过）。stale 帧同样登记，
            // 于是「被作废导致确认推迟」体现为首确认步数变大，而非凭空少一个身份。
            if (d.authenticated && !d.identityId.empty()) {
                firstSeenStep.emplace(d.identityId, step);  // 只记首次
            }
        }

        const auto views = tc.update(faces, frame.tsMs, frame.detectionRan);

        std::set<std::string> viewIds;
        std::map<std::string, int> idOccurrences;
        for (const auto& v : views) {
            if (v.stableId != "Unknown") {
                idOccurrences[v.stableId] += 1;
                carried[v.stableId].insert(v.trackId);
                viewIds.insert(v.stableId);
                if (confirmed.insert(v.stableId).second) {
                    // firstConfirmSteps 的「缺键省略」分支**已知不可测**：视图的 stableId 只能来自
                    // 已认证检测（`@ stale` 只产生 Unknown），而任何已认证检测都已登记进 firstSeenStep；
                    // 空身份串又被解析器拒绝。故 find() 必命中。理由详见文件末「已知不可测项」。
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

            // 串人（真值口径）：以**有效检测**（stale 降级后）为准，未认证检测的真值视作
            // "Unknown"，故「alice 的框盖住一个陌生人」同样计入。
            const pipeline::DetectedFace* truth = nullptr;
            for (const auto& f : faces) {
                if (sameRect(f.bbox, v.bbox)) { truth = &f; break; }
            }
            if (truth == nullptr) {
                m.unattributedFrames += 1;
            } else {
                const std::string truthId =
                    truth->isAuthenticated ? truth->identityId : std::string("Unknown");
                if (truthId != v.stableId) m.staleIdentityFrames += 1;
            }
        }
        for (const auto& kv : idOccurrences) {
            if (kv.second >= 2) m.duplicateIdCases += 1;
        }
        lastViewIds.swap(viewIds);
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
       << " unattributedFrames=" << m.unattributedFrames << " neverConfirmed=" << m.neverConfirmed
       << " clearedFrames=" << m.clearedFrames
       << " revokedIdentities=" << m.revokedIdentities;

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
    // 用 m.frames（帧类步数）而非 script.size()：脚本可能含 @ revoke/@ reset 等不占帧的指令。
    out << "- 回放帧数: " << m.frames << "\n";
    out << "- 回放条目数（含 @ 指令）: " << script.size() << "\n";
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

// 构造一个检测帧步。stale=true 等价于在该帧后写一行 `@ stale`（帧内代次作废）。
ReplayStep frameAt(int index, long long ts, std::vector<ScriptedDetection> dets,
                   bool ran = true, bool stale = false) {
    ReplayStep s;
    s.kind = StepKind::Frame;
    s.frame.frameIndex = index;
    s.frame.tsMs = ts;
    s.frame.detectionRan = ran;
    s.frame.staleAuth = stale;
    s.frame.detections = std::move(dets);
    return s;
}

// 帧间身份撤销：keepIds 为空 → `@ revoke -`（清空全部已报告身份）。
ReplayStep revokeStep(std::vector<std::string> keepIds = {}) {
    ReplayStep s;
    s.kind = StepKind::Revoke;
    s.keepIds = std::move(keepIds);
    return s;
}

// 模型缺失帧：不调用 update()，对外视图清空、内部轨迹保留。
ReplayStep missingStep(int index, long long ts) {
    ReplayStep s;
    s.kind = StepKind::ModelMissing;
    s.frameIndex = index;
    s.tsMs = ts;
    return s;
}

// reset() 步（非产线路径，仅与 missingStep 对照）。
ReplayStep resetStep() {
    ReplayStep s;
    s.kind = StepKind::Reset;
    return s;
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
// **与既有用例的关系（A1 审计 Low-3；复核结论：保留）**：本用例场景与
// test_track_coordinator.cpp 的 ContinuouslyMovingTargetCompletesInitialConfirmation 等价。
//
// ⚠️ **修正记录（A1 验收审计 N1 推翻）**：本注释初版写的保留依据是「遍历本文件全部场景，
// **只有**本用例走中心距离回退匹配的**成功**分支」——**该断言是错的**。同文件
// TwoParallelFacesDoNotMergeAcrossFallbackRatio 的 70px/帧 与 ScriptFileRoundTripsAndReplaysIdentically
// 的 alice 同为 70px/帧，框宽 100 → IoU = 3000/17000 ≈ 0.176 < matchIouThreshold(0.3)，
// 同样落在回退成功分支（前者的注释自己就写着「确认 70px/帧的移动目标」）。
//
// **真正的保留依据**：它断言的是**载体读数**（firstConfirmSteps / trackIdsCarried），
// 而 test_track_coordinator.cpp 的等价用例断言的是**跟踪规则**（trackId / trackCount）——
// 覆盖对象不同。删掉它，载体侧「回退匹配下首次确认 = 2 步」这一读数就失去锚点。
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
//
// **与既有用例的关系（A1 审计 Low-3；#4 独立复核结论：保留）**：本用例与
// test_track_coordinator.cpp 的 DisplacementBeyondFallbackRatioNeverConfirms **同名同场景**（仅
// suite 不同，GTest 按 suite 注册，实测无跳过/覆盖）。看似重复却不删的**独立依据**：本文件其余
// 场景**没有任何一条**会产出 `firstConfirmSteps` 与 `trackIdsCarried` **同时为空**的读数——本用例
// 是载体“读数为空”这一输出形态的**唯一**锚点；且它走的是**回退匹配失败**分支（其余都是命中 IoU
// 或命中回退）。既验证了这条分支的读数，又覆盖了空指标的输出路径。
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
        serializeReplaySteps(out, s);
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
    EXPECT_EQ(s[0].frame.detections.size(), 2u);
    EXPECT_TRUE(s[0].frame.detections[0].authenticated);
    EXPECT_EQ(s[0].frame.detections[0].identityId, "alice");
    EXPECT_FALSE(s[0].frame.detections[1].authenticated);
    EXPECT_TRUE(s[0].frame.detections[1].identityId.empty());
    EXPECT_TRUE(s[0].frame.detectionRan);
    EXPECT_FALSE(s[1].frame.detectionRan);
}

// ── 解析器必须拒绝畸形指令（与上面三类拒绝同规格：行号 + 原因，绝不部分接受）──
TEST(TrackReplay, ParserRejectsMalformedDirectives) {
    auto expectReject = [](const std::string& text, const char* why) {
        std::istringstream in(text);
        ReplayScript s;
        std::string err;
        EXPECT_FALSE(parseReplayScript(in, s, err)) << "应拒绝：" << why;
        EXPECT_FALSE(err.empty()) << "拒绝时必须给出原因：" << why;
    };

    // @ 前缀本身及其操作名
    expectReject("F 0 1000 1\n@\n", "@ 指令缺少操作名");
    expectReject("F 0 1000 1\n@ bogus\n", "未知 @ 指令");
    // @ stale：必须挂在 F 之后，且不接受参数
    expectReject("@ stale\nF 0 1000 1\nD 1 2 3 4 1 alice\n", "@ stale 出现在任何 F 之前");
    expectReject("F 0 1000 1\n@ stale extra\n", "@ stale 多余字段");
    expectReject("F 0 1000 1\n@ missing 1 1000\n@ stale\n", "@ stale 挂在 @ missing 之后");
    // @ revoke：参数缺失 / 多余 / 列表格式非法
    expectReject("F 0 1000 1\n@ revoke\n", "@ revoke 缺参数");
    expectReject("F 0 1000 1\n@ revoke alice bob\n", "@ revoke 多余字段（空格分隔）");
    expectReject("F 0 1000 1\n@ revoke ,\n", "@ revoke 空元素");
    expectReject("F 0 1000 1\n@ revoke alice,\n", "@ revoke 尾随逗号空元素");
    expectReject("F 0 1000 1\n@ revoke alice,-\n", "@ revoke 列表含 '-' 段");
    // @ missing：参数缺失 / 多余 / 其后不得再接 D
    expectReject("F 0 1000 1\n@ missing\n", "@ missing 字段不足");
    expectReject("F 0 1000 1\n@ missing 1 1000 x\n", "@ missing 多余字段");
    expectReject("F 0 1000 1\n@ missing 1 1000\nD 1 2 3 4 1 alice\n", "D 出现在 @ missing 之后");
    // @ reset：不接受参数
    expectReject("F 0 1000 1\n@ reset extra\n", "@ reset 多余字段");
    // 只有指令、没有任何帧类步的脚本同样视为空
    expectReject("@ revoke -\n", "只有指令、无帧类步");

    // 合法指令必须被接受：@ stale / @ revoke <keep> / @ missing / @ reset 各一。
    std::istringstream good(
        "F 0 1000 1\n"
        "D 1 2 3 4 1 alice\n"
        "@ stale\n"
        "@ revoke -\n"
        "@ revoke alice,bob\n"
        "@ missing 1 1100\n"
        "@ reset\n");
    ReplayScript s;
    std::string err;
    ASSERT_TRUE(parseReplayScript(good, s, err)) << err;
    ASSERT_EQ(s.size(), 5u);  // F / @revoke(空) / @revoke(列表) / @missing / @reset（D 与 @stale 不单独成步）
    EXPECT_EQ(s[0].kind, StepKind::Frame);
    EXPECT_TRUE(s[0].frame.staleAuth);
    ASSERT_EQ(s[1].kind, StepKind::Revoke);
    EXPECT_TRUE(s[1].keepIds.empty());
    ASSERT_EQ(s[2].kind, StepKind::Revoke);
    ASSERT_EQ(s[2].keepIds.size(), 2u);
    EXPECT_EQ(s[2].keepIds[0], "alice");
    EXPECT_EQ(s[2].keepIds[1], "bob");
    ASSERT_EQ(s[3].kind, StepKind::ModelMissing);
    EXPECT_EQ(s[3].frameIndex, 1);
    EXPECT_EQ(s[3].tsMs, 1100);
    EXPECT_EQ(s[4].kind, StepKind::Reset);
}

// ══════════════════════════════════════════════════════════════════════════════
// 任务 #2：把 Engine 的三条改写路径做成可回放、可度量的指令
// ══════════════════════════════════════════════════════════════════════════════

// ── 代次作废（@ stale）：把「本帧认证结论已失效」显式建模为输入降级 ─────────────
// 场景：一个**匿名**目标先连中 2 帧（轨迹已建、stableId=Unknown、confirmFrames=2），第 3 帧
// 才被认证为 alice——但恰好这一帧发生图库代次作废，整帧降级为 Unknown 交 update()。
// 于是第 3 帧的 alice **不会**被写进轨迹，对外仍是 Unknown；直到第 4 帧（不再作废）alice 才
// 落定。**可观测后果**：firstConfirmSteps["alice"] 从「0（首现即显示）」变为「1（被推迟一步）」。
//
// 逐帧手推（stableFrames=3；B=(200,150,160,200)）：
//   step0 匿名B → 新轨迹 T1(stableId=Unknown, confirm=1)，视图空
//   step1 匿名B → T1 confirm=2，视图空
//   step2 aliceB 被 stale 降级为「未认证+Unknown」→ T1 confirm=3 → 视图 [T1 Unknown]
//         （firstSeenStep["alice"]=2 记的是**原始**检测；视图 stableId=Unknown 被跳过）
//   step3 aliceB（不再作废）→ stableId=="Unknown" 且已认证 → 写入 alice → 视图 [T1 alice]
//         confirmed.insert(alice) 首见 → firstConfirmSteps = 3 − 2 = 1
// 若不发生作废：step2 即写入 alice → firstConfirmSteps = 2 − 2 = 0。故本断言对 @ stale 承重。
TEST(TrackReplay, GenerationInvalidationDelaysIdentityAcquisition) {
    const cv::Rect B(200, 150, 160, 200);
    ReplayScript s;
    s.push_back(frameAt(0, 1000, {det(B.x, B.y, B.width, B.height, "")}));
    s.push_back(frameAt(1, 1100, {det(B.x, B.y, B.width, B.height, "")}));
    s.push_back(frameAt(2, 1200, {det(B.x, B.y, B.width, B.height, "alice")}, true, /*stale=*/true));
    s.push_back(frameAt(3, 1300, {det(B.x, B.y, B.width, B.height, "alice")}));
    const auto m = runReplay(s);

    EXPECT_EQ(m.frames, 4);
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 1)
        << "作废把 alice 的首次对外显示推迟了一步（不作废时为 0）";
    EXPECT_EQ(metricOf(m.trackIdsCarried, "alice"), 1) << "同一轨迹，未因作废而重建";
    EXPECT_EQ(m.rebuilds, 0);
    EXPECT_EQ(m.staleIdentityFrames, 0) << "stale 帧的真值按降级后计（Unknown），不是串人";
    EXPECT_EQ(m.identityRewrites, 0);
    EXPECT_EQ(m.neverConfirmed, 0);
    EXPECT_EQ(m.clearedFrames, 0);
    EXPECT_EQ(m.revokedIdentities, 0);
}

// ── 身份撤销（@ revoke）：撤销后同一轨迹可被**另一个**身份改写（identityRewrites 非零）──
// 这是审计 A3「identityRewrites 在回放里结构性恒 0」的非零反例，也是 #5 的核心。
//
// **关键机制（与「id → Unknown → id」不同）**：计数要求**前后都非 Unknown**。撤销把 T1 的
// stableId 清为 Unknown 后：
//   * 若下一帧重新命中**同一**身份 alice——中途经一次对外 Unknown（lastReportedId 变 Unknown），
//     前后不再「都非 Unknown」，**不计**改写（那是恢复，不是改写）；
//   * 若下一帧命中**另一个**身份 bob——该帧 update() 在 stableId=="Unknown" 时写入 bob，对外
//     直接从 alice 跳到 bob（**中途不经对外 Unknown**），prev=alice/current=bob **都非 Unknown**
//     且不同 → identityRewrites += 1。这才是真正的「身份改写」。
// （注：@ revoke 不产生视图，故它本身不会把 lastReportedId 置为 Unknown。）
TEST(TrackReplay, RevocationAllowsIdentityRewriteAcrossSameTrack) {
    const cv::Rect B(200, 150, 160, 200);
    ReplayScript s;
    for (int i = 0; i < 3; ++i) {  // alice 三帧确认（T1）
        s.push_back(frameAt(i, 1000 + i * 100, {det(B.x, B.y, B.width, B.height, "alice")}));
    }
    s.push_back(revokeStep({}));   // 空图库：清掉 alice 身份（保留轨迹/bbox/确认计数）
    s.push_back(frameAt(3, 1300, {det(B.x, B.y, B.width, B.height, "bob")}));  // 同一轨迹改报 bob

    const auto m = runReplay(s);

    EXPECT_EQ(m.identityRewrites, 1)
        << "同一 trackId 的报告身份由 alice 直接跳到 bob，即一次身份改写";
    EXPECT_EQ(m.revokedIdentities, 1) << "@ revoke - 撤销了当前对外可见的 alice";
    EXPECT_EQ(metricOf(m.trackIdsCarried, "alice"), 1);
    EXPECT_EQ(metricOf(m.trackIdsCarried, "bob"), 1);
    EXPECT_EQ(m.rebuilds, 0) << "撤销只清身份、不丢轨迹，故不构成重建";
    EXPECT_EQ(m.staleIdentityFrames, 0) << "bob 帧的真值就是 bob";
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "bob"), 0) << "轨迹已确认，写入身份即显示";
    EXPECT_EQ(m.neverConfirmed, 0);
}

// ── 撤销计数按**身份个数**计、非按步/帧计（C-10b）：一次 @ revoke 撤销两人 → 2 ──────
// 动机：identityRewrites 与 revokedIdentities 两个指标的**唯一非零覆盖**原都在上一条用例里
//（各断言 ==1），任一被删则另一个也失去非零覆盖。本用例**独立**钉住 revokedIdentities 的语义。
//
// 字段刻意**不带** `Frames` 后缀（见其声明处注释）：它不是帧计数——一次 @ revoke 可撤销**多个**
// 身份，按**个数**累加，不是按 @ revoke 步数、也不是按帧。故本用例让「步数(1) ≠ 个数(2)」：
// 构造：alice 与 bob 在**不同 y 带**并行确认（互不串轨），末帧对外视图同时含两人；随后**一条**
// `@ revoke -`（空 keep）把两人一并撤销。若计数被误实现为「每步 +1」，本用例立即变红。
TEST(TrackReplay, RevocationCountsIdentitiesNotRevokeSteps) {
    ReplayScript s;
    // alice 在上带、bob 在下带；竖直中心间距 200 > centerMaxRatio×100，IoU/中心判据都不会并轨。
    for (int i = 0; i < 3; ++i) {
        s.push_back(frameAt(i, 1000 + i * 100,
                            {det(0, 0, 100, 100, "alice"), det(0, 200, 100, 100, "bob")}));
    }
    s.push_back(revokeStep({}));  // 空图库：**一条** @ revoke 清掉 alice 与 bob **两个**身份

    const auto m = runReplay(s);

    const int revokeSteps = static_cast<int>(std::count_if(
        s.begin(), s.end(), [](const ReplayStep& st) { return st.kind == StepKind::Revoke; }));
    EXPECT_EQ(revokeSteps, 1) << "脚本里恰有 1 条 @ revoke 步";
    EXPECT_EQ(m.revokedIdentities, 2)
        << "一条 @ revoke 撤销两个对外可见身份 → 计数为 2（按身份个数，非按步/帧）";
    // 两人并行确认、互不串轨的对照读数。
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "alice"), 2);
    EXPECT_EQ(metricOf(m.firstConfirmSteps, "bob"), 2);
    EXPECT_EQ(m.staleIdentityFrames, 0) << "上下两带不得互相串轨";
    EXPECT_EQ(m.duplicateIdCases, 0);
    EXPECT_EQ(m.neverConfirmed, 0);
}

// ── 模型缺失（@ missing）清对外视图 vs reset() 清内部轨迹：**行为必须不同** ─────────
// 审计 A1 指出的微妙点：模型缺失只 `currentTracks_.clear()`（清**对外视图**），coordinator
// 内部轨迹**原样保留**（仅受下一次 update 的 TTL 约束）；而 `reset()` 清的是**内部轨迹本体**。
// 本用例把两者放在**同一段检测序列**上对跑，用读数把差异钉死：
//   * @ missing 后：轨迹 T1 仍在 → 检测恢复后**沿用同一 trackId**、身份仍在 → rebuilds=0、
//     trackIdsCarried[alice]=1；
//   * reset 后：轨迹被清 → 检测恢复后**新建 trackId**、需重新累计 stableFrames → rebuilds=1、
//     trackIdsCarried[alice]=2。
// 两者的 firstConfirmSteps[alice] 都是 2（身份本身没变），差异**只在轨迹连续性**上——正是要证的。
TEST(TrackReplay, ModelMissingClearsViewButKeepsTracksUnlikeReset) {
    const cv::Rect B(10, 20, 120, 160);
    auto confirmThree = [&](ReplayScript& s, long long base) {
        for (int i = 0; i < 3; ++i) {
            s.push_back(frameAt(i, base + i * 100, {det(B.x, B.y, B.width, B.height, "alice")}));
        }
    };

    // A：模型缺失一帧（不调用 update，内部轨迹保留）。
    ReplayScript missing;
    confirmThree(missing, 1000);
    missing.push_back(missingStep(3, 1300));
    confirmThree(missing, 1400);  // 检测恢复：TTL 内，轨迹继续沿用
    const auto mMissing = runReplay(missing);

    EXPECT_EQ(mMissing.frames, 7) << "缺失帧也计入回放帧数（它是视频里的一帧）";
    EXPECT_EQ(mMissing.clearedFrames, 1);
    EXPECT_EQ(mMissing.rebuilds, 0) << "内部轨迹保留 → 不重建";
    EXPECT_EQ(metricOf(mMissing.trackIdsCarried, "alice"), 1) << "始终同一 trackId";
    EXPECT_EQ(metricOf(mMissing.firstConfirmSteps, "alice"), 2);
    EXPECT_EQ(mMissing.neverConfirmed, 0);

    // B：reset()（清空内部轨迹本体）。
    ReplayScript reset;
    confirmThree(reset, 1000);
    reset.push_back(resetStep());
    confirmThree(reset, 1300);
    const auto mReset = runReplay(reset);

    EXPECT_EQ(mReset.frames, 6) << "reset 不是帧，不计入回放帧数";
    EXPECT_EQ(mReset.clearedFrames, 0);
    EXPECT_EQ(mReset.rebuilds, 1) << "内部轨迹被清 → 检测恢复后重建";
    EXPECT_EQ(metricOf(mReset.trackIdsCarried, "alice"), 2) << "先后两个 trackId";
    EXPECT_EQ(metricOf(mReset.firstConfirmSteps, "alice"), 2);

    // 二者行为差异的总断言：模型缺失不重建轨迹，reset 会重建。
    EXPECT_NE(mMissing.rebuilds, mReset.rebuilds);
}

// ── 含指令的脚本同样可无损往返：write → parse → 指标逐字一致 ─────────────────────
// 与前一条纯检测帧的往返互补，覆盖 @ stale / @ revoke / @ missing / @ reset 的序列化与解析。
TEST(TrackReplay, DirectiveScriptRoundTripsAndReplaysIdentically) {
    ReplayScript s;
    s.push_back(frameAt(0, 1000, {det(10, 20, 100, 100, "alice")}));
    s.push_back(frameAt(1, 1100, {det(10, 20, 100, 100, "alice")}));
    s.push_back(frameAt(2, 1200, {det(10, 20, 100, 100, "alice"), det(400, 400, 80, 80, "")},
                        true, /*stale=*/true));
    s.push_back(revokeStep({}));
    s.push_back(missingStep(3, 1300));
    s.push_back(revokeStep({"alice", "bob"}));
    s.push_back(resetStep());
    s.push_back(frameAt(4, 1400, {det(10, 20, 100, 100, "alice")}));

    const std::filesystem::path dir = makeTempDir("directives");
    TempDirGuard guard(dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const std::string scriptPath = (dir / "directives.txt").string();
    std::string text;
    {
        std::ostringstream oss;
        serializeReplaySteps(oss, s);
        text = oss.str();
        std::ofstream out(scriptPath.c_str(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out);
        out << text;
    }
    // 四类指令都必须真的写进文本（否则往返在“空指令”上通过是假绿）。
    EXPECT_NE(text.find("@ stale"), std::string::npos) << text;
    EXPECT_NE(text.find("@ revoke -"), std::string::npos) << text;
    EXPECT_NE(text.find("@ revoke alice,bob"), std::string::npos) << text;
    EXPECT_NE(text.find("@ missing 3 1300"), std::string::npos) << text;
    EXPECT_NE(text.find("@ reset"), std::string::npos) << text;

    std::ifstream in(scriptPath.c_str());
    ASSERT_TRUE(in);
    ReplayScript parsed;
    std::string err;
    ASSERT_TRUE(parseReplayScript(in, parsed, err)) << err;
    EXPECT_TRUE(sameScript(s, parsed)) << "含指令的脚本序列化/解析必须无损";
    EXPECT_EQ(formatMetrics(runReplay(s)), formatMetrics(runReplay(parsed)))
        << "指标须与内存脚本逐字相同";
}

// ══════════════════════════════════════════════════════════════════════════════
// 导出器（pipeline::writeReplayScript）→ **真**解析器 的往返
//
// 为什么要这条用例：导出器在 `src/cpp/src/pipeline/ReplayScriptWriter.cpp`，是 CLI
// （`win_replay_export_cli`）落盘的**唯一**写出路径；但它的输出此前只在它自己的测试文件里，
// 由一份**照抄解析规则另写的镜像 oracle** 验收——镜像与真解析器一旦漂移，那条用例**不会红**。
// 「真实素材 → 检测序列 → 回放」这条链路的格式契约必须由**同一份**解析器锚定，故在此补上。
// （镜像 oracle 的局限已由其作者声明，且此前的真解析器验证是**手工跑**的、不在 CI 内。）
//
// 三条断言各自的定位，缺一不可：
//   ① 真解析器**接受**导出器输出——否则 CLI 产出的文件根本进不了回放；
//   ② 解析回来的脚本经本文件序列化器写出后与导出器原文**逐字节相同**——证明往返**无损**，
//      而不只是「解析器宽容地啃下了一堆它其实理解不了的东西」；
//   ③ 回放指标**非空**（有帧、认得身份）——排除「解析成功但谱出一份空脚本」这种假绿。
TEST(TrackReplay, ExporterWriterOutputIsAcceptedByRealParser) {
    pipeline::ReplayScriptData data;
    for (int frame = 0; frame < 3; ++frame) {
        pipeline::ReplayFrame f;
        f.frameIndex = frame;
        f.tsMs = frame * 40;
        f.detectionRan = true;
        // 已认证与未认证检测**混排**：两条路径都要走到，否则本用例对 `-` 编码的覆盖是空的
        // （未认证检测的 identityId 留空 → 导出器写出 `-`）。
        pipeline::ReplayDetection authed;
        authed.x = 100 + frame * 5;
        authed.y = 120;
        authed.width = 80;
        authed.height = 80;
        authed.authenticated = true;
        authed.identityId = "alice";
        f.detections.push_back(authed);

        pipeline::ReplayDetection anon;
        anon.x = 300;
        anon.y = 200 + frame * 5;
        anon.width = 60;
        anon.height = 60;
        anon.authenticated = false;
        f.detections.push_back(anon);

        data.push_back(f);
    }

    std::string err;
    std::ostringstream emittedStream;
    ASSERT_TRUE(pipeline::writeReplayScript(emittedStream, data, err)) << err;
    const std::string emitted = emittedStream.str();
    EXPECT_NE(emitted.find("D 100 120 80 80 1 alice"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("D 300 200 60 60 0 -"), std::string::npos) << emitted;

    // ① 真解析器接受
    std::istringstream in(emitted);
    ReplayScript parsed;
    ASSERT_TRUE(parseReplayScript(in, parsed, err)) << "导出器输出被真解析器拒绝：" << err;
    ASSERT_EQ(parsed.size(), 3u);

    // ② 往返逐字节无损（本文件的序列化器不写注释头，故与导出器原文可直接比对）
    std::ostringstream round;
    serializeReplaySteps(round, parsed);
    EXPECT_EQ(round.str(), emitted) << "导出器输出与回放侧序列化不一致（格式契约已漂移）";

    // ③ 回放指标非空
    const auto m = runReplay(parsed);
    EXPECT_EQ(m.frames, 3) << formatMetrics(m);
    EXPECT_NE(m.firstConfirmSteps.find("alice"), m.firstConfirmSteps.end())
        << "已认证检测应在回放中被认出身份：" << formatMetrics(m);
}

// CLI 实际写出的文件带 `#` 溯源头（来源视频、抽稀率）。解析器声明忽略 `#` 行——这条
// 不能被上一用例覆盖（它用无头输出比对字节），故单列：带头与不带头必须解析出**同一脚本**。
TEST(TrackReplay, ExporterHeaderCommentDoesNotAlterParsedScript) {
    pipeline::ReplayScriptData data;
    pipeline::ReplayFrame f;
    f.frameIndex = 7;
    f.tsMs = 280;
    pipeline::ReplayDetection d;
    d.x = 10;
    d.y = 20;
    d.width = 30;
    d.height = 40;
    d.authenticated = true;
    d.identityId = "bob";
    f.detections.push_back(d);
    data.push_back(f);

    std::string err;
    // 传**不含 `#`** 的裸文本——镜像 CLI 的真实调用（CLI 传的是 `source=<path>` 这类原文）。
    // ⚠️ 修正记录（A3 审计）：初版这里传的是**已带 `# ` 的文本**，两头不讨好——
    //   ① 没镜像 CLI 的真实调用形态；② 导出器会再加一层前缀写出 `# # …`，从而**掩盖了
    //   「前缀器坏掉」**：把导出器的前缀加上动作删掉，初版断言仍然全绿。现改为裸文本，
    //   并显式断言前缀确实由导出器加上、且不得出现双前缀。
    std::ostringstream withHeader;
    ASSERT_TRUE(pipeline::writeReplayScript(withHeader, data, "source=video.mp4\nstride=2", err))
        << err;

    const std::string text = withHeader.str();
    EXPECT_NE(text.find("# source=video.mp4"), std::string::npos) << text;
    EXPECT_NE(text.find("# stride=2"), std::string::npos) << text;
    EXPECT_EQ(text.find("# # "), std::string::npos) << "不得写出双前缀：" << text;

    std::istringstream in(text);
    ReplayScript parsed;
    ASSERT_TRUE(parseReplayScript(in, parsed, err)) << err;
    ASSERT_EQ(parsed.size(), 1u);
    ASSERT_EQ(parsed[0].frame.detections.size(), 1u);
    EXPECT_EQ(parsed[0].frame.detections[0].identityId, "bob");
    EXPECT_EQ(parsed[0].frame.frameIndex, 7);
}

// 导出器**拒绝侧**与真解析器的一致性（审计 I2 残留缺口：上面两条只覆盖了 happy path）。
//
// 要害是空脚本：真解析器在「一条 F 记录都没有」时**拒绝整个文件**，所以导出器必须**先拒绝**，
// 而不是写出一个本系统自己读不回来的空文件。审计 N2 实测该洞原本真实存在——
// `validate({})` 曾返回 true、写出空串并返回成功，正是导出器头文件所声明的
// 「不写出会被解析器拒绝的半成品」被违反的情形。
TEST(TrackReplay, ExporterRejectsWhatRealParserWouldReject) {
    // 前提（真解析器侧）：空输入必被拒绝。前提若不成立，下面的断言就是空的。
    std::istringstream emptyIn("");
    ReplayScript parsed;
    std::string parseErr;
    EXPECT_FALSE(parseReplayScript(emptyIn, parsed, parseErr))
        << "前提不成立：真解析器竟然接受了空输入";

    // 导出器侧：同样必须拒绝，且不得留下半成品（先校验后写出）。
    std::ostringstream os;
    std::string writeErr;
    EXPECT_FALSE(pipeline::writeReplayScript(os, pipeline::ReplayScriptData{}, writeErr))
        << "空脚本必须被导出器拒绝——否则会产出一份本系统读不回来的空文件";
    EXPECT_FALSE(writeErr.empty()) << "拒绝时必须给出可读原因";
    EXPECT_TRUE(os.str().empty()) << "拒绝时必须先校验后写出，不得留下半成品";
}

// ── 已知不可测项（如实声明，勿改成永远通过的空壳用例）──────────────────────────
// 审计 A3 曾判定 identityRewrites 与 firstConfirmSteps 的「缺键省略」两条分支在回放中不可达。
// 本次复核结论：
//   * identityRewrites —— **已可达**（非零反例见 RevocationAllowsIdentityRewriteAcrossSameTrack）。
//     原判「不可达」的前提是「回放不触发 resetIdentitiesExcept」；@ revoke 指令补上了这一入口，
//     且**换身份**（而非重新认证同一身份）时中途不经对外 Unknown，故计数真能增加。
//   * firstConfirmSteps 的「缺键省略」——**仍不可测**，理由：该分支要求某条对外视图的 stableId
//     不在 firstSeenStep 里。而视图的 stableId 只可能来自：(a) 已认证检测的 identityId——必已登记
//     进 firstSeenStep；(b) "Unknown"——被 `!= "Unknown"` 直接跳过；(c) `@ stale` 降级产生的
//     "Unknown"——同 (b)。解析器又拒绝「已认证却无身份」（空串）这一唯一能造出“非 Unknown 却未
//     登记”的输入。故 find() 必命中，省略分支不可达。**不为其编写永远通过的空壳用例**。
//
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
