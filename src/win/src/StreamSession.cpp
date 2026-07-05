#include "rk_win/StreamSession.h"
#include "rk_win/FramePipeline.h"

namespace rk_win {
namespace stream {

StreamSessionRunner::FrameProvider makeFrameProvider(FramePipeline** ppPipe) {
    return [ppPipe](cv::Mat& out) -> bool {
        FramePipeline* pipe = *ppPipe;
        if (!pipe) return false;
        RenderState rs;
        if (!pipe->tryGetRenderState(rs)) return false;
        out = rs.bgr;
        return true;
    };
}

bool isStreamRequest(const std::string& path, const std::string& method) {
    if (method != "GET") return false;
    return path == "/api/faces/stream" ||
           path == "/api/v1/faces/stream" ||
           path == "/api/v1/preview.mjpeg";
}

}  // namespace stream
}  // namespace rk_win
