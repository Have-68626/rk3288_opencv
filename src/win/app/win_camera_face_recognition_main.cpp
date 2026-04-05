#ifdef _WIN32

#include "rk_win/D3D11Renderer.h"
#include "rk_win/DisplaySettings.h"
#include "rk_win/EventLogger.h"
#include "rk_win/FramePipeline.h"
#include "rk_win/AbcTestRunner.h"
#include "rk_win/HttpFacesPoster.h"
#include "rk_win/HttpFacesServer.h"
#include "rk_win/RenderMetricsLogger.h"
#include "rk_win/WinConfig.h"
#include "rk_win/WinJsonConfig.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace rk_win {

struct AppState {
    AppConfig cfg;
    WinJsonConfigStore settings;
    FramePipeline pipe;
    DisplaySettings displays;
    D3D11Renderer renderer;
    RenderMetricsLogger metrics;
    EventLogger events;
    HttpFacesServer http;
    HttpFacesPoster poster;
    AbcTestRunner abc;
    AbcTestConfig abcCfg;

    HWND hwndMain = nullptr;
    HWND hwndPreview = nullptr;
    int topControlsHeight = 0;

    int dpi = 96;
    double dpiScale = 1.0;

    bool rendererReady = false;
    bool displaysReady = false;

    std::chrono::steady_clock::time_point lastMetricsT{};
    std::chrono::steady_clock::time_point lastTitleT{};

    bool stabilityTest = false;
    bool soakTest = false;
    bool perfReport = false;
    int stabilityPerModeSec = 15;
    int soakHours = 72;
    int perfSeconds = 60;
    std::chrono::steady_clock::time_point testStart{};
    std::chrono::steady_clock::time_point nextSwitch{};
    std::chrono::steady_clock::time_point testEnd{};
    size_t stabilityIndex = 0;
    std::vector<std::pair<int, DisplayMode>> stabilityPlan;
};

AppState* g_app = nullptr;

bool setPerMonitorDpiAwareV2() {
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (!fn) return false;
    return fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == TRUE;
}

namespace {

constexpr int kIdComboCamera = 1001;
constexpr int kIdComboFormat = 1002;
constexpr int kIdCheckFlipX = 1003;
constexpr int kIdCheckFlipY = 1004;
constexpr int kIdStaticFps = 1005;
constexpr int kIdEditEnroll = 1006;
constexpr int kIdBtnEnroll = 1007;
constexpr int kIdBtnClear = 1008;
constexpr int kIdBtnPrivacy = 1009;
constexpr int kIdComboPreviewScale = 1010;
constexpr int kIdComboCamFps = 1011;
constexpr int kIdBtnApplyCamera = 1012;

constexpr int kIdComboDisplay = 1101;
constexpr int kIdComboResolution = 1102;
constexpr int kIdComboRefresh = 1103;
constexpr int kIdCheckVsync = 1104;
constexpr int kIdComboBuffers = 1105;
constexpr int kIdCheckFullscreen = 1106;
constexpr int kIdCheckSystemMode = 1107;
constexpr int kIdCheckSrgb = 1108;
constexpr int kIdEditGamma = 1109;
constexpr int kIdEditTempK = 1110;
constexpr int kIdComboAa = 1111;
constexpr int kIdComboAniso = 1112;
constexpr int kIdBtnApplyDisplay = 1113;
constexpr int kIdStaticFrameTime = 1114;

constexpr UINT_PTR kTimerRender = 1;
constexpr UINT_PTR kTimerMetrics = 2;

std::wstring wFromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string utf8FromW(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void setWindowTextUtf8(HWND hwnd, const std::string& s) {
    SetWindowTextW(hwnd, wFromUtf8(s).c_str());
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // 控制字符：最小实现用空格替代（避免生成非法 JSON）
                    out += ' ';
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// 统一的“落盘配置”入口（替代旧 INI 写入）：
// - 为什么用 patch JSON：settings API 与本地 UI 共享同一套更新/校验/加密/回滚逻辑，避免两套配置分叉；
// - 坑：patch 不是完整配置，可能触发 schema 校验失败；因此失败时必须提示用户并保持 UI/运行时回滚一致。
bool persistSettingsPatch(HWND hwnd, const std::string& patchJson, const wchar_t* failTitle) {
    if (!g_app) return false;
    const auto res = g_app->settings.updateFromJsonBody(patchJson);
    if (!res.ok) {
        g_app->cfg = g_app->settings.current();
        std::wstring msg = wFromUtf8(res.message);
        if (!res.details.empty()) msg += L"\n" + wFromUtf8(res.details.front());
        MessageBoxW(hwnd, msg.c_str(), failTitle ? failTitle : L"配置写入失败", MB_OK | MB_ICONERROR);
        return false;
    }
    g_app->cfg = g_app->settings.current();
    return true;
}

void fillCameraCombo(HWND combo, const std::vector<CameraDevice>& devs) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < devs.size(); i++) {
        const auto& d = devs[i];
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
    }
    if (!devs.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

std::wstring formatToText(const CameraFormat& f) {
    std::wstring s = std::to_wstring(f.width) + L"x" + std::to_wstring(f.height);
    if (f.fps > 0) s += L" @" + std::to_wstring(f.fps) + L"fps";
    return s;
}

void fillFormatCombo(HWND combo, const std::vector<CameraFormat>& formats) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < formats.size(); i++) {
        const auto text = formatToText(formats[i]);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
    }
    if (!formats.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

struct CamResPreset {
    int w = 0;
    int h = 0;
};

const CamResPreset kCamResPresets[] = {
    {1920, 1080},
    {1280, 720},
    {640, 480},
};

const int kCamFpsPresets[] = {15, 30, 60};

void fillCamResPresetCombo(HWND combo, int curW, int curH) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t i = 0; i < std::size(kCamResPresets); i++) {
        const auto& p = kCamResPresets[i];
        const std::wstring text = std::to_wstring(p.w) + L"x" + std::to_wstring(p.h);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        if (p.w == curW && p.h == curH) sel = static_cast<int>(i);
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void fillCamFpsPresetCombo(HWND combo, int curFps) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int sel = 1;
    for (size_t i = 0; i < std::size(kCamFpsPresets); i++) {
        const int fps = kCamFpsPresets[i];
        const std::wstring text = std::to_wstring(fps) + L"fps";
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        if (fps == curFps) sel = static_cast<int>(i);
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

CamResPreset readCamResPreset(HWND combo) {
    const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(std::size(kCamResPresets))) return kCamResPresets[2];
    return kCamResPresets[static_cast<size_t>(sel)];
}

int readCamFpsPreset(HWND combo) {
    const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(std::size(kCamFpsPresets))) return 30;
    return kCamFpsPresets[static_cast<size_t>(sel)];
}

int sPx(int px) {
    if (!g_app) return px;
    return static_cast<int>(static_cast<double>(px) * g_app->dpiScale + 0.5);
}

std::wstring modeToText(const DisplayMode& m) {
    const int hz = static_cast<int>(m.refreshHz() + 0.5);
    return std::to_wstring(m.width) + L"x" + std::to_wstring(m.height) + L" @" + std::to_wstring(hz) + L"Hz";
}

std::wstring refreshToText(const DisplayMode& m) {
    const int hz = static_cast<int>(m.refreshHz() + 0.5);
    return std::to_wstring(hz) + L"Hz";
}

std::wstring readTextW(HWND h) {
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring s(static_cast<size_t>(n), L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}

std::optional<double> tryParseDouble(const std::wstring& s) {
    if (s.empty()) return std::nullopt;
    try {
        return std::stod(s);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> tryParseInt(const std::wstring& s) {
    if (s.empty()) return std::nullopt;
    try {
        return std::stoi(s);
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::pair<int, int>> uniqueResolutions(const std::vector<DisplayMode>& modes) {
    std::vector<std::pair<int, int>> res;
    res.reserve(modes.size());
    for (const auto& m : modes) res.emplace_back(m.width, m.height);
    std::sort(res.begin(), res.end());
    res.erase(std::unique(res.begin(), res.end()), res.end());
    return res;
}

std::vector<DisplayMode> refreshModesForRes(const std::vector<DisplayMode>& modes, int w, int h) {
    std::vector<DisplayMode> out;
    for (const auto& m : modes) {
        if (m.width == w && m.height == h) out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const DisplayMode& a, const DisplayMode& b) { return a.refreshHz() < b.refreshHz(); });
    return out;
}

void fillDisplayCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (!g_app) return;
    const auto& outs = g_app->displays.outputs();
    for (size_t i = 0; i < outs.size(); i++) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)outs[i].name.c_str());
    }
    if (!outs.empty()) {
        int sel = std::clamp(g_app->cfg.display.outputIndex, 0, static_cast<int>(outs.size() - 1));
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
    }
}

void fillResolutionCombo(HWND combo, int outputIndex) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (!g_app) return;
    const auto& outs = g_app->displays.outputs();
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outs.size())) return;
    const auto res = uniqueResolutions(outs[static_cast<size_t>(outputIndex)].modes);
    int sel = 0;
    for (size_t i = 0; i < res.size(); i++) {
        const auto text = std::to_wstring(res[i].first) + L"x" + std::to_wstring(res[i].second);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        if (g_app->cfg.display.width == res[i].first && g_app->cfg.display.height == res[i].second) sel = static_cast<int>(i);
    }
    if (!res.empty()) SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void fillRefreshCombo(HWND combo, int outputIndex, int w, int h) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (!g_app) return;
    const auto& outs = g_app->displays.outputs();
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outs.size())) return;
    const auto rms = refreshModesForRes(outs[static_cast<size_t>(outputIndex)].modes, w, h);
    int sel = 0;
    for (size_t i = 0; i < rms.size(); i++) {
        const auto text = refreshToText(rms[i]);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        if (g_app->cfg.display.refreshNumerator == rms[i].refreshNumerator && g_app->cfg.display.refreshDenominator == rms[i].refreshDenominator) sel = static_cast<int>(i);
    }
    if (!rms.empty()) SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void fillBuffersCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"2");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"3");
    int sel = 0;
    if (g_app && g_app->cfg.display.swapchainBuffers == 3) sel = 1;
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void fillAaCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const std::vector<int> samples = g_app && g_app->rendererReady ? g_app->renderer.supportedMsaaSampleCounts() : std::vector<int>{1};
    int sel = 0;
    for (size_t i = 0; i < samples.size(); i++) {
        const auto text = std::to_wstring(samples[i]) + L"x";
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        if (g_app && g_app->cfg.display.aaSamples == samples[i]) sel = static_cast<int>(i);
    }
    if (!samples.empty()) SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void fillAnisoCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const std::vector<int> levels = {1, 2, 4, 8, 16};
    int sel = 0;
    for (size_t i = 0; i < levels.size(); i++) {
        const auto text = std::to_wstring(levels[i]) + L"x";
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        if (g_app && g_app->cfg.display.anisoLevel == levels[i]) sel = static_cast<int>(i);
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void syncDisplayControlsFromConfig(HWND hwnd) {
    if (!g_app) return;
    SendMessageW(GetDlgItem(hwnd, kIdCheckVsync), BM_SETCHECK, g_app->cfg.display.vsync ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd, kIdCheckFullscreen), BM_SETCHECK, g_app->cfg.display.fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd, kIdCheckSystemMode), BM_SETCHECK, g_app->cfg.display.allowSystemModeSwitch ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd, kIdCheckSrgb), BM_SETCHECK, g_app->cfg.display.enableSRGB ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(GetDlgItem(hwnd, kIdEditGamma), std::to_wstring(g_app->cfg.display.gamma).c_str());
    SetWindowTextW(GetDlgItem(hwnd, kIdEditTempK), std::to_wstring(g_app->cfg.display.colorTempK).c_str());
    fillBuffersCombo(GetDlgItem(hwnd, kIdComboBuffers));
    fillAaCombo(GetDlgItem(hwnd, kIdComboAa));
    fillAnisoCombo(GetDlgItem(hwnd, kIdComboAniso));
}

void layoutControls(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);

    const int pad = sPx(8);
    const int rowH = sPx(32);
    const int clientW = std::max(1, static_cast<int>(rc.right - rc.left));

    int x = pad;
    int y = pad;
    auto nextRow = [&]() {
        x = pad;
        y += rowH + pad;
    };
    auto ensureSpace = [&](int w) {
        if (x == pad) return;
        if (x + w > clientW - pad) nextRow();
    };
    auto place = [&](int id, int w, int h, int dy) {
        HWND c = GetDlgItem(hwnd, id);
        if (!c) return;
        ensureSpace(w);
        MoveWindow(c, x, y + dy, w, h, TRUE);
        x += w + pad;
    };

    place(kIdComboCamera, sPx(220), rowH, 0);
    place(kIdComboFormat, sPx(160), rowH, 0);
    place(kIdComboCamFps, sPx(70), rowH, 0);
    place(kIdBtnApplyCamera, sPx(90), rowH, 0);
    place(kIdComboPreviewScale, sPx(120), rowH, 0);
    place(kIdCheckFlipX, sPx(70), sPx(20), sPx(6));
    place(kIdCheckFlipY, sPx(70), sPx(20), sPx(6));
    place(kIdStaticFps, sPx(120), sPx(20), sPx(6));
    place(kIdEditEnroll, sPx(140), rowH - sPx(4), sPx(3));
    place(kIdBtnEnroll, sPx(80), rowH, 0);
    place(kIdBtnClear, sPx(80), rowH, 0);
    place(kIdBtnPrivacy, sPx(120), rowH, 0);

    nextRow();
    place(kIdComboDisplay, sPx(220), rowH, 0);
    place(kIdComboResolution, sPx(120), rowH, 0);
    place(kIdComboRefresh, sPx(90), rowH, 0);
    place(kIdCheckVsync, sPx(70), sPx(20), sPx(6));
    place(kIdComboBuffers, sPx(52), rowH, 0);
    place(kIdCheckFullscreen, sPx(70), sPx(20), sPx(6));
    place(kIdCheckSystemMode, sPx(120), sPx(20), sPx(6));
    place(kIdCheckSrgb, sPx(60), sPx(20), sPx(6));
    place(kIdEditGamma, sPx(70), rowH - sPx(4), sPx(3));
    place(kIdEditTempK, sPx(70), rowH - sPx(4), sPx(3));
    place(kIdComboAa, sPx(55), rowH, 0);
    place(kIdComboAniso, sPx(55), rowH, 0);
    place(kIdBtnApplyDisplay, sPx(90), rowH, 0);
    place(kIdStaticFrameTime, sPx(260), sPx(20), sPx(6));

    const int topH = y + rowH + pad;
    if (g_app) g_app->topControlsHeight = topH;
    if (g_app && g_app->hwndPreview) {
        MoveWindow(g_app->hwndPreview, 0, topH, rc.right - rc.left, (rc.bottom - rc.top) - topH, TRUE);
    }
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void refreshDisplayCombos(HWND hwnd) {
    if (!g_app) return;
    if (!g_app->displaysReady) return;
    fillDisplayCombo(GetDlgItem(hwnd, kIdComboDisplay));
    const int outIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboDisplay), CB_GETCURSEL, 0, 0));
    fillResolutionCombo(GetDlgItem(hwnd, kIdComboResolution), outIndex);
    const int resIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboResolution), CB_GETCURSEL, 0, 0));
    const auto& outs = g_app->displays.outputs();
    if (outIndex >= 0 && outIndex < static_cast<int>(outs.size())) {
        const auto res = uniqueResolutions(outs[static_cast<size_t>(outIndex)].modes);
        if (resIndex >= 0 && resIndex < static_cast<int>(res.size())) {
            fillRefreshCombo(GetDlgItem(hwnd, kIdComboRefresh), outIndex, res[static_cast<size_t>(resIndex)].first, res[static_cast<size_t>(resIndex)].second);
        }
    }
    syncDisplayControlsFromConfig(hwnd);
}

RenderOptions readRenderOptionsFromUi(HWND hwnd) {
    RenderOptions opt;
    if (!g_app) return opt;
    opt.vsync = (SendMessageW(GetDlgItem(hwnd, kIdCheckVsync), BM_GETCHECK, 0, 0) == BST_CHECKED);
    opt.fullscreen = (SendMessageW(GetDlgItem(hwnd, kIdCheckFullscreen), BM_GETCHECK, 0, 0) == BST_CHECKED);
    opt.allowSystemModeSwitch = (SendMessageW(GetDlgItem(hwnd, kIdCheckSystemMode), BM_GETCHECK, 0, 0) == BST_CHECKED);
    opt.enableSRGB = (SendMessageW(GetDlgItem(hwnd, kIdCheckSrgb), BM_GETCHECK, 0, 0) == BST_CHECKED);

    const auto gamma = tryParseDouble(readTextW(GetDlgItem(hwnd, kIdEditGamma)));
    if (gamma) opt.gamma = static_cast<float>(*gamma);
    const auto tempK = tryParseInt(readTextW(GetDlgItem(hwnd, kIdEditTempK)));
    if (tempK) opt.colorTempK = *tempK;

    const int bufSel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboBuffers), CB_GETCURSEL, 0, 0));
    opt.bufferCount = (bufSel == 1) ? 3 : 2;

    const int aaSel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboAa), CB_GETCURSEL, 0, 0));
    const std::vector<int> aa = g_app->rendererReady ? g_app->renderer.supportedMsaaSampleCounts() : std::vector<int>{1};
    if (aaSel >= 0 && aaSel < static_cast<int>(aa.size())) opt.aaSamples = aa[static_cast<size_t>(aaSel)];

    const int anSel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboAniso), CB_GETCURSEL, 0, 0));
    const std::vector<int> levels = {1, 2, 4, 8, 16};
    if (anSel >= 0 && anSel < static_cast<int>(levels.size())) opt.anisoLevel = levels[static_cast<size_t>(anSel)];

    const int outIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboDisplay), CB_GETCURSEL, 0, 0));
    const int resIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboResolution), CB_GETCURSEL, 0, 0));
    const int hzIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboRefresh), CB_GETCURSEL, 0, 0));

    const auto& outs = g_app->displays.outputs();
    if (outIndex >= 0 && outIndex < static_cast<int>(outs.size())) {
        const auto res = uniqueResolutions(outs[static_cast<size_t>(outIndex)].modes);
        if (resIndex >= 0 && resIndex < static_cast<int>(res.size())) {
            const int w = res[static_cast<size_t>(resIndex)].first;
            const int h = res[static_cast<size_t>(resIndex)].second;
            const auto rms = refreshModesForRes(outs[static_cast<size_t>(outIndex)].modes, w, h);
            if (hzIndex >= 0 && hzIndex < static_cast<int>(rms.size())) {
                const auto& m = rms[static_cast<size_t>(hzIndex)];
                opt.modeWidth = m.width;
                opt.modeHeight = m.height;
                opt.refreshNumerator = m.refreshNumerator;
                opt.refreshDenominator = m.refreshDenominator;
            }
        }
    }

    return opt;
}

bool applyDisplaySettings(HWND hwnd) {
    if (!g_app) return false;
    if (!g_app->rendererReady || !g_app->displaysReady) return false;

    const RenderOptions prevOpt = g_app->renderer.options();
    const RenderOptions nextOpt = readRenderOptionsFromUi(hwnd);

    const int outIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboDisplay), CB_GETCURSEL, 0, 0));
    DisplayMode selectedMode;
    selectedMode.width = nextOpt.modeWidth;
    selectedMode.height = nextOpt.modeHeight;
    selectedMode.refreshNumerator = nextOpt.refreshNumerator;
    selectedMode.refreshDenominator = nextOpt.refreshDenominator;
    const auto vr = g_app->displays.validateMode(outIndex, selectedMode);
    if (!vr.ok) {
        MessageBoxW(hwnd, wFromUtf8(vr.reason).c_str(), L"显示模式不可用", MB_OK | MB_ICONWARNING);
        return false;
    }

    if (outIndex >= 0 && outIndex < static_cast<int>(g_app->displays.outputs().size())) {
        const auto& o = g_app->displays.outputs()[static_cast<size_t>(outIndex)];
        const int w = o.desktopRect.right - o.desktopRect.left;
        const int h = o.desktopRect.bottom - o.desktopRect.top;
        SetWindowPos(hwnd, nullptr, o.desktopRect.left + w / 8, o.desktopRect.top + h / 8, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const auto t0 = std::chrono::steady_clock::now();
    bool ok = g_app->renderer.reconfigure(nextOpt);
    g_app->renderer.renderFrame(nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (!ok) {
        g_app->renderer.reconfigure(prevOpt);
        MessageBoxW(hwnd, wFromUtf8(g_app->renderer.lastError()).c_str(), L"显示切换失败（已回滚）", MB_OK | MB_ICONERROR);
        return false;
    }

    g_app->cfg.display.outputIndex = outIndex;
    g_app->cfg.display.width = nextOpt.modeWidth;
    g_app->cfg.display.height = nextOpt.modeHeight;
    g_app->cfg.display.refreshNumerator = nextOpt.refreshNumerator;
    g_app->cfg.display.refreshDenominator = nextOpt.refreshDenominator;
    g_app->cfg.display.vsync = nextOpt.vsync;
    g_app->cfg.display.swapchainBuffers = nextOpt.bufferCount;
    g_app->cfg.display.fullscreen = nextOpt.fullscreen;
    g_app->cfg.display.allowSystemModeSwitch = nextOpt.allowSystemModeSwitch;
    g_app->cfg.display.enableSRGB = nextOpt.enableSRGB;
    g_app->cfg.display.gamma = nextOpt.gamma;
    g_app->cfg.display.colorTempK = nextOpt.colorTempK;
    g_app->cfg.display.aaSamples = nextOpt.aaSamples;
    g_app->cfg.display.anisoLevel = nextOpt.anisoLevel;

    // 以 patch 的形式更新（局部更新）
    {
        std::ostringstream os;
        os << "{"
           << "\"display\":{"
           << "\"outputIndex\":" << outIndex << ","
           << "\"width\":" << nextOpt.modeWidth << ","
           << "\"height\":" << nextOpt.modeHeight << ","
           << "\"refreshNumerator\":" << nextOpt.refreshNumerator << ","
           << "\"refreshDenominator\":" << nextOpt.refreshDenominator << ","
           << "\"vsync\":" << (nextOpt.vsync ? "true" : "false") << ","
           << "\"swapchainBuffers\":" << nextOpt.bufferCount << ","
           << "\"fullscreen\":" << (nextOpt.fullscreen ? "true" : "false") << ","
           << "\"allowSystemModeSwitch\":" << (nextOpt.allowSystemModeSwitch ? "true" : "false") << ","
           << "\"enableSRGB\":" << (nextOpt.enableSRGB ? "true" : "false") << ","
           << "\"gamma\":" << nextOpt.gamma << ","
           << "\"colorTempK\":" << nextOpt.colorTempK << ","
           << "\"aaSamples\":" << nextOpt.aaSamples << ","
           << "\"anisoLevel\":" << nextOpt.anisoLevel
           << "}}";
        (void)persistSettingsPatch(hwnd, os.str(), L"显示切换成功但配置写入失败");
    }
    if (ms > 500) {
        MessageBoxW(hwnd, (L"切换已完成，但耗时=" + std::to_wstring(ms) + L"ms（超过 500ms 目标）").c_str(), L"提示", MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

int topControlsHeightPx() {
    if (g_app && g_app->topControlsHeight > 0) return g_app->topControlsHeight;
    const int pad = sPx(8);
    const int rowH = sPx(32);
    return pad + (rowH + pad) * 2;
}

std::optional<DisplayMode> pickModeClosestHz(const std::vector<DisplayMode>& modes, int w, int h, int preferHz) {
    std::optional<DisplayMode> best;
    double bestDist = 1e9;
    for (const auto& m : modes) {
        if (m.width != w || m.height != h) continue;
        const double hz = m.refreshHz();
        const double d = std::abs(hz - static_cast<double>(preferHz));
        if (!best || d < bestDist) {
            best = m;
            bestDist = d;
        }
    }
    return best;
}

std::vector<std::pair<int, DisplayMode>> buildStabilityPlan() {
    std::vector<std::pair<int, DisplayMode>> plan;
    if (!g_app) return plan;
    const auto& outs = g_app->displays.outputs();
    if (outs.empty()) return plan;

    const std::vector<std::pair<int, int>> targets = {
        {640, 480},
        {1280, 720},
        {1920, 1080},
        {2560, 1440},
        {3840, 2160},
    };
    const std::vector<int> preferHz = {60, 120, 144, 240};

    const int outIndex = std::clamp(g_app->cfg.display.outputIndex, 0, static_cast<int>(outs.size() - 1));
    const auto& modes = outs[static_cast<size_t>(outIndex)].modes;

    for (const auto& wh : targets) {
        std::optional<DisplayMode> best;
        for (int hz : preferHz) {
            best = pickModeClosestHz(modes, wh.first, wh.second, hz);
            if (best) break;
        }
        if (best) plan.emplace_back(outIndex, *best);
    }

    if (plan.size() < 5) {
        for (const auto& m : modes) {
            if (plan.size() >= 5) break;
            bool exists = false;
            for (const auto& p : plan) {
                const auto& e = p.second;
                if (e.width == m.width && e.height == m.height && e.refreshNumerator == m.refreshNumerator && e.refreshDenominator == m.refreshDenominator) {
                    exists = true;
                    break;
                }
            }
            if (!exists) plan.emplace_back(outIndex, m);
        }
    }
    return plan;
}

bool applyModeProgrammatic(HWND hwnd, int outIndex, const DisplayMode& mode) {
    if (!g_app) return false;
    if (!g_app->rendererReady || !g_app->displaysReady) return false;

    const auto vr = g_app->displays.validateMode(outIndex, mode);
    if (!vr.ok) {
        g_app->events.append("mode_validate_fail", vr.reason);
        return false;
    }

    RenderOptions nextOpt = g_app->renderer.options();
    nextOpt.fullscreen = true;
    nextOpt.allowSystemModeSwitch = true;
    nextOpt.modeWidth = mode.width;
    nextOpt.modeHeight = mode.height;
    nextOpt.refreshNumerator = mode.refreshNumerator;
    nextOpt.refreshDenominator = mode.refreshDenominator;

    const auto prevOpt = g_app->renderer.options();
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = g_app->renderer.reconfigure(nextOpt);
    g_app->renderer.renderFrame(nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (!ok) {
        g_app->renderer.reconfigure(prevOpt);
        g_app->events.append("mode_apply_fail", g_app->renderer.lastError());
        return false;
    }

    g_app->events.append("mode_apply_ok", utf8FromW(modeToText(mode)) + " apply_ms=" + std::to_string(ms));

    if (ms > 500) {
        g_app->events.append("mode_apply_slow", "apply_ms=" + std::to_string(ms));
    }

    if (hwnd) {
        const int topH = topControlsHeightPx();
        SetWindowPos(hwnd, nullptr, 0, 0, mode.width, mode.height + topH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    return true;
}

void runAutomatedTestsIfNeeded(HWND hwnd) {
    if (!g_app) return;
    const bool enabled = g_app->stabilityTest || g_app->soakTest || g_app->perfReport;
    if (!enabled) return;

    const auto now = std::chrono::steady_clock::now();
    if (g_app->testStart.time_since_epoch().count() == 0) {
        g_app->testStart = now;
        g_app->nextSwitch = now;
        if (g_app->soakTest) g_app->testEnd = now + std::chrono::hours(g_app->soakHours);
        else if (g_app->perfReport) g_app->testEnd = now + std::chrono::seconds(g_app->perfSeconds);
        else g_app->testEnd = now + std::chrono::seconds(static_cast<int>(g_app->stabilityPlan.size() * static_cast<size_t>(g_app->stabilityPerModeSec)));

        g_app->events.append("test_start", "stability=" + std::to_string(g_app->stabilityTest ? 1 : 0) + " soak=" + std::to_string(g_app->soakTest ? 1 : 0) +
                                              " perf=" + std::to_string(g_app->perfReport ? 1 : 0));
    }

    if (g_app->stabilityTest && now >= g_app->nextSwitch && !g_app->stabilityPlan.empty()) {
        const auto item = g_app->stabilityPlan[g_app->stabilityIndex % g_app->stabilityPlan.size()];
        applyModeProgrammatic(hwnd, item.first, item.second);
        g_app->stabilityIndex++;
        g_app->nextSwitch = now + std::chrono::seconds(g_app->stabilityPerModeSec);
    }

    if (now >= g_app->testEnd) {
        const auto st = g_app->renderer.stats();
        const double flickerRate = (st.presentCount > 0)
                                       ? (100.0 * static_cast<double>(st.presentFailCount + st.deviceRemovedCount + st.swapchainRecreateCount) /
                                          static_cast<double>(st.presentCount))
                                       : 0.0;
        std::ostringstream oss;
        oss << "present=" << st.presentCount << " fail=" << st.presentFailCount << " removed=" << st.deviceRemovedCount << " recreate=" << st.swapchainRecreateCount
            << " p50=" << st.frameTimes.p50Ms << " p95=" << st.frameTimes.p95Ms << " p99=" << st.frameTimes.p99Ms << " flicker%=" << flickerRate;
        g_app->events.append("test_end", oss.str());
        PostQuitMessage(0);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            InitCommonControls();
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboCamera, nullptr, nullptr);
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboFormat, nullptr, nullptr);
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboCamFps, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"应用相机", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdBtnApplyCamera, nullptr, nullptr);
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboPreviewScale, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"翻转X", BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdCheckFlipX, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"翻转Y", BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdCheckFlipY, nullptr, nullptr);
            CreateWindowExW(0, WC_STATICW, L"FPS: 0", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdStaticFps, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"person_001", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdEditEnroll, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"注册", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdBtnEnroll, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"清空库", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdBtnClear, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"隐私设置", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdBtnPrivacy, nullptr, nullptr);

            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboDisplay, nullptr, nullptr);
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboResolution, nullptr, nullptr);
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboRefresh, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"VSync", BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdCheckVsync, nullptr, nullptr);
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboBuffers, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"全屏", BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdCheckFullscreen, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"系统模式", BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdCheckSystemMode, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"sRGB", BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdCheckSrgb, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"2.2", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdEditGamma, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"6500", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdEditTempK, nullptr, nullptr);
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboAa, nullptr, nullptr);
            CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdComboAniso, nullptr, nullptr);
            CreateWindowExW(0, WC_BUTTONW, L"应用显示", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdBtnApplyDisplay, nullptr, nullptr);
            CreateWindowExW(0, WC_STATICW, L"帧时间: -", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)kIdStaticFrameTime, nullptr, nullptr);

            const wchar_t* previewClass = L"rk_wcfr_preview";
            WNDCLASSW wcPrev{};
            wcPrev.lpfnWndProc = PreviewProc;
            wcPrev.hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            wcPrev.lpszClassName = previewClass;
            wcPrev.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wcPrev);

            HWND preview = CreateWindowExW(0, previewClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, wcPrev.hInstance, nullptr);
            if (g_app) {
                g_app->hwndMain = hwnd;
                g_app->hwndPreview = preview;
            }

            layoutControls(hwnd);
            SetTimer(hwnd, kTimerRender, 16, nullptr);
            SetTimer(hwnd, kTimerMetrics, 1000, nullptr);

            if (g_app) {
                g_app->dpi = GetDpiForWindow(hwnd);
                g_app->dpiScale = static_cast<double>(g_app->dpi) / 96.0;
            }

            if (g_app) {
                g_app->rendererReady = g_app->renderer.initialize(preview);
                if (g_app->rendererReady) {
                    RenderOptions ro;
                    ro.vsync = g_app->cfg.display.vsync;
                    ro.bufferCount = g_app->cfg.display.swapchainBuffers;
                    ro.fullscreen = g_app->cfg.display.fullscreen;
                    ro.allowSystemModeSwitch = g_app->cfg.display.allowSystemModeSwitch;
                    ro.modeWidth = g_app->cfg.display.width;
                    ro.modeHeight = g_app->cfg.display.height;
                    ro.refreshNumerator = g_app->cfg.display.refreshNumerator;
                    ro.refreshDenominator = g_app->cfg.display.refreshDenominator;
                    ro.enableSRGB = g_app->cfg.display.enableSRGB;
                    ro.gamma = static_cast<float>(g_app->cfg.display.gamma);
                    ro.colorTempK = g_app->cfg.display.colorTempK;
                    ro.aaSamples = g_app->cfg.display.aaSamples;
                    ro.anisoLevel = g_app->cfg.display.anisoLevel;
                    g_app->renderer.reconfigure(ro);
                    const PreviewScaleMode psm = (g_app->cfg.ui.previewScaleMode == 1) ? PreviewScaleMode::Letterbox : PreviewScaleMode::CropFill;
                    g_app->renderer.setPreviewScaleMode(psm);
                    RECT prc{};
                    GetClientRect(preview, &prc);
                    const int pw = std::max(1, static_cast<int>(prc.right - prc.left));
                    const int ph = std::max(1, static_cast<int>(prc.bottom - prc.top));
                    g_app->pipe.setPreviewLayout(pw, ph, g_app->cfg.ui.previewScaleMode);
                }

                g_app->displaysReady = g_app->displays.refresh();
                if (g_app->displaysReady) refreshDisplayCombos(hwnd);

                g_app->metrics.open(g_app->cfg.log.logDir);
                g_app->events.open(g_app->cfg.log.logDir);
                g_app->pipe.setEventLogger(&g_app->events);
                if (g_app->abcCfg.testC) g_app->cfg.http.enable = true;
                if (g_app->cfg.http.enable) g_app->http.start(&g_app->pipe, &g_app->events, g_app->cfg.http.port, &g_app->settings);
                if (g_app->cfg.poster.enable) {
                    g_app->poster.start(&g_app->pipe, &g_app->events, g_app->cfg.poster.postUrl, g_app->cfg.poster.throttleMs, g_app->cfg.poster.backoffMinMs,
                                        g_app->cfg.poster.backoffMaxMs);
                }
                g_app->lastMetricsT = std::chrono::steady_clock::now();
                g_app->lastTitleT = g_app->lastMetricsT;
            }

            if (g_app) {
                const auto devs = g_app->pipe.devices();
                fillCameraCombo(GetDlgItem(hwnd, kIdComboCamera), devs);
                int sel = 0;
                if (!g_app->cfg.camera.preferredDeviceId.empty()) {
                    for (size_t i = 0; i < devs.size(); i++) {
                        if (devs[i].deviceId == g_app->cfg.camera.preferredDeviceId) {
                            sel = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (!devs.empty()) SendMessageW(GetDlgItem(hwnd, kIdComboCamera), CB_SETCURSEL, sel, 0);
                fillCamResPresetCombo(GetDlgItem(hwnd, kIdComboFormat), g_app->cfg.camera.width, g_app->cfg.camera.height);
                fillCamFpsPresetCombo(GetDlgItem(hwnd, kIdComboCamFps), g_app->cfg.camera.fps);

                if (!devs.empty()) {
                    const auto res = g_app->pipe.applyCameraSettings(sel, g_app->cfg.camera.width, g_app->cfg.camera.height, g_app->cfg.camera.fps, 300);
                    if (!res.ok) {
                        const auto res2 = g_app->pipe.applyCameraSettings(sel, 640, 480, 30, 300);
                        if (res2.ok) {
                            g_app->cfg.camera.width = 640;
                            g_app->cfg.camera.height = 480;
                            g_app->cfg.camera.fps = 30;
                            (void)persistSettingsPatch(hwnd, "{\"camera\":{\"width\":640,\"height\":480,\"fps\":30}}", L"相机回退成功但配置写入失败");
                            fillCamResPresetCombo(GetDlgItem(hwnd, kIdComboFormat), 640, 480);
                            fillCamFpsPresetCombo(GetDlgItem(hwnd, kIdComboCamFps), 30);
                        }
                    }
                }
            }

            SendMessageW(GetDlgItem(hwnd, kIdComboPreviewScale), CB_RESETCONTENT, 0, 0);
            SendMessageW(GetDlgItem(hwnd, kIdComboPreviewScale), CB_ADDSTRING, 0, (LPARAM)L"裁剪填充");
            SendMessageW(GetDlgItem(hwnd, kIdComboPreviewScale), CB_ADDSTRING, 0, (LPARAM)L"信箱");
            if (g_app) {
                int sel = (g_app->cfg.ui.previewScaleMode == 1) ? 1 : 0;
                SendMessageW(GetDlgItem(hwnd, kIdComboPreviewScale), CB_SETCURSEL, sel, 0);
            } else {
                SendMessageW(GetDlgItem(hwnd, kIdComboPreviewScale), CB_SETCURSEL, 0, 0);
            }

            if (g_app) {
                if (g_app->stabilityTest) g_app->stabilityPlan = buildStabilityPlan();
            }

            if (g_app) {
                const bool abc = g_app->abcCfg.testA || g_app->abcCfg.testB || g_app->abcCfg.testC;
                if (abc) {
                    g_app->abcCfg.httpPort = g_app->cfg.http.port;
                    g_app->abc.start(hwnd, &g_app->pipe, &g_app->events, g_app->abcCfg);
                }
            }

            if (g_app) {
                const bool abc = g_app->abcCfg.testA || g_app->abcCfg.testB || g_app->abcCfg.testC;
                if (!abc && !(g_app->stabilityTest || g_app->soakTest || g_app->perfReport)) {
                MessageBoxW(hwnd,
                            L"提示：本程序会调用摄像头进行实时预览与人脸识别。\n"
                            L"如无法打开摄像头，请检查 Windows 摄像头隐私权限与是否被其他程序占用。\n"
                            L"本程序默认将识别日志写入 storage/win_logs。",
                            L"Windows 摄像头人脸识别测试系统",
                            MB_OK | MB_ICONINFORMATION);
                }
            }

            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE: {
            layoutControls(hwnd);
            if (g_app && g_app->rendererReady && g_app->hwndPreview) {
                RECT rc{};
                GetClientRect(g_app->hwndPreview, &rc);
                const int pw = std::max(1, static_cast<int>(rc.right - rc.left));
                const int ph = std::max(1, static_cast<int>(rc.bottom - rc.top));
                g_app->renderer.onResize(pw, ph);
                g_app->pipe.setPreviewLayout(pw, ph, g_app->cfg.ui.previewScaleMode);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            if (g_app) {
                g_app->dpi = HIWORD(wParam);
                g_app->dpiScale = static_cast<double>(g_app->dpi) / 96.0;
            }
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layoutControls(hwnd);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (!g_app) return 0;

            if (id == kIdBtnApplyCamera && code == BN_CLICKED) {
                const int camIndex = (int)SendMessageW(GetDlgItem(hwnd, kIdComboCamera), CB_GETCURSEL, 0, 0);
                const auto res = readCamResPreset(GetDlgItem(hwnd, kIdComboFormat));
                const int fps = readCamFpsPreset(GetDlgItem(hwnd, kIdComboCamFps));
                const auto r = g_app->pipe.applyCameraSettings(camIndex, res.w, res.h, fps, 300);
                if (!r.ok) {
                    fillCamResPresetCombo(GetDlgItem(hwnd, kIdComboFormat), g_app->cfg.camera.width, g_app->cfg.camera.height);
                    fillCamFpsPresetCombo(GetDlgItem(hwnd, kIdComboCamFps), g_app->cfg.camera.fps);
                    MessageBoxW(hwnd, wFromUtf8(r.reason).c_str(), L"相机切换失败（已回滚）", MB_OK | MB_ICONERROR);
                    return 0;
                }

                const auto devs = g_app->pipe.devices();
                if (camIndex >= 0 && camIndex < static_cast<int>(devs.size())) {
                    g_app->cfg.camera.preferredDeviceId = devs[static_cast<size_t>(camIndex)].deviceId;
                }
                g_app->cfg.camera.width = res.w;
                g_app->cfg.camera.height = res.h;
                g_app->cfg.camera.fps = fps;
                {
                    std::ostringstream os;
                    os << "{\"camera\":{"
                       << "\"preferredDeviceId\":\"" << jsonEscape(utf8FromW(g_app->cfg.camera.preferredDeviceId)) << "\","
                       << "\"width\":" << res.w << ","
                       << "\"height\":" << res.h << ","
                       << "\"fps\":" << fps
                       << "}}";
                    (void)persistSettingsPatch(hwnd, os.str(), L"相机切换成功但配置写入失败");
                }
                if (r.totalMs > 250) {
                    MessageBoxW(hwnd, (L"切换成功，但耗时=" + std::to_wstring(r.totalMs) + L"ms").c_str(), L"提示", MB_OK | MB_ICONINFORMATION);
                }
                return 0;
            }
            if (id == kIdCheckFlipX || id == kIdCheckFlipY) {
                const bool fx = (SendMessageW(GetDlgItem(hwnd, kIdCheckFlipX), BM_GETCHECK, 0, 0) == BST_CHECKED);
                const bool fy = (SendMessageW(GetDlgItem(hwnd, kIdCheckFlipY), BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_app->pipe.setFlip(fx, fy);
                return 0;
            }
            if (id == kIdBtnEnroll && code == BN_CLICKED) {
                wchar_t buf[256];
                GetWindowTextW(GetDlgItem(hwnd, kIdEditEnroll), buf, 256);
                std::string pid = utf8FromW(buf);
                g_app->pipe.requestEnroll(pid);
                return 0;
            }
            if (id == kIdBtnClear && code == BN_CLICKED) {
                g_app->pipe.requestClearDb();
                return 0;
            }
            if (id == kIdBtnPrivacy && code == BN_CLICKED) {
                g_app->pipe.openPrivacySettings();
                return 0;
            }
            if (id == kIdComboPreviewScale && code == CBN_SELCHANGE) {
                const int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboPreviewScale), CB_GETCURSEL, 0, 0));
                const PreviewScaleMode psm = (sel == 1) ? PreviewScaleMode::Letterbox : PreviewScaleMode::CropFill;
                g_app->renderer.setPreviewScaleMode(psm);
                g_app->cfg.ui.previewScaleMode = (sel == 1) ? 1 : 0;
                if (g_app->hwndPreview) {
                    RECT prc{};
                    GetClientRect(g_app->hwndPreview, &prc);
                    const int pw = std::max(1, static_cast<int>(prc.right - prc.left));
                    const int ph = std::max(1, static_cast<int>(prc.bottom - prc.top));
                    g_app->pipe.setPreviewLayout(pw, ph, g_app->cfg.ui.previewScaleMode);
                }
                {
                    std::ostringstream os;
                    os << "{\"ui\":{\"previewScaleMode\":" << g_app->cfg.ui.previewScaleMode << "}}";
                    (void)persistSettingsPatch(hwnd, os.str(), L"预览缩放切换成功但配置写入失败");
                }
                return 0;
            }
            if (id == kIdComboDisplay && code == CBN_SELCHANGE) {
                const int outIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboDisplay), CB_GETCURSEL, 0, 0));
                fillResolutionCombo(GetDlgItem(hwnd, kIdComboResolution), outIndex);
                const int resIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboResolution), CB_GETCURSEL, 0, 0));
                const auto& outs = g_app->displays.outputs();
                if (outIndex >= 0 && outIndex < static_cast<int>(outs.size())) {
                    const auto res = uniqueResolutions(outs[static_cast<size_t>(outIndex)].modes);
                    if (resIndex >= 0 && resIndex < static_cast<int>(res.size())) {
                        fillRefreshCombo(GetDlgItem(hwnd, kIdComboRefresh), outIndex, res[static_cast<size_t>(resIndex)].first, res[static_cast<size_t>(resIndex)].second);
                    }
                }
                return 0;
            }
            if (id == kIdComboResolution && code == CBN_SELCHANGE) {
                const int outIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboDisplay), CB_GETCURSEL, 0, 0));
                const int resIndex = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kIdComboResolution), CB_GETCURSEL, 0, 0));
                const auto& outs = g_app->displays.outputs();
                if (outIndex >= 0 && outIndex < static_cast<int>(outs.size())) {
                    const auto res = uniqueResolutions(outs[static_cast<size_t>(outIndex)].modes);
                    if (resIndex >= 0 && resIndex < static_cast<int>(res.size())) {
                        fillRefreshCombo(GetDlgItem(hwnd, kIdComboRefresh), outIndex, res[static_cast<size_t>(resIndex)].first, res[static_cast<size_t>(resIndex)].second);
                    }
                }
                return 0;
            }
            if (id == kIdBtnApplyDisplay && code == BN_CLICKED) {
                applyDisplaySettings(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_TIMER: {
            if (!g_app) return 0;
            if (wParam == kTimerRender) {
                RenderState rs;
                const bool ok = g_app->pipe.tryGetRenderState(rs);
                if (ok && g_app->rendererReady) {
                    g_app->renderer.renderFrame(&rs.bgr);
                    std::wstring fpsText = L"FPS: " + std::to_wstring((int)(rs.fps + 0.5));
                    SetWindowTextW(GetDlgItem(hwnd, kIdStaticFps), fpsText.c_str());

                    const auto st = g_app->renderer.stats();
                    const double flickerRate = (st.presentCount > 0)
                                                   ? (100.0 * static_cast<double>(st.presentFailCount + st.deviceRemovedCount + st.swapchainRecreateCount) /
                                                      static_cast<double>(st.presentCount))
                                                   : 0.0;
                    const std::wstring ft =
                        L"帧时间ms: last=" + std::to_wstring((int)(st.frameTimes.lastMs + 0.5)) + L" p95=" +
                        std::to_wstring((int)(st.frameTimes.p95Ms + 0.5)) + L" p99=" + std::to_wstring((int)(st.frameTimes.p99Ms + 0.5)) +
                        L" 闪屏%=" + std::to_wstring(flickerRate);
                    SetWindowTextW(GetDlgItem(hwnd, kIdStaticFrameTime), ft.c_str());

                    if (!rs.status.empty()) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - g_app->lastTitleT > std::chrono::milliseconds(250)) {
                            std::wstring title = L"Windows 摄像头人脸识别测试系统 - " + wFromUtf8(rs.status);
                            SetWindowTextW(hwnd, title.c_str());
                            g_app->lastTitleT = now;
                        }
                    }
                } else {
                    if (g_app->rendererReady) g_app->renderer.renderFrame(nullptr);
                }

                if (g_app->pipe.lastErrorIsPrivacyDenied()) {
                    SetWindowTextW(GetDlgItem(hwnd, kIdBtnPrivacy), L"隐私设置（需授权）");
                } else {
                    SetWindowTextW(GetDlgItem(hwnd, kIdBtnPrivacy), L"隐私设置");
                }
            } else if (wParam == kTimerMetrics) {
                if (g_app->rendererReady) {
                    RenderMetricsSample s;
                    s.tsIso8601 = nowIso8601Local();
                    s.renderer = g_app->renderer.stats();
                    s.rssBytes = processRssBytes();
                    s.handleCount = processHandleCount();
                    const auto gm = gpuMemoryBytesBestEffort();
                    if (gm) {
                        s.gpuDedicatedBytes = gm->first;
                        s.gpuSharedBytes = gm->second;
                    }
                    g_app->metrics.append(s);
                }
                // 热重载（轮询）：当 %APPDATA%\rk_wcfr\config.json 被外部修改时，这里会 reload+校验+必要时回滚。
                // 为什么放在定时器：避免在后台线程直接操控 UI/renderer，减少并发死锁风险。
                {
                    AppConfig before = g_app->cfg;
                    bool applied = false;
                    std::string err;
                    if (g_app->settings.pollReloadOnce(applied, err) && applied) {
                        g_app->cfg = g_app->settings.current();
                        // 最小可安全热更新：HTTP server / poster（涉及线程与网络资源，可 stop/start）
                        if (before.http.enable != g_app->cfg.http.enable || before.http.port != g_app->cfg.http.port) {
                            g_app->http.stop();
                            if (g_app->cfg.http.enable) {
                                g_app->http.start(&g_app->pipe, &g_app->events, g_app->cfg.http.port, &g_app->settings);
                            }
                        }
                        const bool posterChanged =
                            (before.poster.enable != g_app->cfg.poster.enable) ||
                            (before.poster.postUrl != g_app->cfg.poster.postUrl) ||
                            (before.poster.throttleMs != g_app->cfg.poster.throttleMs) ||
                            (before.poster.backoffMinMs != g_app->cfg.poster.backoffMinMs) ||
                            (before.poster.backoffMaxMs != g_app->cfg.poster.backoffMaxMs);
                        if (posterChanged) {
                            g_app->poster.stop();
                            if (g_app->cfg.poster.enable) {
                                g_app->poster.start(&g_app->pipe, &g_app->events, g_app->cfg.poster.postUrl, g_app->cfg.poster.throttleMs,
                                                    g_app->cfg.poster.backoffMinMs, g_app->cfg.poster.backoffMaxMs);
                            }
                        }
                    }
                }
                runAutomatedTestsIfNeeded(hwnd);
            }
            return 0;
        }
        case WM_DESTROY: {
            if (g_app) {
                g_app->abc.stop();
                g_app->poster.stop();
                g_app->http.stop();
                g_app->metrics.close();
                g_app->events.close();
                g_app->renderer.shutdown();
                g_app->pipe.shutdown();
            }
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace
}  // namespace rk_win

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    rk_win::setPerMonitorDpiAwareV2();

    rk_win::AppState app;
    rk_win::g_app = &app;
    {
        std::string warn;
        app.settings.initialize(warn);
        app.cfg = app.settings.current();
        // 初始化告警不弹窗（避免启动即打断），但会在日志系统启动后记录。
        // 坑：如果配置文件损坏且 .bak 也无效，initialize 会退回默认/INI 迁移，warn 可用于定位原因。
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto parseNextInt = [&](int& v, int& i) {
        if (i + 1 >= argc) return;
        try {
            v = std::stoi(argv[i + 1]);
            i++;
        } catch (...) {
        }
    };
    auto parseNextPath = [&](std::filesystem::path& p, int& i) {
        if (i + 1 >= argc) return;
        if (argv[i + 1]) {
            p = argv[i + 1];
            i++;
        }
    };
    if (argv) {
        for (int i = 1; i < argc; i++) {
            const std::wstring a = argv[i] ? argv[i] : L"";
            if (a == L"--stability_test") app.stabilityTest = true;
            else if (a == L"--soak_test") app.soakTest = true;
            else if (a == L"--perf_report") app.perfReport = true;
            else if (a == L"--stability_per_mode_sec") parseNextInt(app.stabilityPerModeSec, i);
            else if (a == L"--soak_hours") parseNextInt(app.soakHours, i);
            else if (a == L"--perf_seconds") parseNextInt(app.perfSeconds, i);
            else if (a == L"--test_a") app.abcCfg.testA = true;
            else if (a == L"--test_b") app.abcCfg.testB = true;
            else if (a == L"--test_c") app.abcCfg.testC = true;
            else if (a == L"--test_b_repeats") parseNextInt(app.abcCfg.repeatsB, i);
            else if (a == L"--test_c_seconds") parseNextInt(app.abcCfg.secondsC, i);
            else if (a == L"--test_report_dir") parseNextPath(app.abcCfg.reportDir, i);
        }
        LocalFree(argv);
    }

    if (app.stabilityTest || app.soakTest || app.perfReport || app.abcCfg.testA || app.abcCfg.testB || app.abcCfg.testC) {
        nCmdShow = SW_SHOWMINIMIZED;
    }
    app.pipe.initialize(app.cfg);

    const wchar_t* className = L"rk_win_camera_face_recognition";
    WNDCLASSW wc{};
    wc.lpfnWndProc = rk_win::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0,
                                className,
                                L"Windows 摄像头人脸识别测试系统",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                app.cfg.ui.windowWidth,
                                app.cfg.ui.windowHeight,
                                nullptr,
                                nullptr,
                                hInstance,
                                nullptr);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

#endif

