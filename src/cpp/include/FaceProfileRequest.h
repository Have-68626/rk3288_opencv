#pragma once

#include "FaceInferencePipeline.h"
#include "FaceRuntimeProfile.h"

#include <string>

namespace rk_core {

// §I3（A16/A4/D4）单次管线统一装配：读 profileDir/manifest.json，parseV1 + verifyFiles
// （基目录 = profileDir）。
//
// 成功时**一次性提交**派生字段到 req（覆盖其模型装配相关字段，不按参数先后悄悄叠加）：
//   · detectorId = "scrfd_0.5gf"、embedderId = "arcface"
//   · yoloBackend = arcBackend = "ncnn"
//   · 四个模型文件**绝对路径**（det param/bin、emb param/bin）
//   · 实际节点名 / 输入尺寸 / 阈值 / 公开的模型版本
//   · int8Enabled =（detector 与 embedder 两个 precision 均为 "int8"）
// profile 同时经出参返回，供 SCRFD 侧 configFromProfile 消费全部解码字段（解码模式/anchor/layout 等，
// 这些语义无法用 FaceInferRequest 的字段完整表达，故必须回传 profile）。
//
// 失败（manifest 缺失/非法、模型文件校验失败、ABI 不匹配）→ 返回 false + err，**不改 req**。
bool applyFaceProfileToRequest(const std::string& profileDir, const std::string& currentAbi,
                               FaceInferRequest& req, FaceRuntimeProfile& profile, std::string& err);

} // namespace rk_core
