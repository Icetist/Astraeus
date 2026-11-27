// The clip recorder. Windows' capture of one window (never the screen: only the game ends up in a clip) hands frames to the gpu,
// where they're cropped to the game's own area, scaled and turned into nv12, then the hardware h.264 encoder packs them. What
// comes out is kept in memory, the last stretch only, whole groups of pictures at a time. The sound is the game's own (or all
// but astraeus', so its own cues never land in a clip), caught by windows' per-process loopback and kept as plain samples.
// Saving copies the kept video into an mp4 as it is, no second encode, and encodes the sound to aac on the way.
#include "sys_clip.h"

#include <audioclient.h>
#include <avrt.h>
#include <audioclientactivationparams.h>
#include <codecapi.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <strmif.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGX = winrt::Windows::Graphics::DirectX;

namespace sys::clip {
namespace {

constexpr LONGLONG kSec = 10000000; // media foundation's clock: 100 ns
constexpr int kSurfaces = 8; // nv12 pictures handed round to the encoder: it reads one while the next is drawn
constexpr int kFramesKept = 2; // the capture's own buffers
constexpr LONGLONG kKeepExtra = 2 * kSec; // kept beyond the longest clip, so a whole group of pictures always covers it
constexpr LONGLONG kKeyEvery = kSec; // a clip starts at most this much before where it was asked to
constexpr LONGLONG kRefreshEvery = kSec; // a still game still gets a picture this often: keyframes keep coming
constexpr UINT kRate = 48000, kChannels = 2, kBits = 16; // the sound kept: what aac takes
constexpr UINT kAacBytes = 24000; // 192 kbit/s
constexpr REFERENCE_TIME kAudioBuffer = 2000000; // 200 ms the loopback holds for us
constexpr DWORD kAudioWaitMs = 200;
constexpr UINT kAudioChunk = 4800; // frames per sample written: 100 ms
constexpr LONGLONG kSnapAudio = 500000; // a sound chunk this close to where the last one ended follows it straight on: arrival jitter isn't a gap
constexpr int kSoftFps = 30, kSoftHeight = 720; // the processor encoding: kept light so the game isn't starved
constexpr UINT kVendors[] = {0x10DE, 0x1002, 0x8086, 0x5143}; // nvidia, amd, intel, qualcomm: real graphics, not a virtual display's
constexpr int kInViews = 4; // the capture's own buffers, each read through a view made once
constexpr int kDrainMax = 16; // outputs taken after one cpu-encoded picture, at most
constexpr int kCropEveryMs = 500; // the game's area inside its window, looked up again this often
constexpr UINT kMaxSide = 4096; // the encoders' limit
constexpr double kBitsPerPixel[] = {0.048, 0.08, 0.13}; // light, balanced, high: about 6, 10 and 16 mbit/s at 1080p60
constexpr UINT kMinBitrate = 2000000;

LONGLONG Now() { // the same clock windows stamps the sound with
    static const LONGLONG freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (LONGLONG)((double)c.QuadPart * kSec / freq);
}

struct Packet {
    std::shared_ptr<const std::vector<BYTE>> bytes; // shared with a save, never copied
    LONGLONG at = 0, dur = 0; // now() clock
    bool key = false;
};

struct Chunk { // sound as it came
    LONGLONG at = 0;
    std::vector<int16_t> pcm; // interleaved
};

std::vector<BYTE> SequenceHeader(const std::vector<BYTE>& frame) { // h.264's sps and pps out of a keyframe, start codes and all, when the encoder doesn't say
    std::vector<BYTE> out;
    size_t i = 0, n = frame.size();
    auto start = [&](size_t at) { return at + 3 <= n && frame[at] == 0 && frame[at + 1] == 0 && (frame[at + 2] == 1 || (at + 4 <= n && frame[at + 2] == 0 && frame[at + 3] == 1)); };
    while (i < n && !start(i)) ++i;
    while (i < n) {
        size_t head = frame[i + 2] == 1 ? 3 : 4, j = i + head;
        while (j < n && !start(j)) ++j;
        if (i + head < n) {
            int type = frame[i + head] & 0x1F;
            if (type == 7 || type == 8) out.insert(out.end(), frame.begin() + (ptrdiff_t)i, frame.begin() + (ptrdiff_t)j);
        }
        i = j;
    }
    return out;
}

class Activation : public winrt::implements<Activation, IActivateAudioInterfaceCompletionHandler> { // windows hands the loopback over on another thread
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Activation() { CloseHandle(done); }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done);
        return S_OK;
    }
};

class Impl final : public Recorder {
public:
    Impl(const Options& o) : opt(o) {}
    ~Impl() override {
        Stop();
        CloseHandle(frameReady);
    }

    bool Begin(HWND game, DWORD pid, std::wstring& why);
    void Pause(bool on) override;
    bool Paused() const override { return paused; }
    bool Follow(HWND w) override;
    bool Alive() const override { return alive; }
    size_t Bytes() const override;
    double Seconds() const override;
    bool HardwareEncoder() const override { return hardware; }
    void Save(int secs, int after, const std::wstring& path, HWND notify, UINT msg) override;

private:
    bool MakeDevice(HWND game);
    void Size(int maxHeight);
    bool Capture(HWND w);
    void EndCapture();
    void OnFrame(const WGC::Direct3D11CaptureFramePool& pool);
    bool MakeEncoder(bool hw);
    bool SetTypes();
    void EncodeLoop();
    bool Convert(ID3D11Texture2D* src, UINT cw, UINT ch, int slot);
    void Feed(IMFSample* s);
    void Pump();
    HRESULT TakeOutput();
    void Keep(Packet&& p);
    void AudioLoop(DWORD pid);
    void MicLoop();
    void KeepSound(std::deque<Chunk>& into, Chunk&& c);
    void Stop();
    std::shared_ptr<struct Snapshot> TakeSnap(int secs); // what a save writes, copied out under the lock
    std::mutex saveMu; // saves still waiting out their seconds after the press
    std::condition_variable saveCv;
    int waiting2 = 0;
    bool stopping = false;

    Options opt;
    UINT width = 0, height = 0, bitrate = 0;
    std::atomic<bool> stop{false}, paused{false}, alive{true}, forceKey{false};
    bool hardware = false;

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    UINT vpW = 0, vpH = 0; // the size the processor was made for
    ComPtr<ID3D11VideoProcessorOutputView> outViews[kSurfaces]; // made once per picture, not per frame
    struct InView {
        ID3D11Texture2D* tex = nullptr; // the capture's buffer it reads
        ComPtr<ID3D11VideoProcessorInputView> view;
    };
    InView inViews[kInViews];
    int inNext = 0;
    HANDLE frameReady = CreateEventW(nullptr, FALSE, FALSE, nullptr); // the game drew: the encoder wakes only then
    ComPtr<ID3D11Texture2D> surfaces[kSurfaces];
    ComPtr<ID3D11Texture2D> staging; // the cpu encoder reads from here
    WGX::Direct3D11::IDirect3DDevice rtDevice{nullptr};

    std::mutex frameMu; // the capture, and the picture it handed over
    int readySlot = -1, nextSlot = 0, lastSlot = -1; // the surface waiting for the encoder; where the next goes; the last one encoded
    LONGLONG readyAt = 0, lastTaken = 0; // when windows caught it; when the last one was let through
    bool closing = false;
    HWND window = nullptr;
    RECT crop{}; // the game's area within the captured frame
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession session{nullptr};
    winrt::event_token arrivedToken{}, closedToken{};
    winrt::Windows::Graphics::SizeInt32 poolSize{};

    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<ICodecAPI> codec;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT managerToken = 0;
    bool async = false, providesSamples = false;
    DWORD outSize = 0;
    int needInput = 0;
    std::deque<ComPtr<IMFSample>> waiting;

    mutable std::mutex keepMu; // what's kept, shared with saves
    std::deque<Packet> video;
    std::deque<Chunk> audio, mic; // the game's sound, and the microphone's
    std::vector<BYTE> header;
    size_t videoBytes = 0, audioBytes = 0;

    std::thread encoder, listener, micListener;
};

bool Impl::MakeDevice(HWND game) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> pick, first;
    HMONITOR mon = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; !pick && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || std::none_of(std::begin(kVendors), std::end(kVendors), [&](UINT v) { return v == d.VendorId; })) continue; // a virtual display or the basic driver: no encoder, slow copies
            if (!first) first = a;
            ComPtr<IDXGIOutput> o;
            for (UINT j = 0; !pick && a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j, o.Reset()) {
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == mon) pick = a; // the card showing the game: its frames are already there
            }
        }
    }
    if (!pick) pick = first;
    HRESULT made = pick ? D3D11CreateDevice(pick.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx)
                        : D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(made)) return false;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE); // the encoder works on it from its own threads
    if (FAILED(dev.As(&vdev)) || FAILED(ctx.As(&vctx))) return false;
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(dev.As(&dxgi))) return false;
    winrt::com_ptr<::IInspectable> insp;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()))) return false;
    rtDevice = insp.as<WGX::Direct3D11::IDirect3DDevice>();
    return true;
}

bool Impl::Capture(HWND w) {
    try {
        auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        WGC::GraphicsCaptureItem it{nullptr};
        if (FAILED(interop->CreateForWindow(w, winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(it))) || !it) return false;
        poolSize = it.Size();
        auto p = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(rtDevice, WGX::DirectXPixelFormat::B8G8R8A8UIntNormalized, kFramesKept, poolSize);
        auto s = p.CreateCaptureSession(it);
        s.IsCursorCaptureEnabled(opt.cursor);
        try {
            s.IsBorderRequired(false); // windows 11: no yellow frame round the game
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(frameMu);
            item = it, pool = p, session = s, window = w, closing = false;
        }
        arrivedToken = p.FrameArrived([this](const WGC::Direct3D11CaptureFramePool& sender, const winrt::Windows::Foundation::IInspectable&) { OnFrame(sender); });
        closedToken = it.Closed([this](const WGC::GraphicsCaptureItem&, const winrt::Windows::Foundation::IInspectable&) { alive = false; });
        s.StartCapture();
        return true;
    } catch (...) {
        return false;
    }
}

void Impl::EndCapture() {
    WGC::Direct3D11CaptureFramePool p{nullptr};
    WGC::GraphicsCaptureSession s{nullptr};
    WGC::GraphicsCaptureItem it{nullptr};
    {
        std::lock_guard<std::mutex> lock(frameMu); // waits out a frame being taken
        closing = true;
        p = pool, s = session, it = item;
        pool = nullptr, session = nullptr, item = nullptr;
    }
    try {
        if (p) p.FrameArrived(arrivedToken), p.Close();
        if (it) it.Closed(closedToken);
        if (s) s.Close();
    } catch (...) {
    }
}

void Impl::OnFrame(const WGC::Direct3D11CaptureFramePool& sender) {
    std::lock_guard<std::mutex> lock(frameMu);
    if (closing) return;
    try {
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;
        auto size = frame.ContentSize();
        if (size.Width != poolSize.Width || size.Height != poolSize.Height) { // the window changed size: the capture follows
            poolSize = size;
            for (auto& v : inViews) v = InView{}; // the old buffers go with the old pool
            sender.Recreate(rtDevice, WGX::DirectXPixelFormat::B8G8R8A8UIntNormalized, kFramesKept, size);
        }
        LONGLONG at = frame.SystemRelativeTime().count(); // when windows caught it, on the clock the sound is stamped with
        const LONGLONG period = kSec / (std::max)(opt.fps, 1);
        if (paused || at - lastTaken < period - period / 8) return; // faster than the clip's rate: let go here, nothing copied or converted
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex)))) return;
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        UINT w = (std::min)((UINT)(std::max)(size.Width, 1), d.Width), h = (std::min)((UINT)(std::max)(size.Height, 1), d.Height);
        if (!Convert(tex.Get(), w, h, nextSlot)) return; // straight from windows' buffer into the encoder's picture: one pass on the gpu
        lastTaken = at;
        readySlot = nextSlot, readyAt = at;
        nextSlot = (nextSlot + 1) % kSurfaces;
        SetEvent(frameReady);
    } catch (...) {
    }
}

bool Impl::MakeEncoder(bool hw) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    UINT32 flags = (hw ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &found, &count)) || !count) {
        CoTaskMemFree(found);
        return false;
    }
    bool ok = false;
    for (UINT32 i = 0; i < count && !ok; ++i) {
        mft.Reset(), events.Reset(), codec.Reset();
        if (FAILED(found[i]->ActivateObject(IID_PPV_ARGS(&mft)))) continue;
        ComPtr<IMFAttributes> attr;
        async = false;
        if (SUCCEEDED(mft->GetAttributes(&attr))) {
            UINT32 isAsync = 0;
            if (SUCCEEDED(attr->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
                attr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                async = true;
            }
            UINT32 aware = 0;
            if (hw && SUCCEEDED(attr->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware && manager)
                if (FAILED(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())))) continue;
            if (hw && !aware) continue; // a hardware encoder that can't read gpu pictures isn't worth the copy
        }
        if (async && FAILED(mft.As(&events))) continue;
        mft.As(&codec);
        ok = SetTypes();
    }
    for (UINT32 i = 0; i < count; ++i) found[i]->Release();
    CoTaskMemFree(found);
    if (!ok) mft.Reset();
    hardware = ok && hw;
    return ok;
}

bool Impl::SetTypes() {
    auto set = [&](const GUID& key, UINT32 v, VARTYPE vt) {
        if (!codec) return;
        VARIANT var;
        VariantInit(&var);
        var.vt = vt;
        if (vt == VT_BOOL) var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
        else var.ulVal = v;
        codec->SetValue(&key, &var);
    };
    set(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, VT_UI4);
    set(CODECAPI_AVEncCommonMeanBitRate, bitrate, VT_UI4);
    set(CODECAPI_AVEncMPVGOPSize, (UINT32)opt.fps, VT_UI4); // a keyframe a second: a clip starts within a second of where it was asked to
    set(CODECAPI_AVEncMPVDefaultBPictureCount, 0, VT_UI4);
    set(CODECAPI_AVLowLatencyMode, 1, VT_BOOL);
    ComPtr<IMFMediaType> outType, inType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, (UINT32)opt.fps, 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(mft->SetOutputType(0, outType.Get(), 0))) {
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main); // an older encoder: main, then whatever it likes
        if (FAILED(mft->SetOutputType(0, outType.Get(), 0))) {
            outType->DeleteItem(MF_MT_MPEG2_PROFILE);
            if (FAILED(mft->SetOutputType(0, outType.Get(), 0))) return false;
        }
    }
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, (UINT32)opt.fps, 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(mft->SetInputType(0, inType.Get(), 0))) return false;
    MFT_OUTPUT_STREAM_INFO info{};
    if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;
    providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outSize = (std::max)((DWORD)info.cbSize, (DWORD)(width * height * 3 / 2));
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void Impl::Size(int maxHeight) {
    RECT rc{};
    GetClientRect(window ? window : GetDesktopWindow(), &rc);
    UINT w = (UINT)(std::max)(rc.right - rc.left, 2L), h = (UINT)(std::max)(rc.bottom - rc.top, 2L);
    if (maxHeight > 0 && h > (UINT)maxHeight) w = (UINT)((double)w * maxHeight / h), h = (UINT)maxHeight; // scaled down, never up
    double fit = (std::min)(1.0, (double)kMaxSide / (std::max)(w, h));
    width = (std::max)((UINT)(w * fit) & ~1u, 2u), height = (std::max)((UINT)(h * fit) & ~1u, 2u); // nv12 halves both sides
    bitrate = (std::max)(kMinBitrate, (UINT)(kBitsPerPixel[(std::clamp)(opt.quality, 0, 2)] * width * height * opt.fps));
}

bool Impl::Begin(HWND game, DWORD pid, std::wstring& why) {
    if (!Supported()) return why = L"This version of Windows can't record a single window", false;
    if (!MakeDevice(game)) return why = L"The graphics card couldn't be used for recording", false;
    window = game;
    Size(opt.height);
    if (SUCCEEDED(MFCreateDXGIDeviceManager(&managerToken, &manager))) manager->ResetDevice(dev.Get(), managerToken);
    if (!MakeEncoder(true)) { // no hardware encoder: the processor does it, kept to 30 fps and 720p so the game keeps its share
        opt.fps = (std::min)(opt.fps, kSoftFps);
        Size(opt.height > 0 ? (std::min)(opt.height, kSoftHeight) : kSoftHeight);
        if (!MakeEncoder(false)) return why = L"No video encoder could be started", false;
    }
    for (auto& s : surfaces) {
        D3D11_TEXTURE2D_DESC d{width, height, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | (hardware ? D3D11_BIND_VIDEO_ENCODER : 0u), 0, 0};
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &s))) {
            d.BindFlags = D3D11_BIND_RENDER_TARGET; // not every driver lets the encoder flag through
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &s))) return why = L"The graphics card couldn't make room for recording", false;
        }
    }
    if (!hardware) {
        D3D11_TEXTURE2D_DESC d{width, height, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return why = L"The graphics card couldn't make room for recording", false;
    }
    if (!Capture(game)) return why = L"Windows won't let this game's window be recorded", false;
    encoder = std::thread([this] { EncodeLoop(); });
    if (opt.audio > 0) listener = std::thread([this, pid] { AudioLoop(opt.audio == 1 ? pid : GetCurrentProcessId()); });
    if (opt.mic) micListener = std::thread([this] { MicLoop(); });
    return true;
}

bool Impl::Convert(ID3D11Texture2D* src, UINT cw, UINT ch, int slot) { // windows' captured picture into this nv12 surface, cropped and scaled; frameMu held
    D3D11_TEXTURE2D_DESC sd;
    src->GetDesc(&sd);
    UINT sw = sd.Width, sh = sd.Height;
    RECT area = crop;
    area.right = (std::min)(area.right, (LONG)cw), area.bottom = (std::min)(area.bottom, (LONG)ch); // never past what the window filled
    if (area.right - area.left < 2 || area.bottom - area.top < 2) area = {0, 0, (LONG)cw, (LONG)ch};
    if (!vp || vpW != sw || vpH != sh) { // made again when the captured size changes
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {(UINT)opt.fps, 1}, sw, sh, {(UINT)opt.fps, 1}, width, height, D3D11_VIDEO_USAGE_OPTIMAL_SPEED};
        vpEnum.Reset(), vp.Reset();
        for (auto& v : inViews) v = InView{};
        for (auto& v : outViews) v.Reset(); // they belong to the old processor
        if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &vpEnum)) || FAILED(vdev->CreateVideoProcessor(vpEnum.Get(), 0, &vp))) return false;
        vpW = sw, vpH = sh;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{}, outCs{};
        inCs.RGB_Range = 0; // full range rgb from the game
        outCs.YCbCr_Matrix = 1, outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235; // bt.709, as players expect
        vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inCs);
        vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &outCs);
        D3D11_VIDEO_COLOR black{};
        black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
        vctx->VideoProcessorSetOutputBackgroundColor(vp.Get(), TRUE, &black);
        vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
    }
    double fit = (std::min)((double)width / (std::max)(area.right - area.left, 1L), (double)height / (std::max)(area.bottom - area.top, 1L)); // the game whole, centred: a resized window is never stretched
    LONG dw = (LONG)((area.right - area.left) * fit), dh = (LONG)((area.bottom - area.top) * fit);
    RECT dst{((LONG)width - dw) / 2, ((LONG)height - dh) / 2, 0, 0};
    dst.right = dst.left + dw, dst.bottom = dst.top + dh;
    vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &area);
    vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{0, D3D11_VPIV_DIMENSION_TEXTURE2D, {0, 0}};
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{D3D11_VPOV_DIMENSION_TEXTURE2D, {0}};
    ID3D11VideoProcessorInputView* in = nullptr;
    for (auto& v : inViews)
        if (v.tex == src) in = v.view.Get();
    if (!in) { // one of the capture's few buffers, seen for the first time: its view made once
        InView& v = inViews[inNext];
        inNext = (inNext + 1) % kInViews;
        v.view.Reset(), v.tex = nullptr;
        if (FAILED(vdev->CreateVideoProcessorInputView(src, vpEnum.Get(), &ivd, &v.view))) return false;
        v.tex = src, in = v.view.Get();
    }
    if (!outViews[slot] && FAILED(vdev->CreateVideoProcessorOutputView(surfaces[slot].Get(), vpEnum.Get(), &ovd, &outViews[slot]))) return false;
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in;
    return SUCCEEDED(vctx->VideoProcessorBlt(vp.Get(), outViews[slot].Get(), 0, 1, &stream));
}

void Impl::Feed(IMFSample* s) {
    if (!async) { // the cpu encoder: a picture in, then whatever it has ready out, until it wants more
        if (SUCCEEDED(mft->ProcessInput(0, s, 0)))
            for (int guard = 0; guard < kDrainMax && TakeOutput() == S_OK; ++guard) {
            }
        return;
    }
    if (needInput > 0) {
        if (SUCCEEDED(mft->ProcessInput(0, s, 0))) --needInput;
    } else {
        waiting.emplace_back(s);
        while (waiting.size() > 2) waiting.pop_front(); // the encoder fell behind: the oldest picture goes, never a pile-up
    }
    Pump();
}

void Impl::Pump() {
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        if (!events || FAILED(events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev))) return;
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            if (!waiting.empty()) {
                mft->ProcessInput(0, waiting.front().Get(), 0);
                waiting.pop_front();
            } else {
                ++needInput;
            }
        } else if (type == METransformHaveOutput) {
            TakeOutput();
        }
    }
}

HRESULT Impl::TakeOutput() {
    MFT_OUTPUT_DATA_BUFFER out{};
    ComPtr<IMFSample> own;
    if (!providesSamples) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(outSize, &buf))) return E_OUTOFMEMORY;
        own->AddBuffer(buf.Get());
        out.pSample = own.Get();
    }
    DWORD status = 0;
    HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
    if (out.pEvents) out.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { // the encoder settled its output: take what it offers
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &t))) mft->SetOutputType(0, t.Get(), 0);
        return S_OK; // asked again: the picture is still in there
    }
    ComPtr<IMFSample> sample;
    if (providesSamples) sample.Attach(out.pSample); // ours to release
    else sample = own;
    if (FAILED(hr)) return hr; // MF_E_TRANSFORM_NEED_MORE_INPUT: drained
    if (!sample) return E_FAIL;
    ComPtr<IMFMediaBuffer> buf;
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || FAILED(buf->Lock(&data, nullptr, &len))) return E_FAIL;
    Packet p;
    p.bytes = std::make_shared<const std::vector<BYTE>>(data, data + len);
    buf->Unlock();
    sample->GetSampleTime(&p.at);
    sample->GetSampleDuration(&p.dur);
    p.key = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != 0;
    if (p.key && header.empty()) {
        ComPtr<IMFMediaType> t;
        UINT32 size = 0;
        std::vector<BYTE> h;
        if (SUCCEEDED(mft->GetOutputCurrentType(0, &t)) && SUCCEEDED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
            h.resize(size);
            t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, h.data(), size, nullptr);
        }
        if (h.empty()) h = SequenceHeader(*p.bytes);
        std::lock_guard<std::mutex> lock(keepMu);
        header = std::move(h);
    }
    Keep(std::move(p));
    return S_OK;
}

void Impl::Keep(Packet&& p) {
    std::lock_guard<std::mutex> lock(keepMu);
    if (video.empty() && !p.key) return; // a kept stretch always opens on a keyframe
    videoBytes += p.bytes->size();
    video.push_back(std::move(p));
    LONGLONG keep = (LONGLONG)opt.keepSecs * kSec + kKeepExtra, newest = video.back().at;
    for (;;) { // whole groups of pictures leave the front, once the next group alone still covers the longest clip
        size_t next = 1;
        while (next < video.size() && !video[next].key) ++next;
        if (next >= video.size() || newest - video[next].at < keep) break;
        for (size_t i = 0; i < next; ++i) videoBytes -= video[i].bytes->size();
        video.erase(video.begin(), video.begin() + (ptrdiff_t)next);
    }
}

void Impl::EncodeLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex); // windows' media scheduling: a busy game can't starve it into stutter
    const LONGLONG period = kSec / (std::max)(opt.fps, 1);
    LONGLONG lastFed = 0, cropAt = 0, lastKey = 0, lastAt = 0;
    while (!stop) {
        WaitForSingleObject(frameReady, (DWORD)(kRefreshEvery / 10000)); // asleep until a picture is ready; a still game wakes it once a second, for its keyframe
        if (stop) break;
        if (paused) continue;
        LONGLONG now = Now();
        if (now - cropAt > (LONGLONG)kCropEveryMs * 10000) { // the game's own area: the window less its frame and title bar
            cropAt = now;
            HWND w;
            {
                std::lock_guard<std::mutex> lock(frameMu);
                w = window;
            }
            RECT frame{}, client{};
            POINT origin{0, 0};
            if (w && SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) && GetClientRect(w, &client) && ClientToScreen(w, &origin)) {
                RECT c{origin.x - frame.left, origin.y - frame.top, origin.x - frame.left + client.right, origin.y - frame.top + client.bottom};
                std::lock_guard<std::mutex> lock(frameMu);
                if (c.right > c.left && c.bottom > c.top) crop = c;
            }
        }
        int slot;
        LONGLONG at;
        {
            std::lock_guard<std::mutex> lock(frameMu);
            slot = readySlot, at = readyAt, readySlot = -1;
            if (slot < 0 && lastSlot >= 0 && now - lastFed >= kRefreshEvery) slot = lastSlot, at = now; // nothing new for a second: the last picture again, so keyframes keep coming
            if (slot >= 0) lastSlot = slot;
        }
        if (slot < 0) continue;
        if (at <= lastAt) at = lastAt + 1; // never backwards
        lastAt = at, lastFed = now;
        ID3D11Texture2D* surface = surfaces[slot].Get();
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buf;
        if (hardware) {
            if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), surface, 0, FALSE, &buf))) continue;
            ComPtr<IMF2DBuffer> b2;
            DWORD len = 0;
            if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
        } else { // the cpu encoder: the picture comes back to memory
            ctx->CopyResource(staging.Get(), surface);
            D3D11_MAPPED_SUBRESOURCE map;
            DWORD size = width * height * 3 / 2;
            BYTE* dst = nullptr;
            if (FAILED(MFCreateMemoryBuffer(size, &buf)) || FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) continue;
            if (SUCCEEDED(buf->Lock(&dst, nullptr, nullptr))) {
                const BYTE* src = static_cast<const BYTE*>(map.pData);
                for (UINT y = 0; y < height * 3 / 2; ++y) std::memcpy(dst + (size_t)y * width, src + (size_t)y * map.RowPitch, width); // the luma rows, then the chroma rows under them
                buf->Unlock();
                buf->SetCurrentLength(size);
            }
            ctx->Unmap(staging.Get(), 0);
        }
        if (FAILED(MFCreateSample(&sample))) continue;
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(at);
        sample->SetSampleDuration(period);
        bool due = now - lastKey >= kKeyEvery; // by the clock, not by count: only changed pictures are encoded, so a count drifts
        if ((forceKey.exchange(false) || due) && codec) { // a keyframe each second, and after a pause, where the kept stretch starts over
            lastKey = now;
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4, v.ulVal = 1;
            codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        }
        Feed(sample.Get());
    }
    if (task) AvRevertMmThreadCharacteristics(task);
    if (mft) {
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    waiting.clear();
    mft.Reset(), events.Reset(), codec.Reset(); // let go on the thread that drove it
    CoUninitialize();
}

void Impl::AudioLoop(DWORD pid) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex); // sound taken on time even with the processor busy
    AUDIOCLIENT_ACTIVATION_PARAMS ap{};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode = opt.audio == 1 ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE; // everything but astraeus: its cues stay out
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(ap);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);
    auto handler = winrt::make_self<Activation>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = SUCCEEDED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.get(), &op)) && WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0;
    if (ok) {
        HRESULT hr = E_FAIL;
        ComPtr<IUnknown> unk;
        ok = SUCCEEDED(op->GetActivateResult(&hr, &unk)) && SUCCEEDED(hr) && SUCCEEDED(unk.As(&client));
    }
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, (WORD)kChannels, kRate, kRate * kChannels * kBits / 8, (WORD)(kChannels * kBits / 8), (WORD)kBits, 0};
    ok = ok && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kAudioBuffer, 0, &fmt, nullptr)) &&
         SUCCEEDED(client->SetEventHandle(ready)) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
    while (ok && !stop) {
        WaitForSingleObject(ready, kAudioWaitMs);
        UINT32 frames = 0;
        while (!stop && SUCCEEDED(capture->GetNextPacketSize(&frames)) && frames) {
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 qpc = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) break;
            if (!paused) {
                Chunk c;
                c.at = qpc ? (LONGLONG)qpc : Now() - (LONGLONG)frames * kSec / kRate; // stamped by windows on the clock the pictures use
                c.pcm.assign((size_t)frames * kChannels, 0);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) std::memcpy(c.pcm.data(), data, c.pcm.size() * sizeof(int16_t));
                KeepSound(audio, std::move(c));
            }
            capture->ReleaseBuffer(frames);
        }
    }
    if (client) client->Stop();
    capture.Reset(), client.Reset(), op.Reset();
    CloseHandle(ready);
    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
}

void Impl::KeepSound(std::deque<Chunk>& into, Chunk&& c) {
    std::lock_guard<std::mutex> lock(keepMu);
    if (!into.empty()) { // windows' timestamps wobble: a chunk that nearly follows on is laid right after the last, so nothing overlaps or clicks
        const Chunk& b = into.back();
        LONGLONG end = b.at + (LONGLONG)(b.pcm.size() / kChannels) * kSec / kRate;
        if (c.at > end - kSnapAudio && c.at < end + kSnapAudio) c.at = end;
    }
    audioBytes += c.pcm.size() * sizeof(int16_t);
    into.push_back(std::move(c));
    LONGLONG keep = (LONGLONG)opt.keepSecs * kSec + 2 * kKeepExtra;
    while (!into.empty() && into.back().at - into.front().at > keep) audioBytes -= into.front().pcm.size() * sizeof(int16_t), into.pop_front();
}

void Impl::MicLoop() { // the default microphone, turned by windows into the same 48 khz stereo the game's sound is kept in
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, (WORD)kChannels, kRate, kRate * kChannels * kBits / 8, (WORD)(kChannels * kBits / 8), (WORD)kBits, 0};
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) && SUCCEEDED(devices->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) &&
              SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))) &&
              SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, kAudioBuffer, 0, &fmt, nullptr)) &&
              SUCCEEDED(client->SetEventHandle(ready)) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
    while (ok && !stop) {
        WaitForSingleObject(ready, kAudioWaitMs);
        UINT32 frames = 0;
        while (!stop && SUCCEEDED(capture->GetNextPacketSize(&frames)) && frames) {
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 qpc = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) break;
            if (!paused) {
                Chunk c;
                c.at = qpc ? (LONGLONG)qpc : Now() - (LONGLONG)frames * kSec / kRate;
                c.pcm.assign((size_t)frames * kChannels, 0);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) std::memcpy(c.pcm.data(), data, c.pcm.size() * sizeof(int16_t));
                KeepSound(mic, std::move(c));
            }
            capture->ReleaseBuffer(frames);
        }
    }
    if (client) client->Stop();
    capture.Reset(), client.Reset(), device.Reset(), devices.Reset();
    CloseHandle(ready);
    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
}

void Impl::Pause(bool on) {
    if (paused == on) return;
    paused = on;
    std::lock_guard<std::mutex> lock(keepMu);
    video.clear(), audio.clear(), mic.clear(); // nothing from before the pause joins what comes after
    videoBytes = audioBytes = 0;
    if (!on) forceKey = true;
}

bool Impl::Follow(HWND w) {
    {
        std::lock_guard<std::mutex> lock(frameMu);
        if (w == window) return true;
    }
    EndCapture();
    alive = Capture(w);
    return alive;
}

size_t Impl::Bytes() const {
    std::lock_guard<std::mutex> lock(keepMu);
    return videoBytes + audioBytes;
}

double Impl::Seconds() const {
    std::lock_guard<std::mutex> lock(keepMu);
    return video.empty() ? 0 : (double)(video.back().at + video.back().dur - video.front().at) / kSec;
}

struct Snapshot { // what one save writes, copied out so recording goes on meanwhile
    std::vector<Packet> video;
    std::vector<Chunk> audio, mic;
    std::vector<BYTE> header;
    UINT width = 0, height = 0, bitrate = 0;
    int fps = 60;
    bool sound = false;
};

bool Write(const Snapshot& s, const std::wstring& path, std::wstring& why) {
    if (s.video.empty()) return why = L"Nothing recorded yet", false;
    LONGLONG start = s.video.front().at, end = s.video.back().at + s.video.back().dur;
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSinkWriter> w;
    if (FAILED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr.Get(), &w))) return why = L"The clips folder can't be written to", false;
    ComPtr<IMFMediaType> vt;
    MFCreateMediaType(&vt);
    vt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    vt->SetUINT32(MF_MT_AVG_BITRATE, s.bitrate);
    vt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(vt.Get(), MF_MT_FRAME_SIZE, s.width, s.height);
    MFSetAttributeRatio(vt.Get(), MF_MT_FRAME_RATE, (UINT32)s.fps, 1);
    MFSetAttributeRatio(vt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (!s.header.empty()) vt->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, s.header.data(), (UINT32)s.header.size());
    DWORD vs = 0, as = 0, ms = 0;
    bool voice = false;
    if (FAILED(w->AddStream(vt.Get(), &vs)) || FAILED(w->SetInputMediaType(vs, vt.Get(), nullptr))) return why = L"The video couldn't be written", false; // the same type in and out: copied, not encoded again
    bool sound = s.sound;
    if (sound) {
        ComPtr<IMFMediaType> at, pcm;
        MFCreateMediaType(&at);
        at->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        at->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        at->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        at->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        at->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kBits);
        at->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBytes);
        MFCreateMediaType(&pcm);
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kBits);
        pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * kBits / 8);
        pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * kBits / 8);
        sound = SUCCEEDED(w->AddStream(at.Get(), &as)) && SUCCEEDED(w->SetInputMediaType(as, pcm.Get(), nullptr));
        voice = sound && !s.mic.empty() && SUCCEEDED(w->AddStream(at.Get(), &ms)) && SUCCEEDED(w->SetInputMediaType(ms, pcm.Get(), nullptr)); // the voice alone as a second track, to turn down later in an editor
    }
    if (FAILED(w->BeginWriting())) return why = L"The clip couldn't be started", false;
    for (const Packet& p : s.video) {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buf;
        BYTE* dst = nullptr;
        if (FAILED(MFCreateMemoryBuffer((DWORD)p.bytes->size(), &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) return why = L"Out of memory while saving", false;
        std::memcpy(dst, p.bytes->data(), p.bytes->size());
        buf->Unlock();
        buf->SetCurrentLength((DWORD)p.bytes->size());
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(p.at - start);
        sample->SetSampleDuration(p.dur);
        if (p.key) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (FAILED(w->WriteSample(vs, sample.Get()))) return why = L"The disk is full or the clip couldn't be written", false;
    }
    if (sound) { // the sound laid out on the pictures' timeline: what wasn't heard is silence
        const size_t frames = (size_t)((end - start) * kRate / kSec);
        std::vector<int16_t> pcm(frames * kChannels, 0);
        auto lay = [&](const std::vector<Chunk>& chunks) { // added in, so the game and the voice are both heard; clamped, never wrapped round
            for (const Chunk& c : chunks) {
                LONGLONG off = (c.at - start) * kRate / kSec;
                for (size_t i = 0; i < c.pcm.size() / kChannels; ++i) {
                    LONGLONG at = off + (LONGLONG)i;
                    if (at < 0 || at >= (LONGLONG)frames) continue;
                    for (UINT ch = 0; ch < kChannels; ++ch) {
                        int16_t& d = pcm[(size_t)at * kChannels + ch];
                        d = (int16_t)(std::clamp)((int)d + c.pcm[i * kChannels + ch], -32768, 32767);
                    }
                }
            }
        };
        lay(s.audio);
        lay(s.mic); // the first track: the game and the voice together, what any player plays
        auto track = [&](DWORD stream, const std::vector<int16_t>& samples) {
            for (size_t at = 0; at < frames; at += kAudioChunk) {
                size_t n = (std::min)((size_t)kAudioChunk, frames - at);
                ComPtr<IMFSample> sample;
                ComPtr<IMFMediaBuffer> buf;
                BYTE* dst = nullptr;
                DWORD bytes = (DWORD)(n * kChannels * sizeof(int16_t));
                if (FAILED(MFCreateMemoryBuffer(bytes, &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) break;
                std::memcpy(dst, samples.data() + at * kChannels, bytes);
                buf->Unlock();
                buf->SetCurrentLength(bytes);
                MFCreateSample(&sample);
                sample->AddBuffer(buf.Get());
                sample->SetSampleTime((LONGLONG)at * kSec / kRate);
                sample->SetSampleDuration((LONGLONG)n * kSec / kRate);
                if (FAILED(w->WriteSample(stream, sample.Get()))) break;
            }
        };
        track(as, pcm);
        if (voice) {
            std::fill(pcm.begin(), pcm.end(), (int16_t)0);
            lay(s.mic);
            track(ms, pcm);
        }
    }
    if (FAILED(w->Finalize())) return why = L"The clip couldn't be finished", false;
    return true;
}

void WriteLater(std::shared_ptr<Snapshot> snap, const std::wstring& path, HWND notify, UINT msg) { // off the ui thread; nothing of the recorder's is touched
    std::thread([snap, path, notify, msg] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring why;
        bool ok = Write(*snap, path, why);
        if (!ok) DeleteFileW(path.c_str()); // a half-written clip isn't left behind
        CoUninitialize();
        auto* text = new std::wstring(ok ? path : why);
        if (!notify || !PostMessageW(notify, msg, ok ? 1 : 0, reinterpret_cast<LPARAM>(text))) delete text;
    }).detach();
}

void Impl::Save(int secs, int after, const std::wstring& path, HWND notify, UINT msg) {
    if (after <= 0) return WriteLater(TakeSnap(secs), path, notify, msg);
    {
        std::lock_guard<std::mutex> lock(saveMu);
        ++waiting2;
    }
    std::thread([this, secs, after, path, notify, msg] { // recording goes on for those seconds, then the whole stretch is written
        {
            std::unique_lock<std::mutex> lock(saveMu);
            saveCv.wait_for(lock, std::chrono::seconds(after), [this] { return stopping; }); // the game closing ends the wait early: what there is, is kept
        }
        auto snap = TakeSnap(secs + after);
        {
            std::lock_guard<std::mutex> lock(saveMu);
            --waiting2;
        }
        saveCv.notify_all();
        WriteLater(snap, path, notify, msg);
    }).detach();
}

std::shared_ptr<Snapshot> Impl::TakeSnap(int secs) {
    auto snap = std::make_shared<Snapshot>();
    {
        std::lock_guard<std::mutex> lock(keepMu);
        if (!video.empty()) {
            LONGLONG end = video.back().at + video.back().dur, from = end - (LONGLONG)(std::max)(secs, 1) * kSec;
            size_t first = 0;
            for (size_t i = 0; i < video.size(); ++i)
                if (video[i].key && video[i].at <= from) first = i; // the last keyframe at or before the start: the clip is never short
            snap->video.assign(video.begin() + (ptrdiff_t)first, video.end());
            LONGLONG start = snap->video.front().at;
            for (const Chunk& c : audio)
                if (c.at + (LONGLONG)(c.pcm.size() / kChannels) * kSec / kRate >= start) snap->audio.push_back(c);
            for (const Chunk& c : mic)
                if (c.at + (LONGLONG)(c.pcm.size() / kChannels) * kSec / kRate >= start) snap->mic.push_back(c);
        }
        snap->header = header;
    }
    snap->width = width, snap->height = height, snap->bitrate = bitrate, snap->fps = opt.fps, snap->sound = opt.audio > 0 || opt.mic;
    return snap;
}

void Impl::Stop() {
    {
        std::unique_lock<std::mutex> lock(saveMu); // saves waiting out their seconds take what there is now, before it goes
        stopping = true;
        saveCv.notify_all();
        saveCv.wait(lock, [this] { return waiting2 == 0; });
    }
    stop = true;
    SetEvent(frameReady); // the encoder wakes to see it
    EndCapture();
    if (encoder.joinable()) encoder.join();
    if (listener.joinable()) listener.join();
    if (micListener.joinable()) micListener.join();
    if (manager) manager.Reset();
}

std::once_flag started;

}

std::unique_ptr<Recorder> Recorder::Start(HWND game, DWORD pid, const Options& o, std::wstring& why) {
    std::call_once(started, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); }); // once, for the app's life: shut down with it
    if (!game || !IsWindow(game)) return why = L"The game's window is gone", nullptr;
    auto r = std::make_unique<Impl>(o);
    if (!r->Begin(game, pid, why)) return nullptr;
    return r;
}

bool Supported() {
    try {
        return WGC::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

}
