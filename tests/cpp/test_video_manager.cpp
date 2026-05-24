#include "VideoManager.h"
#include <string>

bool test_video_manager_is_url_source_valid_urls() {
    if (!VideoManager::isUrlSource("http://example.com/video.mp4")) return false;
    if (!VideoManager::isUrlSource("https://example.com/video.mp4")) return false;
    if (!VideoManager::isUrlSource("rtsp://192.168.1.100:554/stream")) return false;
    if (!VideoManager::isUrlSource("rtmp://live.twitch.tv/app/stream_key")) return false;
    return true;
}

bool test_video_manager_is_url_source_local_paths() {
    if (VideoManager::isUrlSource("/home/user/video.mp4")) return false;
    if (VideoManager::isUrlSource("C:\\Users\\user\\video.mp4")) return false;
    if (VideoManager::isUrlSource("video.mp4")) return false;
    if (VideoManager::isUrlSource("./video.mp4")) return false;
    if (VideoManager::isUrlSource("../video.mp4")) return false;
    return true;
}

bool test_video_manager_is_url_source_edge_cases() {
    if (VideoManager::isUrlSource("")) return false;
    if (VideoManager::isUrlSource("http")) return false; // Shorter than prefix
    if (VideoManager::isUrlSource("http:")) return false;
    if (VideoManager::isUrlSource("http:/")) return false;

    // Prefix not at the beginning
    if (VideoManager::isUrlSource(" file://http://example.com")) return false;
    if (VideoManager::isUrlSource("my_http://example.com")) return false;
    if (VideoManager::isUrlSource("/some/path/http://example.com")) return false;

    // Case sensitivity (current implementation is case-sensitive and only checks lowercase)
    // If the requirements change to case-insensitive, this test will fail and need to be updated.
    if (VideoManager::isUrlSource("HTTP://example.com/video.mp4")) return false;
    if (VideoManager::isUrlSource("RTSP://192.168.1.100/stream")) return false;

    return true;
}
