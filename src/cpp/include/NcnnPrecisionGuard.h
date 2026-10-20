#pragma once

#include "NcnnParamGuard.h"

namespace rk_core {

// 文件名和 manifest precision 不能证明量化；只认真实权重层的 ncnn int8_scale_term。
// ncnn 允许混合图，故只要求至少一个卷积/全连接权重层带量化标志。
inline bool satisfiesInt8Requirement(const std::string& backend, const std::string& paramPath,
                                    std::string& err) {
    if (backend != "ncnn") {
        err = "int8_requires_ncnn";
        return false;
    }
    std::ifstream file(paramPath, std::ios::binary);
    if (!file) {
        err = "int8_param_unreadable";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    if (file.bad() || !validateNcnnParamText(text, err)) return false;
    std::istringstream lines(text);
    std::string line;
    std::getline(lines, line);  // magic 由后续 ncnn loader 校验。
    std::getline(lines, line);
    int layerCount = 0, blobCount = 0;
    std::istringstream header(line);
    if (!(header >> layerCount >> blobCount) || layerCount <= 0 || blobCount <= 0) {
        err = "int8_param_invalid_header";
        return false;
    }
    int parsedLayers = 0;
    // loader 只加载声明层数，尾部未加载的量化声明不能作为精度证据。
    while (parsedLayers < layerCount && std::getline(lines, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        ++parsedLayers;
        std::istringstream fields(line);
        std::string type, name, token;
        int bottoms = 0, tops = 0;
        if (!(fields >> type >> name >> bottoms >> tops) || bottoms < 0 || tops < 0) continue;
        for (int i = 0; i < bottoms && fields; ++i) fields >> token;
        for (int i = 0; i < tops && fields; ++i) fields >> token;
        if (type != "Convolution" && type != "ConvolutionDepthWise" && type != "InnerProduct") continue;
        int scaleTerm = 0;
        while (fields >> token) {
            std::istringstream value(token);
            int key = 0, term = 0;
            char equal = 0, trailing = 0;
            // ncnn 的 %d= 接受前导零和正号；遇到非参数内容即停止，不能扫描注释中的假标志。
            if (!(value >> key >> equal) || equal != '=') break;
            // 数组参数不能证明标量量化开关。
            if (key == -23308) { scaleTerm = 0; continue; }
            if (key != 8) continue;
            // ParamDict 对重复 key 采用最后一个值，不能把前面的 8>0 误作最终量化声明。
            scaleTerm = ((value >> term) && !(value >> trailing)) ? term : 0;
        }
        if (scaleTerm > 0) return true;
    }
    err = "int8_required_param_not_quantized";
    return false;
}

}  // namespace rk_core
