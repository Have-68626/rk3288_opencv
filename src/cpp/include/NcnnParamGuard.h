#pragma once

// S-SEC-B10-001 ncnn .param 前置校验守卫
//
// 背景：本仓库 Android/Windows 构建所依赖的 ncnn 20260113 受 CVE-2026-50144
// （GHSA-jxmc-3mv6-7pwr）影响——ParamDict::load_param 仅校验 key id 上限
// （id >= NCNN_MAX_PARAM_COUNT，=32），未校验下限；负 id（[-23299,-1]）绕过
// is_array 归一化后直接作为 params[NCNN_MAX_PARAM_COUNT] 的下标，产生可控偏移的
// 越界堆写（CWE-787）。上游尚无含修复的官方发布（20260526 即公告确认的脆弱提交
// e54f7b1），因此在本仓库模型加载路径前置文本校验，镜像上游修复语义
// （`id < 0 || id >= NCNN_MAX_PARAM_COUNT`），并额外封堵公告另列的数组长度
// 超上限 huge-alloc 弱点。
//
// 语义与 ncnn ParamDict::load_param 一致：
//   - key 形如 `[-\d]*\d=`（`%d=` 扫描）；
//   - id <= -23300：数组 key，归一化 id = -id - 23300（值随后以逗号分隔，首个整数为长度）；
//   - id ∈ [-23299, -1]：负标量 id（CVE 越界触发点）→ 拒绝；
//   - 归一化后 id < 0 或 id >= 32 → 拒绝；
//   - 数组长度 len < 0 或 > kNcnnMaxParamArrayLen → 拒绝（huge-alloc DoS）。
//
// 本文件为纯标准库实现（不依赖 ncnn/OpenCV），可随 core_unit_tests 在
// RK_SKIP_OPENCV=ON 的 CI 下编译运行。加载真实模型 param 前的调用示例：
//   std::string err;
//   if (!rk_core::validateNcnnParamFile(paramPath.c_str(), err)) { /* 拒绝加载 */ }

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

namespace rk_core {

// ncnn NCNN_MAX_PARAM_COUNT（src/paramdict.h）
inline constexpr int kNcnnMaxParamCount = 32;
// ncnn 数组 key 归一化阈值（id <= -23300 视为数组 key）
inline constexpr long long kNcnnArrayKeyThreshold = -23300;
// 数组长度上限：封堵公告另列的 create(len) huge-alloc DoS；真实模型 param
// 数组（卷积核/步长/膨胀等配置）长度均远小于该值，无误拒风险
inline constexpr long long kNcnnMaxParamArrayLen = 1 << 20;

// 校验 .param 文本内容；拒绝返回 false 并填充 err
inline bool validateNcnnParamText(const std::string& text, std::string& err) {
    const std::size_t n = text.size();
    std::size_t pos = 0;
    int lineNo = 1;

    while (pos < n) {
        const char c = text[pos];
        if (c == '\n') {
            ++lineNo;
            ++pos;
            continue;
        }
        if (c == '\r') {
            ++pos;
            continue;
        }

        // 候选 key token 起始：符号 + 数字 + '='
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t i = pos;
            bool neg = false;
            if (i < n && text[i] == '-') {
                neg = true;
                ++i;
            }
            if (i >= n || text[i] < '0' || text[i] > '9') {
                ++pos;
                continue;
            }
            // 解析数字并钳制，避免超大数溢出（超上限必然拒绝）
            long long v = 0;
            while (i < n && text[i] >= '0' && text[i] <= '9') {
                if (v <= 1000000000LL) {
                    v = v * 10 + (text[i] - '0');
                }
                ++i;
            }
            if (i < n && text[i] == '=') {
                const long long rawId = neg ? -v : v;
                const bool isArray = rawId <= kNcnnArrayKeyThreshold;
                // 镜像 ncnn `id = -id - 23300`：kNcnnArrayKeyThreshold 为负，故为加法
                const long long idx = isArray ? (-rawId + kNcnnArrayKeyThreshold) : rawId;
                if (idx < 0 || idx >= kNcnnMaxParamCount) {
                    std::ostringstream oss;
                    oss << "param key id=" << rawId << " (normalized " << idx
                        << ") out of range [0," << kNcnnMaxParamCount
                        << ") at line " << lineNo << " [CVE-2026-50144]";
                    err = oss.str();
                    return false;
                }
                pos = i + 1;  // 越过 '='

                if (isArray) {
                    // 读取数组长度（huge-alloc DoS 封堵）
                    std::size_t j = pos;
                    if (j >= n || text[j] < '0' || text[j] > '9') {
                        err = "array param missing length at line " + std::to_string(lineNo);
                        return false;
                    }
                    long long len = 0;
                    while (j < n && text[j] >= '0' && text[j] <= '9') {
                        if (len <= kNcnnMaxParamArrayLen) {
                            len = len * 10 + (text[j] - '0');
                        }
                        ++j;
                    }
                    if (len <= 0 || len > kNcnnMaxParamArrayLen) {
                        std::ostringstream oss;
                        oss << "array param id=" << rawId << " length=" << len
                            << " exceeds cap " << kNcnnMaxParamArrayLen << " at line " << lineNo;
                        err = oss.str();
                        return false;
                    }
                    pos = j;  // 值列表以逗号分隔，不含 '='，由循环继续扫描
                }
                continue;
            }
        }
        ++pos;
    }
    return true;
}

// 校验 .param 文件（文本读取后交 validateNcnnParamText）
inline bool validateNcnnParamFile(const char* path, std::string& err) {
    if (path == nullptr || *path == '\0') {
        err = "param path is empty";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        err = std::string("cannot open param file: ") + path;
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return validateNcnnParamText(buf.str(), err);
}

}  // namespace rk_core
