#include "pipeline/ReplayScriptWriter.h"

#include <ostream>
#include <sstream>

namespace pipeline {
namespace {

// identityId 必须是不含任何空白字符的单 token——否则写出后 D 行会被解析器拆成多余字段
// （解析器对多余字段一律报错），或把身份截断。'\r'/'\n' 更会直接破坏行结构。
bool hasWhitespace(const std::string& s) {
    for (char c : s) {
        // 必须覆盖 C locale 下 operator>> 视为分隔符的**全部**字符。只查 ' ' 与 \t\r\n
        // 会漏掉 \v/\f：含它们的身份串能通过本校验，写出后却被解析器按空白拆成多余字段
        // 而报错——导出器「不写出会被解析器拒绝的半成品」这一契约就在边角处失效。
        switch (c) {
            case ' ':
            case '\t':
            case '\n':
            case '\v':
            case '\f':
            case '\r':
                return true;
            default:
                break;
        }
    }
    return false;
}

// 与解析器内 sameRect 同口径（字段逐一比较，不依赖 operator==：mock cv::Rect 没有它，
// 虽然本结构不是 cv::Rect，但保持同口径可避免任何比较语义漂移）。
bool sameBox(const ReplayDetection& a, const ReplayDetection& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

std::string detectionError(std::size_t frameIdx, std::size_t detIdx, const std::string& why) {
    std::ostringstream os;
    os << "第 " << frameIdx << " 帧第 " << detIdx << " 个检测：" << why;
    return os.str();
}

// 逐条复刻解析器的拒绝规则。任何一条不满足即返回 false——**不静默规范化**，
// 与解析器「宁可报错也不让错误的读数混进来」的立场一致。
bool validate(const ReplayScriptData& script, std::string& err) {
    // 拒绝⓪：空脚本（一帧都没有）。解析器在「一条 F 记录都没有」时直接拒绝
    //（"检测序列为空"），所以写出一个空文件等于产出一份**本系统自己读不回来**的产物；
    // 典型触发是上游视频零帧或抽稀后无帧。写空文件是**静默失败**，必须在此拦下。
    // 注意区分：有 F 行但该帧无检测是**合法**的（解析器只要求存在帧记录）。
    if (script.empty()) {
        err = "检测序列为空：至少需要一帧检测记录（否则解析器会拒绝整个文件）";
        return false;
    }
    for (std::size_t fi = 0; fi < script.size(); ++fi) {
        const ReplayFrame& f = script[fi];
        for (std::size_t di = 0; di < f.detections.size(); ++di) {
            const ReplayDetection& d = f.detections[di];
            if (hasWhitespace(d.identityId)) {
                err = detectionError(fi, di, "身份串不得包含空白字符（会破坏 D 行 token 结构）");
                return false;
            }
            if (d.authenticated) {
                // 拒绝①：已认证却无身份。写 `-`(或空)会被解析器拒绝；即便侥幸通过，
                // 协调器也会把新轨迹的 stableId 置为空串，被指标当成「真实身份」混进读数。
                if (d.identityId.empty() || d.identityId == "-") {
                    err = detectionError(
                        fi, di, "authenticated=1 必须携带真实身份串（不得为空或 '-'）");
                    return false;
                }
            } else {
                // 拒绝②：未认证却携带身份串。反向矛盾同样说明上游有 bug，静默丢弃会掩盖它。
                // 允许空与规范编码 "-"，其余一律拒绝。
                if (!d.identityId.empty() && d.identityId != "-") {
                    err = detectionError(fi, di, "未认证检测（authenticated=0）不得携带身份串");
                    return false;
                }
            }
            // 拒绝③：同帧重复 bbox。真值身份归属只能取其一，是未定义的；解析器亦拒绝。
            for (std::size_t pj = 0; pj < di; ++pj) {
                if (sameBox(f.detections[pj], d)) {
                    err = detectionError(fi, di, "同一帧内出现重复 bbox，真值身份归属无法定义");
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

bool writeReplayScript(std::ostream& os, const ReplayScriptData& script,
                       const std::string& headerNote, std::string& err) {
    err.clear();
    if (!validate(script, err)) return false;

    if (!headerNote.empty()) {
        // '#' 起始的行被解析器忽略（trimmed 后首字符为 '#'）。逐行加前缀，多行注释一并写出。
        std::istringstream lines(headerNote);
        std::string line;
        while (std::getline(lines, line)) {
            os << "# " << line << "\n";
        }
    }
    for (const auto& f : script) {
        os << "F " << f.frameIndex << " " << f.tsMs << " " << (f.detectionRan ? 1 : 0) << "\n";
        for (const auto& d : f.detections) {
            os << "D " << d.x << " " << d.y << " " << d.width << " " << d.height << " "
               << (d.authenticated ? 1 : 0) << " "
               << (d.authenticated ? d.identityId : std::string("-")) << "\n";
        }
    }
    // 传播流向 ostream 的错误位（磁盘满/写失败），让调用方能据此判定落盘是否成功。
    return static_cast<bool>(os);
}

bool writeReplayScript(std::ostream& os, const ReplayScriptData& script, std::string& err) {
    return writeReplayScript(os, script, std::string(), err);
}

}  // namespace pipeline
