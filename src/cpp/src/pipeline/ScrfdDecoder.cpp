#include "pipeline/ScrfdDecoder.h"

#include <algorithm>
#include <cmath>

namespace pipeline {

namespace {

// letterbox 坐标（输入 640×640 空间）→ 原图坐标
float mapX(float v, const ScrfdDecodeOptions& opt) {
    return (v - opt.padX) / opt.scale;
}

float mapY(float v, const ScrfdDecodeOptions& opt) {
    return (v - opt.padY) / opt.scale;
}

}  // namespace

std::vector<ScrfdDetection> ScrfdDecoder::decode(
    const ScrfdTensor score[3], const ScrfdTensor bbox[3], const ScrfdTensor kps[3],
    const ScrfdDecodeOptions& opt, std::string& err) {
    err.clear();
    if (!(opt.scale > 0.0f) || opt.origWidth <= 0 || opt.origHeight <= 0) {
        err = "scrfd 无效的原图尺寸/scale";
        return {};
    }

    std::vector<ScrfdDetection> dets;
    bool anyLayer = false;
    for (int l = 0; l < 3; l++) {
        const ScrfdTensor& S = score[l];
        const ScrfdTensor& B = bbox[l];
        const ScrfdTensor& K = kps[l];
        if (!S.valid() && !B.valid() && !K.valid()) continue;  // 该 stride 未提供

        // 形状校验：三层必须同时到位，且通道数符合契约；缺失或非法即拒绝，不猜测解码
        if (!(S.valid() && B.valid() && K.valid())) {
            err = "scrfd 层(" + std::to_string(opt.stride[l]) + ")张量缺失，拒绝猜测解码";
            return {};
        }
        if (S.channels != 1 && S.channels != 2) {
            err = "scrfd score 通道非法: " + std::to_string(S.channels);
            return {};
        }
        if (B.channels != 4) {
            err = "scrfd bbox 通道非法: " + std::to_string(B.channels);
            return {};
        }
        if (K.channels != 10) {
            err = "scrfd kps 通道非法: " + std::to_string(K.channels);
            return {};
        }
        const int featW = static_cast<int>(S.featW);
        const int featH = static_cast<int>(S.featH);
        if (B.featW != featW || B.featH != featH || K.featW != featW || K.featH != featH) {
            err = "scrfd 层(" + std::to_string(opt.stride[l]) + ")特征图尺寸不一致";
            return {};
        }
        // dataLen 越界读防护：形状声明的字节必须真实存在（A2 M2）
        const std::size_t needScore = static_cast<std::size_t>(featW) * featH * S.channels;
        const std::size_t needBbox = static_cast<std::size_t>(featW) * featH * 4;
        const std::size_t needKps = static_cast<std::size_t>(featW) * featH * 10;
        if ((S.dataLen > 0 && S.dataLen < needScore) ||
            (B.dataLen > 0 && B.dataLen < needBbox) ||
            (K.dataLen > 0 && K.dataLen < needKps)) {
            err = "scrfd 层(" + std::to_string(opt.stride[l]) + ")张量数据长度不足";
            return {};
        }
        anyLayer = true;

        const int s = opt.stride[l];
        const float aBase = static_cast<float>(s) * opt.anchorScale[l];  // anchor 尺寸基线
        for (int fy = 0; fy < featH; fy++) {
            for (int fx = 0; fx < featW; fx++) {
                const std::size_t idx = static_cast<std::size_t>(fy) * featW + fx;
                const float fg = (S.channels == 2)
                                     ? S.data[idx * 2 + 1]
                                     : S.data[idx * 1];
                if (!(fg >= opt.confThreshold)) continue;

                const float* b = B.data + idx * 4;
                const float dx = b[0], dy = b[1], dw = b[2], dh = b[3];
                // 回归分量 NaN/Inf → 丢弃（防 (int) 转型 UB；与 Retina 守卫同模式，A2 M1）
                if (!(std::isfinite(dx) && std::isfinite(dy) &&
                      std::isfinite(dw) && std::isfinite(dh))) {
                    continue;
                }
                // anchor 中心：简化 anchor 网格（每位置单 anchor），基线尺寸 = stride×anchorScale
                const float anchorCx = (fx + 0.5f) * s;
                const float anchorCy = (fy + 0.5f) * s;
                const float cx = anchorCx + dx * s;
                const float cy = anchorCy + dy * s;
                const float bw = aBase * std::exp(dw);
                const float bh = aBase * std::exp(dh);
                if (!(std::isfinite(bw) && std::isfinite(bh) && bw > 0.0f && bh > 0.0f)) {
                    continue;
                }
                const float x1 = cx - bw / 2, y1 = cy - bh / 2;
                const float x2 = cx + bw / 2, y2 = cy + bh / 2;

                ScrfdDetection d;
                d.score = fg;
                d.keypoints.reserve(5);
                const float* kp = K.data + idx * 10;
                for (int k = 0; k < 5; k++) {
                    d.keypoints.emplace_back(Keypoint{mapX(kp[k * 2], opt), mapY(kp[k * 2 + 1], opt)});
                }
                const float mx1 = mapX(x1, opt), my1 = mapY(y1, opt);
                const float mx2 = mapX(x2, opt), my2 = mapY(y2, opt);
                if (!(std::isfinite(mx1) && std::isfinite(my1) &&
                      std::isfinite(mx2) && std::isfinite(my2))) {
                    continue;
                }
                const int ox1 = static_cast<int>(std::lround(mx1));
                const int oy1 = static_cast<int>(std::lround(my1));
                const int ox2 = static_cast<int>(std::lround(mx2));
                const int oy2 = static_cast<int>(std::lround(my2));
                d.bbox = cv::Rect(ox1, oy1,
                                  std::max(1, ox2 - ox1), std::max(1, oy2 - oy1));
                dets.push_back(std::move(d));
            }
        }
    }
    if (!anyLayer) {
        err = "scrfd 无有效输出层";
        return {};
    }

    // 统一后处理（NMS/包含抑制/maxFaces）并把保留索引同步回关键点
    std::vector<DetectedFace> faces;
    faces.reserve(dets.size());
    for (const auto& d : dets) {
        DetectedFace f;
        f.bbox = d.bbox;
        f.confidence = d.score;
        faces.push_back(f);
    }
    std::vector<std::size_t> kept =
        FacePostProcessor::keptIndices(faces, opt.origWidth, opt.origHeight, opt.post);
    std::vector<ScrfdDetection> out;
    out.reserve(kept.size());
    for (std::size_t k : kept) {
        ScrfdDetection d = dets[k];  // 顺序 == process 输出顺序
        // 与 FacePostProcessor::process 对齐：保留项裁剪到原图边界（关键点不变）
        const int x1 = std::max(d.bbox.x, 0);
        const int y1 = std::max(d.bbox.y, 0);
        const int x2 = std::min(d.bbox.x + d.bbox.width, opt.origWidth);
        const int y2 = std::min(d.bbox.y + d.bbox.height, opt.origHeight);
        d.bbox = cv::Rect(x1, y1, std::max(1, x2 - x1), std::max(1, y2 - y1));
        // 关键点随检测同步（不做几何裁剪）
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace pipeline