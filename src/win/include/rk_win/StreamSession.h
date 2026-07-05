#pragma once
#include "rk_win/StreamSessionRunner.h"

#include <string>

namespace rk_win {

class FramePipeline;

namespace stream {

/// 从 FramePipeline 构造 FrameProvider（含 OpenCV 依赖）
/// ppPipe: 指向 FramePipeline* 的指针（允许运行时为 nullptr，start() 后注入）
StreamSessionRunner::FrameProvider makeFrameProvider(FramePipeline** ppPipe);

/// 判断请求是否为流式端点（SSE / MJPEG）
bool isStreamRequest(const std::string& path, const std::string& method);

}  // namespace stream
}  // namespace rk_win
