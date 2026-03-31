#include "rk_win/MfCamera.h"

#ifdef _WIN32

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <sstream>

namespace rk_win {
namespace {

using Microsoft::WRL::ComPtr;

std::string utf8FromWide(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wideFromGuid(const GUID& g) {
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return std::wstring(buf);
}

ErrorCategory classifyOpenError(HRESULT hr) {
    if (hr == MF_E_NO_CAPTURE_DEVICES_AVAILABLE) return ErrorCategory::DeviceNotFound;
    if (hr == E_ACCESSDENIED) return ErrorCategory::PrivacyDenied;
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY)) return ErrorCategory::PrivacyDenied;
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) return ErrorCategory::PrivacyDenied;
    if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) return ErrorCategory::DeviceBusy;
    if (hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_INVALIDTYPE) return ErrorCategory::FormatNotSupported;
    if (FAILED(hr)) return ErrorCategory::BackendFailure;
    return ErrorCategory::Unknown;
}

std::string hrToCode(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

std::string hrToMessage(HRESULT hr) {
    LPWSTR buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buf, 0, nullptr);
    if (n == 0 || !buf) return "Media Foundation 错误";
    std::wstring ws(buf, buf + n);
    LocalFree(buf);
    while (!ws.empty() && (ws.back() == L'\r' || ws.back() == L'\n')) ws.pop_back();
    return utf8FromWide(ws);
}

bool getAttrSize(IMFMediaType* t, int& w, int& h) {
    UINT32 ww = 0, hh = 0;
    if (FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &ww, &hh))) return false;
    w = static_cast<int>(ww);
    h = static_cast<int>(hh);
    return true;
}

int getAttrFps(IMFMediaType* t) {
    UINT32 num = 0, den = 0;
    if (FAILED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den))) return 0;
    if (den == 0) return 0;
    return static_cast<int>(num / den);
}

std::wstring getStringAttr(IMFActivate* act, REFGUID key) {
    UINT32 cch = 0;
    if (FAILED(act->GetStringLength(key, &cch))) return L"";
    std::wstring s;
    s.resize(cch);
    if (FAILED(act->GetString(key, s.data(), cch + 1, &cch))) return L"";
    s.resize(cch);
    return s;
}

}  // namespace

struct MfCamera::Impl {
    bool started = false;
    bool open = false;
    CameraDevice device{};
    CameraFormat format{};

    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaSource> source;
    UINT32 stride = 0;
};

MfCamera::MfCamera() : impl_(new Impl()) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    impl_->started = SUCCEEDED(hr);
}

MfCamera::~MfCamera() {
    close();
    if (impl_->started) {
        MFShutdown();
    }
    delete impl_;
}

std::vector<CameraDevice> MfCamera::enumerateDevices() {
    std::vector<CameraDevice> out;

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return out;
    if (FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) return out;

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
    if (FAILED(hr) || count == 0) return out;

    out.reserve(count);
    for (UINT32 i = 0; i < count; i++) {
        CameraDevice d;
        d.name = getStringAttr(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        d.deviceId = getStringAttr(devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);

        ComPtr<IMFMediaSource> src;
        HRESULT ahr = devices[i]->ActivateObject(IID_PPV_ARGS(&src));
        if (SUCCEEDED(ahr) && src) {
            ComPtr<IMFAttributes> readerAttrs;
            if (SUCCEEDED(MFCreateAttributes(&readerAttrs, 1))) {
                readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            }
            ComPtr<IMFSourceReader> reader;
            if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(src.Get(), readerAttrs.Get(), &reader)) && reader) {
                std::vector<CameraFormat> formats;
                for (DWORD ti = 0; ti < 256; ti++) {
                    ComPtr<IMFMediaType> t;
                    HRESULT thr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, ti, &t);
                    if (thr == MF_E_NO_MORE_TYPES) break;
                    if (FAILED(thr) || !t) continue;

                    int w = 0, h = 0;
                    if (!getAttrSize(t.Get(), w, h)) continue;
                    const int fps = getAttrFps(t.Get());
                    GUID subtype{};
                    if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;

                    CameraFormat f;
                    f.width = w;
                    f.height = h;
                    f.fps = fps;
                    f.subtype = wideFromGuid(subtype);

                    const bool dup = std::any_of(formats.begin(), formats.end(), [&](const CameraFormat& x) {
                        return x.width == f.width && x.height == f.height && x.fps == f.fps;
                    });
                    if (!dup) formats.push_back(std::move(f));
                }
                std::sort(formats.begin(), formats.end(), [](const CameraFormat& a, const CameraFormat& b) {
                    if (a.width != b.width) return a.width < b.width;
                    if (a.height != b.height) return a.height < b.height;
                    return a.fps < b.fps;
                });
                d.formats = std::move(formats);
            }
            src->Shutdown();
        }

        out.push_back(std::move(d));
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    return out;
}

CameraOpenResult MfCamera::open(const std::wstring& deviceId, int width, int height, int fps) {
    close();

    CameraOpenResult r;
    if (!impl_->started) {
        r.ok = false;
        r.category = ErrorCategory::BackendFailure;
        r.code = "mf_startup_failed";
        r.message = "Media Foundation 初始化失败";
        return r;
    }

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }
    hr = attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, deviceId.c_str());
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    ComPtr<IMFMediaSource> source;
    hr = MFCreateDeviceSource(attrs.Get(), &source);
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    ComPtr<IMFAttributes> readerAttrs;
    hr = MFCreateAttributes(&readerAttrs, 2);
    if (SUCCEEDED(hr)) {
        readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader);
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    ComPtr<IMFMediaType> type;
    hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get());
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    ComPtr<IMFMediaType> actual;
    hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    int aw = 0, ah = 0;
    getAttrSize(actual.Get(), aw, ah);
    const int afps = getAttrFps(actual.Get());
    GUID subtype{};
    actual->GetGUID(MF_MT_SUBTYPE, &subtype);

    UINT32 stride = 0;
    if (FAILED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        stride = static_cast<UINT32>(aw * 4);
    }

    impl_->reader = reader;
    impl_->source = source;
    impl_->open = true;
    impl_->stride = stride;
    impl_->device.deviceId = deviceId;
    impl_->device.name = L"";
    impl_->format.width = aw;
    impl_->format.height = ah;
    impl_->format.fps = afps > 0 ? afps : fps;
    impl_->format.subtype = wideFromGuid(subtype);

    r.ok = true;
    r.category = ErrorCategory::None;
    r.code = "ok";
    r.message = "ok";
    return r;
}

void MfCamera::close() {
    if (!impl_->open) return;
    impl_->reader.Reset();
    if (impl_->source) {
        impl_->source->Shutdown();
    }
    impl_->source.Reset();
    impl_->open = false;
}

bool MfCamera::isOpen() const {
    return impl_->open;
}

CameraDevice MfCamera::currentDevice() const {
    return impl_->device;
}

CameraFormat MfCamera::currentFormat() const {
    return impl_->format;
}

CameraOpenResult MfCamera::readFrameBgr(cv::Mat& outBgr, std::uint64_t& outTimestamp100ns) {
    CameraOpenResult r;
    if (!impl_->open || !impl_->reader) {
        r.ok = false;
        r.category = ErrorCategory::DeviceNotFound;
        r.code = "not_open";
        r.message = "摄像头未打开";
        return r;
    }

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    HRESULT hr = impl_->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &ts, &sample);
    if (FAILED(hr)) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }
    if (flags & MF_SOURCE_READERF_STREAMTICK) {
        r.ok = false;
        r.category = ErrorCategory::BackendFailure;
        r.code = "stream_tick";
        r.message = "采集流出现 tick";
        return r;
    }
    if (!sample) {
        r.ok = false;
        r.category = ErrorCategory::BackendFailure;
        r.code = "null_sample";
        r.message = "采集到空帧";
        return r;
    }

    ComPtr<IMFMediaBuffer> buf;
    hr = sample->ConvertToContiguousBuffer(&buf);
    if (FAILED(hr) || !buf) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    hr = buf->Lock(&data, &maxLen, &curLen);
    if (FAILED(hr) || !data) {
        r.ok = false;
        r.category = classifyOpenError(hr);
        r.code = hrToCode(hr);
        r.message = hrToMessage(hr);
        return r;
    }

    const int w = impl_->format.width;
    const int h = impl_->format.height;
    const int stride = static_cast<int>(impl_->stride);
    cv::Mat bgra(h, w, CV_8UC4, data, stride);
    cv::cvtColor(bgra, outBgr, cv::COLOR_BGRA2BGR);

    buf->Unlock();
    outTimestamp100ns = static_cast<std::uint64_t>(ts);
    r.ok = true;
    r.category = ErrorCategory::None;
    r.code = "ok";
    r.message = "ok";
    return r;
}

}  // namespace rk_win

#endif

