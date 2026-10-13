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

// ★ 2026-09-28 新增（外部审查 [P2]「异常检测框可触发整数转换或加法溢出」+ 独立复核：
//   **本文件才是该缺陷最先命中的地方** —— `mapX/mapY` 的结果仅要求「有限」，而 SCRFD 的
//   distance/delta 回归分量**无上界**，畸形/敌意模型可给出 ~1e9 的**有限**值）。
//   `std::lround` 在结果超出 int 范围时**返回值由实现定义/未指定**，随后的 `static_cast<int>`
//   更是 UB；且相邻两个端点若各自贴在 int 边界附近，「端点差」（int 减法）会溢出。
//   ⇒ 这里把端点夹进 ±INT_MAX/2：**既**保证单点可转 int，**又**保证任意两点之差 ≤ INT_MAX。
//   ⚠️ 夹取之后仍用 `lround`（**不是**截断）—— 见 `mapToIntClamped` 内注释。
constexpr double kScrfdIntHalf = 1073741823.0;  // = INT_MAX / 2

int mapToIntClamped(float v) {
    const double d = std::min(std::max(static_cast<double>(v), -kScrfdIntHalf), kScrfdIntHalf);
    // ★ 必须保留原实现的**就近取整**语义：本批初版此处写成 `static_cast<int>(d)`（向零**截断**），
    //   等于**静默**把检测框坐标改了最多 1px —— 由验收代理查出，且当时**无任何用例可判别**
    //   （`test_scrfd_decoder.cpp` 的断言输入全为整数，截断与取整同值）。
    //   夹取到 ±INT_MAX/2 之后 `lround` 的结果必然落在 long 内 ⇒ 转 int 安全。
    return static_cast<int>(std::lround(d));
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

    // golden 实证（2026-09-03）：真实 SCRFD 是每位置 N anchor + distance 语义。
    const bool distance = (opt.mode == DecodeMode::kDistance);
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
        const int N = distance ? opt.numAnchors[l] : 1;
        if (N <= 0) {
            err = "scrfd 层(" + std::to_string(opt.stride[l]) + ")anchor 数非法: " + std::to_string(N);
            return {};
        }
        if (distance) {
            // canonical distance 契约（golden 实证）：每位置 N anchor，score=N、bbox=4N、kps=10N
            if (S.channels != N) {
                err = "scrfd score 通道非法: " + std::to_string(S.channels) + "（期望 " + std::to_string(N) + "）";
                return {};
            }
            if (B.channels != 4 * N) {
                err = "scrfd bbox 通道非法: " + std::to_string(B.channels) + "（期望 " + std::to_string(4 * N) + "）";
                return {};
            }
            if (K.channels != 10 * N) {
                err = "scrfd kps 通道非法: " + std::to_string(K.channels) + "（期望 " + std::to_string(10 * N) + "）";
                return {};
            }
        } else {
            // legacy delta：单 anchor（隐含 N=1），score∈{1,2}、bbox=4、kps=10
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
        }
        const int featW = static_cast<int>(S.featW);
        const int featH = static_cast<int>(S.featH);
        if (B.featW != featW || B.featH != featH || K.featW != featW || K.featH != featH) {
            err = "scrfd 层(" + std::to_string(opt.stride[l]) + ")特征图尺寸不一致";
            return {};
        }
        // dataLen 越界读防护：形状声明的字节必须真实存在（A2 M2）
        const std::size_t needScore = static_cast<std::size_t>(featW) * featH * S.channels;
        const std::size_t needBbox = static_cast<std::size_t>(featW) * featH * B.channels;
        const std::size_t needKps = static_cast<std::size_t>(featW) * featH * K.channels;
        if ((S.dataLen > 0 && S.dataLen < needScore) ||
            (B.dataLen > 0 && B.dataLen < needBbox) ||
            (K.dataLen > 0 && K.dataLen < needKps)) {
            err = "scrfd 层(" + std::to_string(opt.stride[l]) + ")张量数据长度不足";
            return {};
        }
        anyLayer = true;

        const int s = opt.stride[l];
        const float aBase = static_cast<float>(s) * opt.anchorScale[l];  // delta 模式 anchor 尺寸基线
        for (int fy = 0; fy < featH; fy++) {
            for (int fx = 0; fx < featW; fx++) {
                const std::size_t pos = static_cast<std::size_t>(fy) * featW + fx;
                // anchor 中心：canonical 布局统一按 [pos][anchor] 行连续（适配器已归一化）
                const float anchorCx = (fx + 0.5f) * s;
                const float anchorCy = (fy + 0.5f) * s;
                for (int a = 0; a < N; a++) {
                    const std::size_t rowBase = pos * static_cast<std::size_t>(N) + a;
                    const float fg = distance
                                         ? S.data[rowBase]  // distance：每 anchor 单 sigmoid
                                         : (S.channels == 2 ? S.data[rowBase * 2 + 1] : S.data[rowBase]);
                    if (!(fg >= opt.confThreshold)) continue;

                    const float* b = B.data + rowBase * 4;
                    const float d0 = b[0], d1 = b[1], d2 = b[2], d3 = b[3];
                    // 回归分量 NaN/Inf → 丢弃（防 (int) 转型 UB；与 Retina 守卫同模式，A2 M1）
                    if (!(std::isfinite(d0) && std::isfinite(d1) &&
                          std::isfinite(d2) && std::isfinite(d3))) {
                        continue;
                    }
                    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
                    if (distance) {
                        // 官方语义（golden 查证 2026-09-03：scrfd.py / nihui scrfd.cpp）：
                        //   中心 = 网格·stride（无 +0.5）；bbox 距离分量须乘 stride
                        //   （官方 forward 做 bbox_preds*stride → distance2bbox）。
                        const float dcx = static_cast<float>(fx) * s;
                        const float dcy = static_cast<float>(fy) * s;
                        x1 = dcx - d0 * s;
                        y1 = dcy - d1 * s;
                        x2 = dcx + d2 * s;
                        y2 = dcy + d3 * s;
                    } else {
                        // legacy delta：偏移 + exp（历史兼容）
                        const float cx = anchorCx + d0 * s;
                        const float cy = anchorCy + d1 * s;
                        const float bw = aBase * std::exp(d2);
                        const float bh = aBase * std::exp(d3);
                        if (!(std::isfinite(bw) && std::isfinite(bh) && bw > 0.0f && bh > 0.0f)) {
                            continue;
                        }
                        x1 = cx - bw / 2;
                        y1 = cy - bh / 2;
                        x2 = cx + bw / 2;
                        y2 = cy + bh / 2;
                    }

                    ScrfdDetection d;
                    d.score = fg;
                    // 关键点先求值到栈数组并校验有限性（与 bbox 守卫同模式，A2-Med）：
                    // kp 偏移 NaN/Inf 会经 distance×stride/delta 注入 FaceDetection::keypoints5 → 下游 FaceAlign 污染
                    const float dcx = distance ? static_cast<float>(fx) * s : 0.0f;
                    const float dcy = distance ? static_cast<float>(fy) * s : 0.0f;
                    Keypoint pts[5];
                    bool kpOk = true;
                    const float* kp = K.data + rowBase * 10;
                    for (int k = 0; k < 5; k++) {
                        if (distance) {
                            // distance：关键点 = 中心 + 偏移·stride（官方 kps_preds*stride → distance2kps）
                            pts[k] = Keypoint{
                                mapX(dcx + kp[k * 2] * s, opt),
                                mapY(dcy + kp[k * 2 + 1] * s, opt)};
                        } else {
                            pts[k] = Keypoint{
                                mapX(kp[k * 2], opt), mapY(kp[k * 2 + 1], opt)};
                        }
                        if (!(std::isfinite(pts[k].x) && std::isfinite(pts[k].y))) kpOk = false;
                    }
                    if (!kpOk) continue;  // 关键点含 NaN/Inf → 丢弃该检测
                    d.keypoints.reserve(5);
                    for (int k = 0; k < 5; k++) d.keypoints.push_back(pts[k]);
                    const float mx1 = mapX(x1, opt), my1 = mapY(y1, opt);
                    const float mx2 = mapX(x2, opt), my2 = mapY(y2, opt);
                    if (!(std::isfinite(mx1) && std::isfinite(my1) &&
                          std::isfinite(mx2) && std::isfinite(my2))) {
                        continue;
                    }
                    // ★ 2026-09-28 修：原为 `static_cast<int>(std::lround(mx1))` + `ox2 - ox1`（int 减法）。
                    //   lround 超范围为实现定义、转 int 为 UB；端点差还会 int 溢出。（详见文件上方 helper 注释）
                    const int ox1 = mapToIntClamped(mx1);
                    const int oy1 = mapToIntClamped(my1);
                    const int ox2 = mapToIntClamped(mx2);
                    const int oy2 = mapToIntClamped(my2);
                    d.bbox = cv::Rect(
                        ox1, oy1,
                        static_cast<int>(std::max<long long>(
                            1, static_cast<long long>(ox2) - static_cast<long long>(ox1))),
                        static_cast<int>(std::max<long long>(
                            1, static_cast<long long>(oy2) - static_cast<long long>(oy1))));
                    dets.push_back(std::move(d));
                }
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
        // ★ 2026-09-28 修：`d.bbox.x + d.bbox.width` 是 **int 加法**，端点接近 int 边界时溢出 UB。
        //   改为 64 位求和后再夹到原图边界（result ≤ origWidth/Height，故回到 int 是安全的）。
        const long long bx2 = static_cast<long long>(d.bbox.x) + d.bbox.width;
        const long long by2 = static_cast<long long>(d.bbox.y) + d.bbox.height;
        const int x1 = std::max(d.bbox.x, 0);
        const int y1 = std::max(d.bbox.y, 0);
        const int x2 = static_cast<int>(std::min<long long>(bx2, opt.origWidth));
        const int y2 = static_cast<int>(std::min<long long>(by2, opt.origHeight));
        d.bbox = cv::Rect(x1, y1, std::max(1, x2 - x1), std::max(1, y2 - y1));
        // 关键点随检测同步（不做几何裁剪）
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace pipeline