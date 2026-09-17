#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace pipeline {

// ══════════════════════════════════════════════════════════════════════════════
// 「视频/磁盘 → 逐帧检测序列」的序列化器（任务 C1 离线回放载体的最后一环）
//
// 背景：tests/cpp/test_track_replay.cpp 定义了**检测序列文本格式**与解析器
// parseReplayScript，并驱动 TrackCoordinator 产出轨迹指标。但把真实录像转成该文本的
// 步骤在本仓库此前**不存在**——本文件补上这一环。
//
// **唯一事实源**：文本格式与约束全部对齐 test_track_replay.cpp 的 parseReplayScript。
// 该解析器逐条强制以下规则（本序列化器在写出前先校验，违反者返回 false，而不是写出一个
// 会被解析器拒绝、或被静默半解析出错误指标的半成品）：
//   * `F <frameIndex> <tsMs> <detectionRan:0|1>`          ← 开始一帧
//   * `D <x> <y> <w> <h> <authenticated:0|1> <identityId>` ← 该帧的一个检测
//   * identityId 为不含**任何空白字符**（含 `\v`/`\f`）的单 token；未认证写 `-`。
//   * `authenticated=1` 必须带真实身份串（写 `-` 一律被解析器拒绝）。
//   * `authenticated=0` 不得携带身份串（须写 `-`）。
//   * **同一帧内不允许两个完全相同的 bbox**（真值身份归属未定义）。
//   * **至少一帧**：空脚本会被解析器拒绝，故此处直接拒绝，宁可不写也不产出一份
//     本系统读不回来的空文件（有帧但该帧零检测是合法的）。
//   * 注释行以 `#` 起始；空行忽略；容忍行尾 `\r`。
//
// **为什么用自定义 POD 而非 cv::Rect**：本序列化器要编进 core_unit_tests
// （RK_SKIP_OPENCV=ON），该 target 链接的是 deps/opencv 下的 mock stub——mock 的
// cv::Rect **没有 operator==**，且核心文本格式本就不该依赖 OpenCV。故此处只留整数字段。
struct ReplayDetection {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool authenticated = false;  // 已认证 → identityId 必非空；未认证 → 留空
    std::string identityId;      // 未认证时留空；写出时自动编码为 '-'
};

struct ReplayFrame {
    int frameIndex = 0;
    long long tsMs = 0;
    bool detectionRan = true;
    std::vector<ReplayDetection> detections;
};

using ReplayScriptData = std::vector<ReplayFrame>;

// 序列化到 os。成功返回 true；失败返回 false 并置 err（含具体帧号/检测序号与原因）。
// headerNote 非空时在文件首部写出 `#` 注释行（抽稀率、来源视频等溯源信息写这里；
// 解析器忽略 `#` 行，故不影响兼容性）。⚠️ headerNote 应传**不含 `#` 前缀**的原文——
// 前缀由本函数统一逐行添加；传已带 `#` 的文本会写出 `# # …`（仍是注释、不破坏兼容，
// 但属于调用方用法错误。CLI 侧传的是 `source=<path>` 这样的裸文本）。
// 校验**先于**任何写出——返回 false 时调用方应
// 丢弃其已落盘内容（本函数不做磁盘回滚，那属于调用方的职责）。
bool writeReplayScript(std::ostream& os, const ReplayScriptData& script,
                       const std::string& headerNote, std::string& err);

// 无注释头的便捷重载。
bool writeReplayScript(std::ostream& os, const ReplayScriptData& script, std::string& err);

}  // namespace pipeline
