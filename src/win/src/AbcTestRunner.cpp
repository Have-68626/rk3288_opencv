#include "rk_win/AbcTestRunner.h"

#include "rk_win/EventLogger.h"
#include "rk_win/FramePipeline.h"

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace rk_win {
namespace {

std::string nowStamp() {
    using clock = std::chrono::system_clock;
    const auto t = clock::to_time_t(clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

#ifdef _WIN32
bool httpGetLocal(int port, const std::string& path, std::string& outResp) {
    outResp.clear();
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    DWORD recvTimeoutMs = 2000;
    DWORD sendTimeoutMs = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeoutMs), sizeof(recvTimeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        WSACleanup();
        return false;
    }

    std::string req;
    req += "GET " + path + " HTTP/1.1\r\n";
    req += "Host: 127.0.0.1\r\n";
    req += "Connection: close\r\n";
    req += "\r\n";
    send(s, req.c_str(), static_cast<int>(req.size()), 0);

    char buf[2048];
    for (;;) {
        int r = recv(s, buf, static_cast<int>(sizeof(buf)), 0);
        if (r <= 0) break;
        outResp.append(buf, buf + r);
        if (outResp.size() > 256000) break;
    }

    closesocket(s);
    WSACleanup();
    return !outResp.empty();
}

bool sseReadSome(int port, const std::string& path, int seconds, int& eventsOut) {
    eventsOut = 0;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    DWORD recvTimeoutMs = 2000;
    DWORD sendTimeoutMs = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeoutMs), sizeof(recvTimeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        WSACleanup();
        return false;
    }

    std::string req;
    req += "GET " + path + " HTTP/1.1\r\n";
    req += "Host: 127.0.0.1\r\n";
    req += "Accept: text/event-stream\r\n";
    req += "Connection: close\r\n";
    req += "\r\n";
    send(s, req.c_str(), static_cast<int>(req.size()), 0);

    auto endT = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, seconds));
    std::string bufAll;
    bufAll.reserve(4096);
    char buf[2048];
    while (std::chrono::steady_clock::now() < endT) {
        int r = recv(s, buf, static_cast<int>(sizeof(buf)), 0);
        if (r > 0) {
            bufAll.append(buf, buf + r);
            size_t pos = 0;
            while ((pos = bufAll.find("data:")) != std::string::npos) {
                eventsOut++;
                bufAll.erase(0, pos + 5);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (eventsOut >= 2) break;
    }

    closesocket(s);
    WSACleanup();
    return eventsOut > 0;
}
#endif

}  // namespace

AbcTestRunner::AbcTestRunner() = default;

AbcTestRunner::~AbcTestRunner() {
    stop();
}

bool AbcTestRunner::start(void* hwndMain, FramePipeline* pipe, EventLogger* events, const AbcTestConfig& cfg) {
    stop();
    if (!hwndMain || !pipe) return false;
    hwndMain_ = hwndMain;
    pipe_ = pipe;
    events_ = events;
    cfg_ = cfg;
    running_ = true;
    thread_ = std::thread(&AbcTestRunner::run, this);
    return true;
}

void AbcTestRunner::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AbcTestRunner::run() {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(hwndMain_);
    std::error_code ec;
    std::filesystem::create_directories(cfg_.reportDir, ec);
    const std::filesystem::path reportPath = cfg_.reportDir / ("TEST_REPORT_ABC_" + nowStamp() + ".md");
    std::ofstream ofs(reportPath, std::ios::binary);
    if (!ofs) return;

    if (events_) events_->append("test_abc_start", reportPath.string());

    ofs << "# WCFR 测试报告（A/B/C）\n\n";
    ofs << "- 报告时间: " << nowStamp() << "\n";
    ofs << "- A(UI/布局/宽高比): " << (cfg_.testA ? "开启" : "关闭") << "\n";
    ofs << "- B(相机切换): " << (cfg_.testB ? "开启" : "关闭") << "\n";
    ofs << "- C(输出端口): " << (cfg_.testC ? "开启" : "关闭") << "\n\n";

    if (cfg_.testA && running_) {
        ofs << "## A. UI/布局/宽高比切换\n\n";
        const int comboScaleId = 1010;
        HWND combo = GetDlgItem(hwnd, comboScaleId);
        const std::vector<std::pair<int, int>> sizes = {{800, 600}, {1280, 720}, {1920, 1080}, {500, 360}};
        bool okA = true;
        for (const auto& s : sizes) {
            if (!running_) break;
            SetWindowPos(hwnd, nullptr, 0, 0, s.first, s.second, SWP_NOMOVE | SWP_NOZORDER);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            for (int mode = 0; mode <= 1; mode++) {
                if (!combo) continue;
                SendMessageW(combo, CB_SETCURSEL, mode, 0);
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(comboScaleId, CBN_SELCHANGE), (LPARAM)combo);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                FacesSnapshot snap;
                if (!pipe_ || !pipe_->snapshotFaces(snap)) {
                    okA = false;
                    continue;
                }
                if (snap.previewScaleMode != mode) okA = false;
                if (snap.previewWidth <= 0 || snap.previewHeight <= 0) okA = false;

                RenderState rs;
                if (pipe_->tryGetRenderState(rs)) {
                    const cv::Scalar m = cv::mean(rs.bgr);
                    const double avg = (m[0] + m[1] + m[2]) / 3.0;
                    if (avg < 1.0) okA = false;
                } else {
                    okA = false;
                }
            }
        }
        ofs << "- 结果: " << (okA ? "通过" : "失败") << "\n\n";
    }

    if (cfg_.testB && running_) {
        ofs << "## B. 相机分辨率/FPS 切换回归\n\n";
        const int cameraComboId = 1001;
        int camIndex = 0;
        HWND camCombo = GetDlgItem(hwnd, cameraComboId);
        if (camCombo) {
            const int sel = static_cast<int>(SendMessageW(camCombo, CB_GETCURSEL, 0, 0));
            if (sel >= 0) camIndex = sel;
        }

        struct Item {
            int w;
            int h;
            int fps;
        };
        const std::vector<Item> plan = {
            {1920, 1080, 15}, {1920, 1080, 30}, {1920, 1080, 60}, {1280, 720, 15}, {1280, 720, 30}, {1280, 720, 60}, {640, 480, 15}, {640, 480, 30}, {640, 480, 60},
        };

        int okCount = 0;
        int failCount = 0;
        std::int64_t maxMs = 0;
        std::int64_t sumMs = 0;
        for (int r = 0; r < std::max(1, cfg_.repeatsB) && running_; r++) {
            for (const auto& it : plan) {
                if (!running_) break;
                const auto res = pipe_->applyCameraSettings(camIndex, it.w, it.h, it.fps, 300);
                if (res.ok) {
                    okCount++;
                    maxMs = std::max(maxMs, res.totalMs);
                    sumMs += res.totalMs;
                } else {
                    failCount++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            }
        }
        const int total = okCount + failCount;
        const double avgMs = (okCount > 0) ? (static_cast<double>(sumMs) / static_cast<double>(okCount)) : 0.0;
        ofs << "- 总次数: " << total << "\n";
        ofs << "- 成功: " << okCount << "\n";
        ofs << "- 失败: " << failCount << "\n";
        ofs << "- 最大耗时(ms): " << maxMs << "\n";
        ofs << "- 平均耗时(ms): " << avgMs << "\n\n";
    }

    if (cfg_.testC && running_) {
        ofs << "## C. 输出端口（HTTP + SSE）\n\n";
        bool okGet = false;
        bool okSse = false;
#ifdef _WIN32
        std::string resp;
        if (httpGetLocal(cfg_.httpPort, "/api/faces", resp)) {
            okGet = (resp.find("200 OK") != std::string::npos) && (resp.find("\"faces\"") != std::string::npos);
        }
        int ev = 0;
        okSse = sseReadSome(cfg_.httpPort, "/api/faces/stream", cfg_.secondsC, ev);
        ofs << "- GET /api/faces: " << (okGet ? "通过" : "失败") << "\n";
        ofs << "- SSE /api/faces/stream: " << (okSse ? "通过" : "失败") << "（events=" << ev << "）\n\n";
#else
        ofs << "- 当前平台不支持\n\n";
#endif
    }

    ofs.flush();
    if (events_) events_->append("test_abc_done", reportPath.string());
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
#endif
}

}  // namespace rk_win

