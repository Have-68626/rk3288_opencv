#include "rk_win/D3D11Renderer.h"

#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <opencv2/imgproc.hpp>

namespace rk_win {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

namespace {

struct ShaderConstants {
    float gamma = 2.2f;
    float invGamma = 1.0f / 2.2f;
    float tempScaleR = 1.0f;
    float tempScaleG = 1.0f;
    float tempScaleB = 1.0f;
    float uvScaleX = 1.0f;
    float uvScaleY = 1.0f;
    float uvOffsetX = 0.0f;
    float uvOffsetY = 0.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
};

struct Vertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

static std::array<float, 3> temperatureScaleRgb(int kelvin) {
    int t = kelvin;
    if (t < 1000) t = 1000;
    if (t > 40000) t = 40000;

    double temp = static_cast<double>(t) / 100.0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    if (temp <= 66.0) {
        r = 255.0;
        g = 99.4708025861 * std::log(temp) - 161.1195681661;
        if (temp <= 19.0) b = 0.0;
        else b = 138.5177312231 * std::log(temp - 10.0) - 305.0447927307;
    } else {
        r = 329.698727446 * std::pow(temp - 60.0, -0.1332047592);
        g = 288.1221695283 * std::pow(temp - 60.0, -0.0755148492);
        b = 255.0;
    }

    auto clamp01 = [](double v) -> float {
        if (v < 0.0) v = 0.0;
        if (v > 255.0) v = 255.0;
        return static_cast<float>(v / 255.0);
    };

    return {clamp01(r), clamp01(g), clamp01(b)};
}

static std::vector<double> sortedCopy(const std::vector<double>& v) {
    std::vector<double> out = v;
    std::sort(out.begin(), out.end());
    return out;
}

static double percentileFromSorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    if (p <= 0.0) return sorted.front();
    if (p >= 1.0) return sorted.back();
    const double idx = (static_cast<double>(sorted.size() - 1)) * p;
    const size_t i0 = static_cast<size_t>(std::floor(idx));
    const size_t i1 = std::min(sorted.size() - 1, i0 + 1);
    const double frac = idx - static_cast<double>(i0);
    return sorted[i0] * (1.0 - frac) + sorted[i1] * frac;
}

}  // namespace

struct D3D11Renderer::Impl {
    HWND hwnd = nullptr;
    RenderOptions opt;
    RendererStats stats;

    std::string lastError;
    mutable std::mutex mu;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> swapchain;

    ComPtr<ID3D11Texture2D> backbuffer;
    ComPtr<ID3D11RenderTargetView> backbufferRtv;

    ComPtr<ID3D11Texture2D> msaaColor;
    ComPtr<ID3D11RenderTargetView> msaaRtv;
    int msaaSamples = 1;

    ComPtr<ID3D11Texture2D> frameTex;
    ComPtr<ID3D11ShaderResourceView> frameSrv;
    int frameW = 0;
    int frameH = 0;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> cb;

    PreviewScaleMode previewScaleMode = PreviewScaleMode::CropFill;

    bool allowTearing = false;

    bool windowedStyleSaved = false;
    RECT windowedRect{};
    LONG_PTR windowedStyle = 0;
    LONG_PTR windowedExStyle = 0;

    std::vector<double> frameMs;
    size_t frameMsMax = 4096;

    bool createDeviceAndFactory() {
        UINT flags = 0;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;
        const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, static_cast<UINT>(std::size(fls)), D3D11_SDK_VERSION,
                                       &device, &flOut, &context);
        if (FAILED(hr)) {
            lastError = "D3D11CreateDevice 失败";
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device.As(&dxgiDevice))) {
            lastError = "Query IDXGIDevice 失败";
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
            lastError = "GetAdapter 失败";
            return false;
        }
        ComPtr<IDXGIFactory2> fac;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&fac)))) {
            lastError = "GetParent(IDXGIFactory2) 失败";
            return false;
        }
        factory = fac;

        allowTearing = false;
        ComPtr<IDXGIFactory5> fac5;
        if (SUCCEEDED(factory.As(&fac5))) {
            BOOL tearing = FALSE;
            if (SUCCEEDED(fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
                allowTearing = (tearing == TRUE);
            }
        }

        return true;
    }

    bool createShadersAndPipeline() {
        static const char* kHlsl = R"(
cbuffer Cb : register(b0) {
    float gamma;
    float invGamma;
    float tempR;
    float tempG;
    float tempB;
    float uvScaleX;
    float uvScaleY;
    float uvOffsetX;
    float uvOffsetY;
    float pad0;
    float pad1;
    float pad2;
};

struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut vs_main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos.xy, 0.0, 1.0);
    o.uv = i.uv;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 ps_main(VSOut i) : SV_Target {
    float2 uv = i.uv * float2(uvScaleX, uvScaleY) + float2(uvOffsetX, uvOffsetY);
    float4 c = tex0.Sample(samp0, uv);
    float3 rgb = saturate(c.rgb);
    rgb *= float3(tempR, tempG, tempB);
    rgb = saturate(rgb);
    rgb = pow(rgb, invGamma);
    return float4(rgb, 1.0);
}
)";

        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        ComPtr<ID3DBlob> err;

        HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vsBlob, &err);
        if (FAILED(hr)) {
            lastError = "D3DCompile VS 失败";
            return false;
        }
        hr = D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &psBlob, &err);
        if (FAILED(hr)) {
            lastError = "D3DCompile PS 失败";
            return false;
        }

        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr)) {
            lastError = "CreateVertexShader 失败";
            return false;
        }
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
        if (FAILED(hr)) {
            lastError = "CreatePixelShader 失败";
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = device->CreateInputLayout(il, static_cast<UINT>(std::size(il)), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);
        if (FAILED(hr)) {
            lastError = "CreateInputLayout 失败";
            return false;
        }

        const Vertex tri[3] = {
            {-1.0f, -1.0f, 0.0f, 1.0f},
            {-1.0f, 3.0f, 0.0f, -1.0f},
            {3.0f, -1.0f, 2.0f, 1.0f},
        };
        D3D11_BUFFER_DESC vbDesc{};
        vbDesc.ByteWidth = static_cast<UINT>(sizeof(tri));
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vbInit{};
        vbInit.pSysMem = tri;
        hr = device->CreateBuffer(&vbDesc, &vbInit, &vb);
        if (FAILED(hr)) {
            lastError = "CreateBuffer(VB) 失败";
            return false;
        }

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = static_cast<UINT>((sizeof(ShaderConstants) + 15) / 16 * 16);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&cbDesc, nullptr, &cb);
        if (FAILED(hr)) {
            lastError = "CreateBuffer(CB) 失败";
            return false;
        }

        return updateSampler();
    }

    bool updateSampler() {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = (opt.anisoLevel > 1) ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxAnisotropy = static_cast<UINT>(std::clamp(opt.anisoLevel, 1, 16));
        sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        sampler.Reset();
        if (FAILED(device->CreateSamplerState(&sd, &sampler))) {
            lastError = "CreateSamplerState 失败";
            return false;
        }
        return true;
    }

    std::optional<RECT> clientRect() const {
        if (!hwnd) return std::nullopt;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return std::nullopt;
        return rc;
    }

    void releaseSwapchainResources() {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        msaaRtv.Reset();
        msaaColor.Reset();
        backbufferRtv.Reset();
        backbuffer.Reset();
        swapchain.Reset();
    }

    bool createSwapchainForClient() {
        auto rcOpt = clientRect();
        if (!rcOpt) return false;
        RECT rc = *rcOpt;
        int w = std::max(1, static_cast<int>(rc.right - rc.left));
        int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
        return createSwapchain(w, h);
    }

    bool createSwapchain(int w, int h) {
        releaseSwapchainResources();

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = static_cast<UINT>(w);
        sc.Height = static_cast<UINT>(h);
        DXGI_FORMAT backbufferFmt = opt.enableSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        sc.Format = backbufferFmt;
        sc.SampleDesc.Count = 1;
        sc.SampleDesc.Quality = 0;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = static_cast<UINT>(std::clamp(opt.bufferCount, 2, 3));
        sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sc.Scaling = DXGI_SCALING_STRETCH;
        sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sc.Flags = 0;
        if (!opt.vsync && allowTearing) sc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        ComPtr<IDXGISwapChain1> sc1;
        HRESULT hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sc, nullptr, nullptr, &sc1);
        if (FAILED(hr)) {
            lastError = "CreateSwapChainForHwnd 失败";
            return false;
        }
        swapchain = sc1;

        hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
        if (FAILED(hr)) {
            lastError = "GetBuffer(backbuffer) 失败";
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = backbufferFmt;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtv.Texture2D.MipSlice = 0;
        hr = device->CreateRenderTargetView(backbuffer.Get(), &rtv, &backbufferRtv);
        if (FAILED(hr)) {
            lastError = "CreateRenderTargetView(backbuffer) 失败";
            return false;
        }

        stats.swapchainRecreateCount++;
        return createOrUpdateMsaaTargets(w, h);
    }

    bool createOrUpdateMsaaTargets(int w, int h) {
        msaaRtv.Reset();
        msaaColor.Reset();
        msaaSamples = std::clamp(opt.aaSamples, 1, 8);
        if (msaaSamples <= 1) {
            msaaSamples = 1;
            return true;
        }

        UINT quality = 0;
        if (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(msaaSamples), &quality)) || quality == 0) {
            msaaSamples = 1;
            return true;
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        td.SampleDesc.Count = static_cast<UINT>(msaaSamples);
        td.SampleDesc.Quality = quality - 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &msaaColor))) {
            msaaSamples = 1;
            return true;
        }

        DXGI_FORMAT rtvFmt = opt.enableSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D11_RENDER_TARGET_VIEW_DESC msv{};
        msv.Format = rtvFmt;
        msv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        if (FAILED(device->CreateRenderTargetView(msaaColor.Get(), &msv, &msaaRtv))) {
            msaaSamples = 1;
            msaaColor.Reset();
            return true;
        }
        return true;
    }

    bool ensureFrameTexture(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (frameTex && frameSrv && frameW == w && frameH == h) return true;

        frameTex.Reset();
        frameSrv.Reset();
        frameW = w;
        frameH = h;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device->CreateTexture2D(&td, nullptr, &frameTex))) {
            lastError = "CreateTexture2D(frameTex) 失败";
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(frameTex.Get(), &sd, &frameSrv))) {
            lastError = "CreateShaderResourceView(frameSrv) 失败";
            return false;
        }
        return true;
    }

    bool updateConstants(float uvScaleX, float uvScaleY, float uvOffsetX, float uvOffsetY) {
        ShaderConstants c{};
        if (opt.enableSRGB) {
            c.gamma = 1.0f;
            c.invGamma = 1.0f;
        } else {
            c.gamma = (opt.gamma > 0.01f) ? opt.gamma : 2.2f;
            c.invGamma = 1.0f / c.gamma;
        }
        const auto scale = temperatureScaleRgb(opt.colorTempK);
        c.tempScaleR = scale[0];
        c.tempScaleG = scale[1];
        c.tempScaleB = scale[2];
        c.uvScaleX = uvScaleX;
        c.uvScaleY = uvScaleY;
        c.uvOffsetX = uvOffsetX;
        c.uvOffsetY = uvOffsetY;

        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
        std::memcpy(ms.pData, &c, sizeof(c));
        context->Unmap(cb.Get(), 0);
        return true;
    }

    void pushFrameTime(double ms) {
        stats.frameTimes.lastMs = ms;
        if (frameMs.size() < frameMsMax) frameMs.push_back(ms);
        else {
            frameMs.erase(frameMs.begin());
            frameMs.push_back(ms);
        }
        auto sorted = sortedCopy(frameMs);
        stats.frameTimes.p50Ms = percentileFromSorted(sorted, 0.50);
        stats.frameTimes.p95Ms = percentileFromSorted(sorted, 0.95);
        stats.frameTimes.p99Ms = percentileFromSorted(sorted, 0.99);
    }

    void saveWindowedStyle() {
        if (windowedStyleSaved || !hwnd) return;
        windowedStyleSaved = true;
        windowedStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        windowedExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        GetWindowRect(hwnd, &windowedRect);
    }

    void restoreWindowedStyle() {
        if (!windowedStyleSaved || !hwnd) return;
        SetWindowLongPtrW(hwnd, GWL_STYLE, windowedStyle);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, windowedExStyle);
        SetWindowPos(hwnd, nullptr, windowedRect.left, windowedRect.top, windowedRect.right - windowedRect.left, windowedRect.bottom - windowedRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    void setBorderlessFullscreenOnMonitor(HMONITOR mon) {
        if (!hwnd) return;
        saveWindowedStyle();
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return;
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, 0);
        const RECT r = mi.rcMonitor;
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
    }

    bool applyFullscreenState(bool wantFullscreen) {
        ComPtr<IDXGISwapChain> sc0;
        if (swapchain) swapchain.As(&sc0);
        if (!sc0) return true;

        if (!wantFullscreen) {
            sc0->SetFullscreenState(FALSE, nullptr);
            restoreWindowedStyle();
            return true;
        }

        if (!opt.allowSystemModeSwitch) {
            const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            setBorderlessFullscreenOnMonitor(mon);
            sc0->SetFullscreenState(FALSE, nullptr);
            return true;
        }

        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        setBorderlessFullscreenOnMonitor(mon);
        sc0->SetFullscreenState(TRUE, nullptr);
        if (opt.modeWidth > 0 && opt.modeHeight > 0 && opt.refreshNumerator > 0) {
            DXGI_MODE_DESC md{};
            md.Width = static_cast<UINT>(opt.modeWidth);
            md.Height = static_cast<UINT>(opt.modeHeight);
            md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            md.RefreshRate.Numerator = opt.refreshNumerator;
            md.RefreshRate.Denominator = opt.refreshDenominator ? opt.refreshDenominator : 1;
            md.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
            md.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
            sc0->ResizeTarget(&md);
        }
        return true;
    }

    bool recreateAll() {
        releaseSwapchainResources();
        frameTex.Reset();
        frameSrv.Reset();
        vb.Reset();
        sampler.Reset();
        cb.Reset();
        inputLayout.Reset();
        vs.Reset();
        ps.Reset();
        context.Reset();
        device.Reset();
        factory.Reset();

        if (!createDeviceAndFactory()) return false;
        if (!createShadersAndPipeline()) return false;
        return createSwapchainForClient();
    }
};

D3D11Renderer::D3D11Renderer() = default;
D3D11Renderer::~D3D11Renderer() { shutdown(); }

bool D3D11Renderer::initialize(HWND hwnd) {
    shutdown();
    impl_ = new Impl();
    impl_->hwnd = hwnd;
    impl_->opt = RenderOptions{};
    impl_->frameMs.clear();
    impl_->stats = RendererStats{};

    if (!impl_->createDeviceAndFactory()) return false;
    if (!impl_->createShadersAndPipeline()) return false;
    if (!impl_->createSwapchainForClient()) return false;
    return true;
}

void D3D11Renderer::shutdown() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (impl_->swapchain) {
            ComPtr<IDXGISwapChain> sc0;
            impl_->swapchain.As(&sc0);
            if (sc0) sc0->SetFullscreenState(FALSE, nullptr);
        }
        impl_->restoreWindowedStyle();
    }
    delete impl_;
    impl_ = nullptr;
}

bool D3D11Renderer::reconfigure(const RenderOptions& opt) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->opt = opt;
    if (!impl_->updateSampler()) return false;
    if (!impl_->createSwapchainForClient()) return false;
    if (!impl_->applyFullscreenState(opt.fullscreen)) return false;
    return true;
}

void D3D11Renderer::onResize(int clientW, int clientH) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->swapchain) return;
    if (clientW <= 0 || clientH <= 0) return;
    impl_->createSwapchain(clientW, clientH);
    impl_->applyFullscreenState(impl_->opt.fullscreen);
}

void D3D11Renderer::setPreviewScaleMode(PreviewScaleMode mode) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->previewScaleMode = mode;
}

PreviewScaleMode D3D11Renderer::previewScaleMode() const {
    if (!impl_) return PreviewScaleMode::CropFill;
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->previewScaleMode;
}

bool D3D11Renderer::renderFrame(const cv::Mat* bgr) {
    if (!impl_) return false;
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->swapchain || !impl_->backbufferRtv) return false;

    auto rcOpt = impl_->clientRect();
    if (!rcOpt) return false;
    RECT rc = *rcOpt;
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));

    ID3D11RenderTargetView* rt = impl_->msaaRtv ? impl_->msaaRtv.Get() : impl_->backbufferRtv.Get();
    impl_->context->OMSetRenderTargets(1, &rt, nullptr);

    const float clearColor[4] = {0, 0, 0, 1};
    impl_->context->ClearRenderTargetView(rt, clearColor);

    if (bgr && !bgr->empty()) {
        cv::Mat bgra;
        if (bgr->type() == CV_8UC3) {
            cv::cvtColor(*bgr, bgra, cv::COLOR_BGR2BGRA);
        } else if (bgr->type() == CV_8UC4) {
            // Performance optimization: Use shallow copy for CV_8UC4 since texture upload is read-only.
            // Why: Avoids an expensive deep copy (bgr->clone()) which allocates megabytes per frame.
            // Rollback: Revert to `bgra = bgr->clone();` if non-continuous data causes texture upload issues.
            bgra = *bgr;
        } else {
            cv::Mat tmp;
            bgr->convertTo(tmp, CV_8UC3);
            cv::cvtColor(tmp, bgra, cv::COLOR_BGR2BGRA);
        }

        float uvScaleX = 1.0f;
        float uvScaleY = 1.0f;
        float uvOffsetX = 0.0f;
        float uvOffsetY = 0.0f;
        D3D11_VIEWPORT vp{};
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        const float dstAspect = static_cast<float>(w) / static_cast<float>(h);
        const float srcAspect = static_cast<float>(bgra.cols) / static_cast<float>(bgra.rows);

        if (impl_->previewScaleMode == PreviewScaleMode::Letterbox) {
            if (dstAspect > srcAspect) {
                const float vpW = static_cast<float>(h) * srcAspect;
                vp.TopLeftX = (static_cast<float>(w) - vpW) * 0.5f;
                vp.TopLeftY = 0.0f;
                vp.Width = vpW;
                vp.Height = static_cast<float>(h);
            } else {
                const float vpH = static_cast<float>(w) / srcAspect;
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = (static_cast<float>(h) - vpH) * 0.5f;
                vp.Width = static_cast<float>(w);
                vp.Height = vpH;
            }
        } else {
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.Width = static_cast<float>(w);
            vp.Height = static_cast<float>(h);
            if (dstAspect > srcAspect) {
                uvScaleY = srcAspect / dstAspect;
                uvOffsetY = (1.0f - uvScaleY) * 0.5f;
            } else if (dstAspect < srcAspect) {
                uvScaleX = dstAspect / srcAspect;
                uvOffsetX = (1.0f - uvScaleX) * 0.5f;
            }
        }

        impl_->context->RSSetViewports(1, &vp);
        if (!impl_->updateConstants(uvScaleX, uvScaleY, uvOffsetX, uvOffsetY)) return false;

        if (!impl_->ensureFrameTexture(bgra.cols, bgra.rows)) return false;

        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(impl_->context->Map(impl_->frameTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
        const std::uint8_t* src = bgra.data;
        std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(ms.pData);
        const size_t srcRow = static_cast<size_t>(bgra.cols) * 4;
        for (int y = 0; y < bgra.rows; y++) {
            std::memcpy(dst + static_cast<size_t>(y) * ms.RowPitch, src + static_cast<size_t>(y) * srcRow, srcRow);
        }
        impl_->context->Unmap(impl_->frameTex.Get(), 0);

        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        impl_->context->IASetInputLayout(impl_->inputLayout.Get());
        impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* vb = impl_->vb.Get();
        impl_->context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

        impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
        impl_->context->PSSetShader(impl_->ps.Get(), nullptr, 0);
        ID3D11SamplerState* samp = impl_->sampler.Get();
        impl_->context->PSSetSamplers(0, 1, &samp);
        ID3D11ShaderResourceView* srv = impl_->frameSrv.Get();
        impl_->context->PSSetShaderResources(0, 1, &srv);
        ID3D11Buffer* cb = impl_->cb.Get();
        impl_->context->VSSetConstantBuffers(0, 1, &cb);
        impl_->context->PSSetConstantBuffers(0, 1, &cb);

        impl_->context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        impl_->context->PSSetShaderResources(0, 1, &nullSrv);
    } else {
        D3D11_VIEWPORT vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        impl_->context->RSSetViewports(1, &vp);
        impl_->updateConstants(1.0f, 1.0f, 0.0f, 0.0f);
        impl_->stats.blackFrameCount++;
    }

    if (impl_->msaaColor) {
        auto fmt = impl_->opt.enableSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        impl_->context->ResolveSubresource(impl_->backbuffer.Get(), 0, impl_->msaaColor.Get(), 0, fmt);
    }

    UINT presentFlags = 0;
    UINT syncInterval = impl_->opt.vsync ? 1 : 0;
    if (!impl_->opt.vsync && impl_->allowTearing) presentFlags |= DXGI_PRESENT_ALLOW_TEARING;

    HRESULT pr = impl_->swapchain->Present(syncInterval, presentFlags);
    impl_->stats.presentCount++;
    if (FAILED(pr)) {
        impl_->stats.presentFailCount++;
        if (pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) {
            impl_->stats.deviceRemovedCount++;
            impl_->recreateAll();
        }
        impl_->lastError = "Present 失败";
        return false;
    }

    const auto t1 = clock::now();
    const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
    impl_->pushFrameTime(ms);
    return true;
}

RendererStats D3D11Renderer::stats() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->stats;
}

RenderOptions D3D11Renderer::options() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->opt;
}

std::vector<int> D3D11Renderer::supportedMsaaSampleCounts() const {
    if (!impl_) return {1};
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<int> out = {1};
    if (!impl_->device) return out;
    for (int s : {2, 4, 8}) {
        UINT q = 0;
        if (SUCCEEDED(impl_->device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(s), &q)) && q > 0) out.push_back(s);
    }
    return out;
}

int D3D11Renderer::maxAnisotropy() const {
    return 16;
}

std::string D3D11Renderer::lastError() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->lastError;
}

#else

struct D3D11Renderer::Impl {};

D3D11Renderer::D3D11Renderer() = default;
D3D11Renderer::~D3D11Renderer() = default;
bool D3D11Renderer::initialize(HWND) { return false; }
void D3D11Renderer::shutdown() {}
bool D3D11Renderer::reconfigure(const RenderOptions&) { return false; }
bool D3D11Renderer::renderFrame(const cv::Mat*) { return false; }
void D3D11Renderer::onResize(int, int) {}
void D3D11Renderer::setPreviewScaleMode(PreviewScaleMode) {}
PreviewScaleMode D3D11Renderer::previewScaleMode() const { return PreviewScaleMode::CropFill; }
RendererStats D3D11Renderer::stats() const { return {}; }
RenderOptions D3D11Renderer::options() const { return {}; }
std::vector<int> D3D11Renderer::supportedMsaaSampleCounts() const { return {1}; }
int D3D11Renderer::maxAnisotropy() const { return 1; }
std::string D3D11Renderer::lastError() const { return {}; }

#endif

}  // namespace rk_win

