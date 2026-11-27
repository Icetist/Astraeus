// The clip studio. A window over one screen, made like the fullscreen player's: its own device, swapchain and composition
// visual, torn down on leaving. The clip plays through windows' media engine in frame-server mode on this window's own
// device, so decoding is the gpu's and each frame is copied once into a texture drawn as a bitmap. Trims are copies of the
// clip's own frames, never encoded again; saving over sends the original to the recycle bin, so nothing is lost by mistake.
#include "clipstudio.h"

#include <d2d1_1helper.h>
#include <mfapi.h>
#include <shellscalingapi.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <thread>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "sys_render.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace clips = sys::clips;

namespace {

constexpr wchar_t kClass[] = L"AstraeusClipStudio";
constexpr UINT kEngineMsg = WM_APP + 1, kWorkMsg = WM_APP + 2, kMixMsg = WM_APP + 3; // the media engine's news, the worker's, the mix engine's
constexpr float kPanelW = 340, kPanelGap = 20; // the effects panel, right of the picture
constexpr float kLeaveSec = 0.24f;
constexpr float kSide = 40, kTopH = 76; // the room's margins: the name above, the video between
constexpr float kBarW = 1040, kBarPad = 18, kBarR = 24, kBarBottom = 28; // the floating bar, as the long pill's editor has it
constexpr float kRowH = 44, kRulerH = 18, kTimelineH = 60, kLabelH = 20, kRowGap = 10; // transport, the times, the strip, the trim's ends
constexpr float kToolH = 34, kToolPad = 28, kToolGap = 8;
constexpr float kIconBtn = 36, kPlayBtn = 44, kIconPx = 20; // round tools, the play button between the skips
constexpr float kBarH = 2 * kBarPad + kRowH + kRowGap + kRulerH + kTimelineH + kLabelH + kRowGap + kToolH;
constexpr float kHandleW = 14, kHandleR = 6, kFrameT = 3, kHeadW = 2, kKnobR = 5; // the trim's grips sit just outside what's kept
constexpr float kStripR = 8, kDim = 0.62f; // the strip's corners; what's cut away, darkened
constexpr float kRulerMinPx = 76; // the times along the top never crowd closer than this
constexpr float kBubbleH = 22, kBubblePad = 9;
constexpr float kCodeW = 150, kCodeH = 32; // where it is, in its capsule
constexpr float kTipH = 30, kTipPad = 12, kTipGap = 8, kTipSlack = 4; // a hovered control's explanation
constexpr double kTipDelay = 0.4; // a pass over a control isn't a question: a moment's rest is
constexpr float kToastH = 36, kToastPad = 18, kToastGap = 12;
constexpr float kStroke = 1.8f; // the drawn icons, as heavy as the font's
constexpr float kPi = 3.14159265f;
constexpr float kVideoR = 18, kPlayBig = 72;
constexpr float kFramingRoom = 0.8f, kGhost = 0.22f; // framing: the frame smaller, with what's left out showing this faintly around it
constexpr float kTurnGap = 28, kTurnR = 8, kHandleHit = 16, kGrip = 16; // the turn and size handles
constexpr float kDegPerRad = 57.2957795f, kSnapEvery = 45, kSnapDeg = 3; // turning settles on straight angles when close
constexpr float kWheelTurn = 5, kWheelGrow = 1.08f; // a notch of the wheel
constexpr double kNudge = 1, kNudgeBig = 5, kMinKeep = 1;
enum Drawn : int { kDrawBack = -1, kDrawUndo = -2, kDrawRedo = -3, kDrawSkipBack = -4, kDrawSkipFwd = -5 }; // icons the font hasn't got: drawn as strokes
constexpr float kHoldSec = 0.9f;
constexpr int kPollFrames = 8; // a seek's picture arrives within a few frames
constexpr UINT kStripPx = 192;
constexpr ULONGLONG kSaidMs = 6000;
constexpr float kIntroRate = 9;

double Seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / freq.QuadPart;
}

std::wstring Clock(double s, bool tenths) { return StudioClock(s, tenths); }

std::wstring FreeName(const std::wstring& path, const std::wstring& suffix) { // "<name> edit.mp4", numbered when taken
    std::wstring base = path.substr(0, path.size() - 4) + suffix, out = base + L".mp4";
    for (int n = 2; GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) out = base + L" " + std::to_wstring(n) + L".mp4";
    return out;
}

void Head(Canvas& c, D2D1_POINT_2F tip, float dir, float h, Color ink) { // an arrowhead pointing along dir (+1 right, -1 left)
    c.Line(tip, {tip.x - dir * h, tip.y - h}, ink, kStroke);
    c.Line(tip, {tip.x - dir * h, tip.y + h}, ink, kStroke);
}

void DrawIcon(Canvas& c, int icon, D2D1_POINT_2F p, float s, Color ink) { // s: the box it fills, as a glyph's size
    switch (icon) {
    case kDrawBack: {
        float r = s * 0.32f;
        c.Line({p.x + r, p.y}, {p.x - r, p.y}, ink, kStroke);
        Head(c, {p.x - r, p.y}, -1, s * 0.22f, ink);
        break;
    }
    case kDrawUndo:
    case kDrawRedo: { // a hooked arrow; redo is undo in a mirror
        float m = icon == kDrawUndo ? 1.0f : -1.0f, rr = s * 0.22f;
        D2D1_POINT_2F o = {p.x + m * s * 0.1f, p.y + s * 0.05f};
        c.Arc(o, rr, m > 0 ? -kPi / 2 : kPi / 2, kPi, ink, kStroke);
        float top = o.y - rr, bottom = o.y + rr, tipX = p.x - m * s * 0.32f;
        c.Line({o.x, top}, {tipX, top}, ink, kStroke);
        Head(c, {tipX, top}, -m, s * 0.15f, ink);
        c.Line({o.x, bottom}, {o.x - m * s * 0.16f, bottom}, ink, kStroke);
        break;
    }
    case kDrawSkipBack:
    case kDrawSkipFwd: { // a circling arrow with the seconds inside, as players have it
        bool back = icon == kDrawSkipBack;
        float rr = s * 0.34f, gap = 0.5f;
        c.Arc(p, rr, -kPi / 2 + gap, 2 * kPi - 2 * gap, ink, kStroke);
        float side = back ? 1.0f : -1.0f; // the arrow ends on the gap's right going back, its left going on
        D2D1_POINT_2F end = {p.x + side * rr * std::sin(gap), p.y - rr * std::cos(gap)};
        Head(c, {end.x - side * s * 0.1f, end.y}, -side, s * 0.13f, ink);
        c.Text(L"5", Rect(p.x - rr, p.y - rr + 1, 2 * rr, 2 * rr), Weight::Semi, s * 0.34f, ink, Align::Center);
        break;
    }
    default:
        break;
    }
}

class Notify : public winrt::implements<Notify, IMFMediaEngineNotify> { // the engine's news, carried to the window's own thread
public:
    HWND hwnd = nullptr;
    UINT msg = kEngineMsg;
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        if (hwnd) PostMessageW(hwnd, msg, event, 0);
        return S_OK;
    }
};

}

std::wstring StudioClock(double s, bool tenths) {
    s = (std::max)(s, 0.0);
    int whole = (int)s;
    wchar_t buf[24];
    if (tenths) swprintf_s(buf, L"%d:%02d.%d", whole / 60, whole % 60, (int)((s - whole) * 10));
    else swprintf_s(buf, L"%d:%02d", whole / 60, whole % 60);
    return buf;
}

ClipStudio::ClipStudio(Model& m, HMONITOR mon, const std::wstring& file) : model(m), path(file), work(std::make_shared<Work>()) {
    static bool registered = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) return;
    MONITORINFO mi = {sizeof(mi)};
    if (!mon) mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    scale = SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) ? dx / 96.0f : 1.0f;
    const RECT& r = mi.rcMonitor;
    UINT pw = (UINT)(r.right - r.left), ph = (UINT)(r.bottom - r.top);
    W = pw / scale, H = ph / scale;
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass, L"Astraeus clip studio", WS_POPUP, r.left, r.top, (int)pw, (int)ph, nullptr, nullptr,
                           GetModuleHandleW(nullptr), this);
    if (!hwnd || !MakeDev(pw, ph)) return;
    access.Attach(hwnd, L"Astraeus clip studio");
    access.Enable(model.a11yReader != 0);
    Load();
    if (!engine) return;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd); // the keys work at once: space, the arrows, esc
    openedAt = Seconds();
}

ClipStudio::~ClipStudio() {
    Release();
    take.reset();
    if (mixEngine) mixEngine->Shutdown(); // lets go of the mix file, so it can go
    mixEngine.Reset();
    mixNotify.Reset();
    for (const std::wstring& f : mixFiles) DeleteFileW(f.c_str());
    videoBrush.Reset();
    strip.clear();
    videoBmp.Reset();
    videoTex.Reset();
    canvas.reset();
    visual.Reset(); // everything on the device first, then the driver lets it all go
    target.Reset();
    dcomp.Reset();
    backBuf.Reset();
    swap.Reset();
    ctx.Reset();
    manager.Reset();
    gfx::Settle(d3d.Get());
    if (frameWait) CloseHandle(frameWait);
    if (leaveTick) CloseHandle(leaveTick);
    if (hwnd) DestroyWindow(hwnd);
}

bool ClipStudio::MakeDev(UINT pxW, UINT pxH) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT; // the media engine decodes on it too
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr))) return false;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(d3d.As(&mt))) mt->SetMultithreadProtected(TRUE); // the engine works on it from its own threads
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(d3d.As(&dxgi)) || FAILED(gfx::D2D()->CreateDevice(dxgi.Get(), &device)) || (device->SetMaximumTextureMemory(64ull << 20), false) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = pxW, d.Height = pxH;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE; // every pixel painted: nothing for the compositor to blend
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (FAILED(factory->CreateSwapChainForComposition(d3d.Get(), &d, nullptr, &sc))) {
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(factory->CreateSwapChainForComposition(d3d.Get(), &d, nullptr, &sc))) return false;
    }
    if (!sc || FAILED(sc.As(&swap))) return false;
    swap->SetMaximumFrameLatency(1);
    frameWait = swap->GetFrameLatencyWaitableObject();
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                         96 * scale, 96 * scale);
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface))) || FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &backBuf))) return false;
    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp))) || FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual)) ||
        FAILED(visual->SetContent(swap.Get())) || FAILED(target->SetRoot(visual.Get())) || FAILED(dcomp->Commit()))
        return false;
    dc->SetTarget(backBuf.Get());
    dc->SetDpi(96 * scale, 96 * scale);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ctx = dc;
    canvas = std::make_unique<Canvas>(ctx.Get());
    if (SUCCEEDED(MFCreateDXGIDeviceManager(&managerToken, &manager))) manager->ResetDevice(d3d.Get(), managerToken);
    return true;
}

bool ClipStudio::MakeEngine() {
    static std::once_flag started;
    std::call_once(started, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    ComPtr<IMFMediaEngineClassFactory> factory;
    ComPtr<IMFAttributes> attr;
    if (!manager || FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || FAILED(MFCreateAttributes(&attr, 3))) return false;
    auto n = winrt::make_self<Notify>();
    n->hwnd = hwnd;
    notify.Attach(static_cast<IMFMediaEngineNotify*>(n.detach()));
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager.Get()); // decoded on this window's gpu, handed over as textures
    attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    return SUCCEEDED(factory->CreateInstance(0, attr.Get(), &engine)); // no window of its own: frames are asked for, so the studio draws them in place
}

void ClipStudio::Load() {
    if (!engine && !MakeEngine()) return;
    size_t slash = path.find_last_of(L'\\');
    name = path.substr(slash + 1, path.size() - slash - 5);
    std::wstring dir = path.substr(0, slash);
    game = dir.substr(dir.find_last_of(L'\\') + 1);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    info.clear();
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        SYSTEMTIME st, lt;
        FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
        SystemTimeToTzSpecificLocalTime(nullptr, &st, &lt);
        wchar_t day[48];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &lt, nullptr, day, 48, nullptr);
        double mb = (double)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) / 1048576.0;
        wchar_t size[24];
        swprintf_s(size, L"%.1f MB", mb);
        info = (game == L"Astraeus" ? L"" : game + L"  ·  ") + std::wstring(day) + L"  ·  " + size;
    }
    BSTR url = SysAllocString(path.c_str());
    engine->SetSource(url);
    SysFreeString(url);
    engine->Load();
    haveFrame = false, poll = kPollFrames;
    gamePcm.reset(), soundAsked = false, noSound = false; // read again: a save over changed it
    Poke();
}

void ClipStudio::Release() {
    if (mixEngine) mixEngine->Pause();
    if (!engine) return;
    engine->Pause();
    engine->Shutdown(); // the file let go of; the last picture stays on screen
    engine.Reset();
    notify.Reset();
    playing = previewing = false;
}

void ClipStudio::OnEngine(DWORD event) {
    if (!engine) return;
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: {
        len = engine->GetDuration();
        DWORD w = 0, h = 0;
        engine->GetNativeVideoSize(&w, &h);
        if (w && h && (w != vw || h != vh || !videoTex)) { // a texture the size of the clip, drawn as a bitmap on this device
            vw = w, vh = h;
            videoTex.Reset(), videoBmp.Reset();
            D3D11_TEXTURE2D_DESC d{vw, vh, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
            ComPtr<IDXGISurface> surface;
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(d3d->CreateTexture2D(&d, nullptr, &videoTex)) && SUCCEEDED(videoTex.As(&surface))) ctx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &videoBmp);
            videoBrush.Reset();
            if (videoBmp) ctx->CreateBitmapBrush(videoBmp.Get(), &videoBrush);
        }
        if (cut.pieces.empty() || std::fabs(cut.pieces.back().to - len) > 0.05) { // a new clip, or one saved over: nothing done to it yet. a rename keeps the edit
            clipedit::Init(cut, len);
            steps.assign(1, cut), stepAt = 0, leaveAsked = false;
            mixWant = mixMade = 0, mixing = false;
        }
        Strip();
        LoadSound();
        break;
    }
    case MF_MEDIA_ENGINE_EVENT_PLAYING:
        playing = true;
        break;
    case MF_MEDIA_ENGINE_EVENT_PAUSE:
    case MF_MEDIA_ENGINE_EVENT_ENDED:
        playing = false, previewing = false;
        break;
    case MF_MEDIA_ENGINE_EVENT_ERROR:
        Say(L"This clip couldn't be played");
        break;
    default:
        break;
    }
    poll = kPollFrames; // a seek, the first frame or a state change: the picture is looked for over the next few frames
    Poke();
}

void ClipStudio::TakeFrame() { // before the frame starts: a new picture from the engine into the video texture
    if (!engine || !videoTex) return;
    LONGLONG pts = 0;
    if (engine->OnVideoStreamTick(&pts) != S_OK) return;
    MFVideoNormalizedRect src{0, 0, 1, 1};
    RECT dst{0, 0, (LONG)vw, (LONG)vh};
    MFARGB black{0, 0, 0, 255};
    if (SUCCEEDED(engine->TransferVideoFrame(videoTex.Get(), &src, &dst, &black))) haveFrame = true;
}

void ClipStudio::Strip() { // pictures along the timeline, decoded off the ui thread; drawn when they're in
    float bw = (std::min)(kBarW, W - 2 * kSide) - 2 * kBarPad;
    double aspect = vh ? (double)vw / vh : 16.0 / 9;
    stripCount = (std::max)(4, (int)std::lround(bw / (kTimelineH * aspect)));
    strip.assign((size_t)stripCount, nullptr);
    auto w = work;
    std::wstring file = path;
    std::vector<double> when((size_t)stripCount);
    for (int i = 0; i < stripCount; ++i) when[(size_t)i] = len * (i + 0.5) / stripCount;
    int gen;
    {
        std::lock_guard<std::mutex> lock(w->mu);
        gen = ++w->stripGen;
        w->strip.clear();
    }
    HWND to = hwnd;
    std::thread([w, file, when, gen, to] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        clips::Frames(file, when, kStripPx, [&](size_t i, clips::Picture&& p) { // one opening of the clip for all of them, each shown as it's ready
            {
                std::lock_guard<std::mutex> lock(w->mu);
                if (w->stripGen != gen) return; // a newer strip is on its way: this one's done with
                w->strip.emplace_back(i, std::move(p));
            }
            PostMessageW(to, kWorkMsg, 0, 0);
        });
        CoUninitialize();
    }).detach();
}

void ClipStudio::Seek(double s) {
    s = (std::clamp)(s, 0.0, (std::max)(len, 0.0));
    at = s;
    if (engine) engine->SetCurrentTime(s);
    poll = kPollFrames;
    Poke();
}

namespace {

bool MakeClip(const std::wstring& file, const clipedit::Edit& c, std::shared_ptr<const sys::audiofx::Pcm> sound, std::shared_ptr<const sys::audiofx::Pcm> song, const std::wstring& out,
              std::atomic<int>* pct, std::wstring& why) {
    if (!clipedit::Reworked(c)) return clips::Trim(file, c.from, c.to, out, why); // only trimmed: copied as it is, instantly and losing nothing
    if (!sound) { // not read yet: read now
        sys::audiofx::Pcm own;
        std::wstring skip;
        if (sys::audiofx::Decode(file, own, skip)) sound = std::make_shared<const sys::audiofx::Pcm>(std::move(own));
    }
    sys::audiofx::Pcm mix = sys::audiofx::Mix(c, sound.get(), c.song.empty() ? nullptr : song.get());
    return sys::render::Render(file, c, mix, out, pct, why);
}

}

void ClipStudio::Toggle() {
    if (!engine) return;
    previewing = false;
    if (engine->IsPaused() || engine->IsEnded()) {
        if (Edited()) { // plays as it'll be saved: from the trim's start when outside it, past removed parts
            bool outside = at < cut.from || at >= cut.to - 0.05;
            if (outside || !clipedit::Kept(cut, at)) Seek(clipedit::NextKept(cut, outside ? cut.from : at));
        } else if (at >= len - 0.05) {
            Seek(0); // at the end: from the top again
        }
        engine->Play();
    } else {
        engine->Pause();
    }
    Poke();
}

void ClipStudio::Preview() {
    if (!engine) return;
    Seek(clipedit::NextKept(cut, cut.from));
    engine->Play();
}

bool ClipStudio::Edited() const { return len > 0 && (clipedit::Trimmed(cut, len) || clipedit::Reworked(cut)); }

void ClipStudio::Commit() {
    if (steps[stepAt] == cut) return;
    steps.resize(stepAt + 1);
    steps.push_back(cut);
    stepAt = steps.size() - 1;
    leaveAsked = false;
    MixLater();
}

void ClipStudio::Undo() {
    if (stepAt == 0) return;
    cut = steps[--stepAt];
    MixLater();
    Poke();
}

void ClipStudio::Redo() {
    if (stepAt + 1 >= steps.size()) return;
    cut = steps[++stepAt];
    MixLater();
    Poke();
}

void ClipStudio::Revert() {
    clipedit::Init(cut, len);
    Commit();
    Seek(0);
}

void ClipStudio::SaveOver() { // the edit takes the clip's place; the original goes to the recycle bin, so it can still come back
    if (!Edited() || busy) return;
    busy = true;
    Say(clipedit::Reworked(cut) ? L"Making the clip…" : L"Saving…");
    work->pct = clipedit::Reworked(cut) ? 0 : -1;
    Release(); // the engine lets go of the file first
    auto w = work;
    std::wstring file = path;
    Cut c = cut;
    auto sound = gamePcm, song = songPcm;
    HWND to = hwnd;
    std::thread([w, file, c, sound, song, to] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring tmp = file.substr(0, file.size() - 4) + L" (saving).mp4", why;
        bool ok = MakeClip(file, c, sound, song, tmp, &w->pct, why);
        if (ok && !(clips::Recycle(file) && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))) ok = false, why = L"The original couldn't be replaced: is it open somewhere?";
        if (!ok) DeleteFileW(tmp.c_str());
        CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(w->mu);
            w->done = true, w->ok = ok, w->kind = 1, w->text = ok ? L"Saved. The original is in the Recycle Bin if you want it back" : why;
        }
        PostMessageW(to, kWorkMsg, 0, 0);
    }).detach();
}

void ClipStudio::SaveCopy() { // the edit beside the clip, which stays as it was
    if (!Edited() || busy) return;
    busy = true;
    Say(clipedit::Reworked(cut) ? L"Making a copy…" : L"Saving a copy…");
    work->pct = clipedit::Reworked(cut) ? 0 : -1;
    auto w = work;
    std::wstring file = path, out = FreeName(path, L" edit");
    Cut c = cut;
    auto sound = gamePcm, song = songPcm;
    HWND to = hwnd;
    std::thread([w, file, out, c, sound, song, to] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring why;
        bool ok = MakeClip(file, c, sound, song, out, &w->pct, why);
        if (!ok) DeleteFileW(out.c_str());
        CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(w->mu);
            w->done = true, w->ok = ok, w->kind = 2, w->newPath = out;
            w->text = ok ? L"Saved a copy: " + out.substr(out.find_last_of(L'\\') + 1, out.size() - out.find_last_of(L'\\') - 5) : why;
        }
        PostMessageW(to, kWorkMsg, 0, 0);
    }).detach();
}

void ClipStudio::ShrinkCopy() { // made small enough for a chat's upload limit, off the ui thread, then onto the clipboard
    if (busy) return;
    busy = true;
    Say(L"Making a copy small enough for Discord…");
    auto w = work;
    std::wstring file = path;
    HWND to = hwnd;
    std::thread([w, file, to] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring out, why;
        bool ok = clips::Shrink(file, clips::kDiscordBytes, out, why);
        CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(w->mu);
            w->done = true, w->ok = ok, w->kind = 4, w->newPath = out, w->text = why;
        }
        PostMessageW(to, kWorkMsg, 0, 0);
    }).detach();
}

void ClipStudio::Bin() {
    Release();
    if (clips::Recycle(path)) {
        ++model.clipsEpoch;
        Leave();
        return;
    }
    Say(L"It couldn't be deleted: is it open somewhere?");
    Load();
}

void ClipStudio::StartName() {
    if (busy) return;
    naming = true;
    nameEdit = name;
    Poke();
}

void ClipStudio::EndName(bool keep) {
    naming = false;
    Poke();
    if (!keep || nameEdit.empty() || nameEdit == name) return;
    Release();
    std::wstring renamed, why;
    if (clips::Rename(path, nameEdit, renamed, why)) {
        path = renamed;
        ++model.clipsEpoch;
        Say(L"Renamed");
    } else {
        Say(why);
    }
    Load();
}

void ClipStudio::Leave() {
    if (leaving) return;
    Release();
    leaving = true;
    leaveAt = Seconds();
    float pw = W * scale, ph = H * scale;
    leaveTick = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER first;
    first.QuadPart = -160000; // 16 ms
    ComPtr<IDCompositionAnimation> grow, fade;
    ComPtr<IDCompositionScaleTransform> shrink;
    ComPtr<IDCompositionEffectGroup> fx;
    if (!leaveTick || !SetWaitableTimer(leaveTick, &first, 16, nullptr, nullptr, FALSE) || !dcomp || !visual || FAILED(dcomp->CreateAnimation(&grow)) ||
        FAILED(dcomp->CreateAnimation(&fade)) || FAILED(dcomp->CreateScaleTransform(&shrink)) || FAILED(dcomp->CreateEffectGroup(&fx))) {
        done = true; // nothing to animate with: just go
        return;
    }
    const float T = kLeaveSec, end = 0.95f; // a touch smaller while it fades: the way the player leaves
    grow->AddCubic(0, 1, -2 * (1 - end) / T, (1 - end) / (T * T), 0);
    grow->End(T, end);
    fade->AddCubic(0, 1, 0, -3 / (T * T), 2 / (T * T * T));
    fade->End(T, 0);
    shrink->SetCenterX(pw / 2);
    shrink->SetCenterY(ph / 2);
    shrink->SetScaleX(grow.Get());
    shrink->SetScaleY(grow.Get());
    fx->SetOpacity(fade.Get());
    visual->SetTransform(shrink.Get());
    visual->SetEffect(fx.Get());
    dcomp->Commit();
}

LRESULT CALLBACK ClipStudio::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* self = reinterpret_cast<ClipStudio*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && !self->hwnd) self->hwnd = h;
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT ClipStudio::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kEngineMsg:
        OnEngine((DWORD)wp);
        return 0;
    case kMixMsg: // the mix engine's news: SyncMix looks at it each frame
        Poke();
        return 0;
    case kWorkMsg: {
        Poke();
        std::lock_guard<std::mutex> lock(work->mu);
        if (work->soundDone) { // the clip's own sound is in: the mix and the quiet-part finder can use it
            work->soundDone = false;
            gamePcm = work->sound, noSound = !gamePcm;
            work->sound.reset();
            MixLater();
        }
        if (work->songDone) {
            work->songDone = false;
            songBusy = false;
            if (work->song) {
                songs[work->songPath] = work->song, songBeats[work->songPath] = std::move(work->beats);
                songFor = L"\x1"; // looked up again for the edit's song
                size_t slash = work->songPath.find_last_of(L'\\'), dot = work->songPath.find_last_of(L'.');
                cut.song = work->songPath;
                cut.songName = work->songPath.substr(slash + 1, (dot != std::wstring::npos && dot > slash ? dot : work->songPath.size()) - slash - 1);
                cut.songAt = 0;
                Commit();
                Say(songBeats[work->songPath].empty() ? L"Music added" : L"Music added. Its beats show on the timeline");
            } else {
                Say(work->songWhy.empty() ? L"That song couldn't be read" : L"That song couldn't be read: " + work->songWhy);
            }
            work->song.reset();
        }
        if (work->mixDone) { // a mix is written: it plays from here, unless a newer one is already wanted and on its way
            work->mixDone = false;
            mixBusy = false;
            if (!work->mixPath.empty()) {
                mixFiles.push_back(work->mixPath);
                if (!mixEngine) {
                    ComPtr<IMFMediaEngineClassFactory> factory;
                    ComPtr<IMFAttributes> attr;
                    if (SUCCEEDED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) && SUCCEEDED(MFCreateAttributes(&attr, 1))) {
                        auto n = winrt::make_self<Notify>();
                        n->hwnd = hwnd, n->msg = kMixMsg;
                        mixNotify.Attach(static_cast<IMFMediaEngineNotify*>(n.detach()));
                        attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, mixNotify.Get());
                        factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attr.Get(), &mixEngine);
                    }
                }
                if (mixEngine) {
                    BSTR url = SysAllocString(work->mixPath.c_str());
                    mixEngine->SetSource(url);
                    SysFreeString(url);
                    mixEngine->Load();
                    mixMade = work->mixVer, mixing = true;
                }
            }
            if (mixMade != mixWant) mixDue = Seconds(); // changed again meanwhile: once more
        }
        if (work->done) {
            work->done = false, busy = false;
            work->pct = -1;
            if (work->kind == 4) { // the small copy is ready: onto the clipboard, from this thread
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                double mb = GetFileAttributesExW(work->newPath.c_str(), GetFileExInfoStandard, &fa) ? (double)((((uint64_t)fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) / 1e6 : 0;
                wchar_t size[24];
                swprintf_s(size, L"%.1f MB", mb);
                Say(work->ok && clips::Copy(hwnd, work->newPath) ? L"Copied a " + std::wstring(size) + L" copy. Paste it into Discord" : work->text.empty() ? L"It couldn't be copied" : work->text);
                return 0;
            }
            Say(work->text);
            if (work->ok) ++model.clipsEpoch;
            if (work->kind == 1) { // the edited clip in the original's place: it's the clip now, with nothing done to it
                if (work->ok) cut = Cut{}, mixing = false;
                Load();
            }
            if (work->kind == 2 && work->ok && !work->newPath.empty()) { // the copy is what's being edited from here; the original stays as it was
                Release();
                path = work->newPath;
                cut = Cut{}, mixing = false;
                Load();
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        input.mouse = {(short)LOWORD(lp) / scale, (short)HIWORD(lp) / scale};
        Poke();
        return 0;
    case WM_MOUSEWHEEL:
        input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        Poke();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        input.pressPos = input.mouse;
        input.pressed = input.down = true;
        Poke();
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        input.released = true;
        input.down = false;
        Poke();
        return 0;
    case WM_CAPTURECHANGED:
        input.down = false;
        Poke();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            bool grab = Framing() && (frameKind == 1 || (!frameKind && Inside(videoRect, input.mouse))); // the picture moves under the hand
            SetCursor(LoadCursorW(nullptr, grab ? IDC_SIZEALL : ui.Hand() ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_GETOBJECT: {
        LRESULT out = 0;
        if (access.OnGetObject(wp, lp, out)) return out;
        break;
    }
    case Access::kMsg:
        access.Apply(ui, wp, lp);
        Poke();
        return 0;
    case WM_CHAR:
        if (!naming) return 0;
        if (wp == 8) {
            if (!nameEdit.empty()) nameEdit.pop_back();
        } else if (wp >= 32 && wp != 127 && nameEdit.size() < 120) {
            nameEdit += (wchar_t)wp;
        }
        Poke();
        return 0;
    case WM_KEYUP:
        if (wp == VK_RETURN || wp == VK_SPACE) ui.KeyHold(false);
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        Poke();
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (naming) { // the name is being typed: enter keeps it, esc doesn't
            if (wp == VK_RETURN) EndName(true);
            else if (wp == VK_ESCAPE) EndName(false);
            return 0;
        }
        if (model.a11yKeys && (wp == VK_TAB || ui.Focused())) { // tab picks a control; until then the keys are the player's
            if (wp == VK_TAB) return ui.Tab(shift ? -1 : 1), 0;
            if (wp == VK_ESCAPE) return ui.Unfocus(), 0;
            if (wp == VK_RETURN || wp == VK_SPACE) return ui.FireFocus(), ui.KeyHold(true), 0;
        }
        if (ctrl && wp == 'Z') shift ? Redo() : Undo();
        else if (ctrl && wp == 'Y') Redo();
        else if (ctrl && wp == 'S') shift ? SaveCopy() : SaveOver();
        else if (wp == VK_SPACE) Toggle();
        else if (wp == VK_LEFT || wp == VK_RIGHT) Seek(at + (wp == VK_LEFT ? -1 : 1) * (shift ? kNudgeBig : kNudge));
        else if (wp == VK_HOME) Seek(cut.from);
        else if (wp == VK_END) Seek(cut.to);
        else if (wp == 'I') cut.from = (std::min)(at, cut.to - kMinKeep), Commit(); // in and out at the playhead, as editors have it
        else if (wp == 'O') cut.to = (std::max)(at, cut.from + kMinKeep), Commit();
        else if (wp == 'P') Preview();
        else if (wp == 'S') SplitHere();
        else if (wp == VK_DELETE) RemoveHere();
        else if (wp == VK_F2) StartName();
        else if (wp == VK_ESCAPE) AskLeave(L"Your trim isn't saved. Esc again to leave without it");
        return 0;
    }
    case WM_CLOSE:
        Leave();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ClipStudio::Tip(const D2D1_RECT_F& r, const wchar_t* text) {
    if (text && !input.down && Inside(r, input.mouse)) tipNow = text, tipRect = r;
}

bool ClipStudio::Tool(const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* label, const wchar_t* tip, bool primary, bool live) {
    bool hit = sheet::Act(ui, t, Ui::Id(id), r, label, primary, false, live && !busy);
    Tip(r, tip);
    return hit;
}

bool ClipStudio::IconTool(const Theme& t, const char* id, D2D1_POINT_2F c, int icon, const wchar_t* tip, bool live) {
    D2D1_RECT_F r = Rect(c.x - kIconBtn / 2, c.y - kIconBtn / 2, kIconBtn, kIconBtn);
    bool on = live && !busy;
    Ui::Hit h = ui.Button(Ui::Id(id), r);
    ui.Name(tip);
    float lit = ui.Smooth(Ui::Id(id, 1), h.hover && on ? 1.0f : 0, w::kRowFade);
    if (lit > 0.01f) canvas->Dot(c, kIconBtn / 2, t.text.Alpha(0.09f * lit));
    Color ink = on ? t.textDim.Mix(t.text, lit) : t.textLow.Alpha(0.45f); // not now: still there, plainly resting
    if (icon >= 0) canvas->Icon(icon, c, kIconPx, ink);
    else DrawIcon(*canvas, icon, c, kIconPx + 2, ink);
    Tip(r, tip);
    return h.clicked && on;
}

void ClipStudio::AskLeave(const wchar_t* again) {
    if (Edited() && !leaveAsked && steps.size() > 1) leaveAsked = true, Say(again);
    else Leave();
}

void ClipStudio::DrawTop(const Theme& t, float a) {
    Canvas& c = *canvas;
    float y = 22 + (1 - a) * -8, mid = y + kIconBtn / 2;
    if (IconTool(t, "cs.back", {kSide + kIconBtn / 2, mid}, kDrawBack, L"Back to the games page  ·  Esc", true)) AskLeave(L"Your trim isn't saved. Press back again to leave without it");
    float x = kSide + kIconBtn + 10, room = W - x - kSide - 280;
    D2D1_RECT_F title = Rect(x, mid - 17, room, 26);
    if (naming) {
        D2D1_RECT_F f = Rect(title.left - 8, title.top - 4, (std::min)(room, 520.0f), 34);
        c.Fill(f, 10, t.track.Alpha(a));
        c.Stroke(f, 10, t.accent.Alpha(0.85f * a), 1.5f);
        c.Text(nameEdit + ((int)(Seconds() * 2) % 2 == 0 ? L"|" : L""), Inset(f, 10, 0), Weight::Semi, 18, t.text.Alpha(a));
        ui.Wake();
    } else {
        float nw = (std::min)(c.Measure(name, Weight::Semi, 20), room - 30);
        D2D1_RECT_F hit = Rect(title.left - 6, title.top - 4, nw + 40, 34);
        Ui::Hit h = ui.Button(Ui::Id("cs.title"), hit);
        ui.Name(L"Rename: " + name);
        float lit = ui.Smooth(Ui::Id("cs.title", 1), h.hover ? 1.0f : 0, w::kRowFade);
        if (lit > 0.01f) c.Fill(hit, 10, t.text.Alpha(0.06f * lit * a));
        c.Text(name, Rect(title.left, title.top, nw, 26), Weight::Semi, 20, t.text.Alpha(a));
        c.Icon(glyph::Edit, {title.left + nw + 18, title.top + 13}, 15, t.textLow.Mix(t.text, lit).Alpha(a)); // always there: the name can be changed
        Tip(hit, L"Rename the clip  ·  F2");
        if (h.clicked) StartName();
    }
    std::wstring meta = info;
    if (vh) meta += L"  ·  " + std::to_wstring(vh) + L"p";
    if (len > 0) meta += L"  ·  " + Clock(len, false);
    c.Text(meta, Rect(x, mid + 11, room, 18), Weight::Regular, 12.5f, t.textDim.Alpha(a));
    float doneW = c.Measure(L"Done", Weight::Medium, 12) + 2 * kToolPad;
    D2D1_RECT_F doneBtn = Rect(W - kSide - doneW, mid - kToolH / 2, doneW, kToolH);
    float fw = c.Measure(L"Show in folder", Weight::Medium, 12) + kToolPad;
    if (Tool(t, "cs.folder", Rect(doneBtn.left - kToolGap - fw, doneBtn.top, fw, kToolH), L"Show in folder", L"Opens its folder with the clip picked out", false, true)) clips::Show(path);
    if (Tool(t, "cs.done", doneBtn, L"Done", L"Back to the games page  ·  Esc", true, true)) AskLeave(L"Your trim isn't saved. Press Done again to leave without it");
}

namespace {

bool Close(D2D1_POINT_2F p, D2D1_POINT_2F q, float d) { return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) <= d * d; }

float Wrap(float deg) { // into -180..180
    while (deg > 180) deg -= 360;
    while (deg <= -180) deg += 360;
    return deg;
}

}

void ClipStudio::DrawFraming(const Theme& t, const D2D1_RECT_F& r, const float m[6], float k, float a) {
    Canvas& c = *canvas;
    D2D1_POINT_2F mid = {(r.left + r.right) / 2, (r.top + r.bottom) / 2}, ms = input.mouse;
    c.Stroke(r, kVideoR, Color{1, 1, 1, 0.9f * a}, 2);
    for (int i = 1; i < 3; ++i) { // thirds, as a camera's guide has them
        float gx = r.left + Width(r) * i / 3, gy = r.top + Height(r) * i / 3;
        c.Line({gx, r.top}, {gx, r.bottom}, Color{1, 1, 1, 0.22f * a}, 1, false);
        c.Line({r.left, gy}, {r.right, gy}, Color{1, 1, 1, 0.22f * a}, 1, false);
    }
    D2D1_POINT_2F turn = {mid.x, r.top - kTurnGap}, grow = {r.right, r.bottom};
    bool onTurn = frameKind == 3 || (!frameKind && Close(ms, turn, kHandleHit)), onGrow = frameKind == 2 || (!frameKind && Close(ms, grow, kHandleHit));
    c.Line({mid.x, r.top}, turn, Color{1, 1, 1, 0.7f * a}, 1.5f);
    c.Dot(turn, kTurnR + (onTurn ? 2.0f : 0.0f), Color{1, 1, 1, a}); // turns it
    c.Dot(turn, kTurnR - 2.5f, t.accent.Alpha(a));
    D2D1_RECT_F grip = Rect(grow.x - kGrip / 2, grow.y - kGrip / 2, kGrip, kGrip); // sizes it
    c.Fill(Inset(grip, onGrow ? -2.0f : 0.0f, onGrow ? -2.0f : 0.0f), 4, Color{1, 1, 1, a});
    c.Fill(Inset(grip, 3, 3), 2, t.accent.Alpha(a));
    Tip(Rect(turn.x - kHandleHit, turn.y - kHandleHit, 2 * kHandleHit, 2 * kHandleHit), L"Drag to turn the picture. It settles on straight angles");
    Tip(Rect(grow.x - kHandleHit, grow.y - kHandleHit, 2 * kHandleHit, 2 * kHandleHit), L"Drag to make the picture bigger or smaller");
    if (!onTurn && !onGrow) Tip(r, L"Drag to move the picture. Scroll to resize it, Shift+scroll to turn it");
    if (input.pressed) {
        if (Close(ms, turn, kHandleHit)) {
            frameKind = 3, grabAngle = cut.angle, grabTurn = std::atan2(ms.y - mid.y, ms.x - mid.x);
        } else if (Close(ms, grow, kHandleHit)) {
            frameKind = 2, grabZoom = cut.zoom, grabDist = (std::max)(std::hypot(ms.x - mid.x, ms.y - mid.y), 1.0f);
        } else if (Inside(r, ms)) {
            frameKind = 1, frameGrab = ms;
            clipedit::FrameAt(cut, at, grabX, grabY);
            if (cut.moving && engine && !engine->IsPaused()) engine->Pause(); // a spot is set at a moment: the clip holds still for it
        }
    }
    if (frameKind) {
        if (input.down) {
            if (frameKind == 1) { // the hand's move, in the frame's pixels, back into the picture's: the picture follows the hand
                float du = (ms.x - frameGrab.x) / k, dv = (ms.y - frameGrab.y) / k;
                float nx = (std::clamp)(grabX - (m[0] * du + m[1] * dv) / vw, 0.0f, 1.0f), ny = (std::clamp)(grabY - (m[3] * du + m[4] * dv) / vh, 0.0f, 1.0f);
                if (cut.moving) clipedit::SetSpot(cut, at, nx, ny);
                else cut.frameX = nx, cut.frameY = ny;
            } else if (frameKind == 2) {
                cut.zoom = (std::clamp)(grabZoom * std::hypot(ms.x - mid.x, ms.y - mid.y) / grabDist, clipedit::kMinZoom, clipedit::kMaxZoom);
            } else {
                float deg = Wrap(grabAngle + (std::atan2(ms.y - mid.y, ms.x - mid.x) - grabTurn) * kDegPerRad);
                float straight = std::round(deg / kSnapEvery) * kSnapEvery;
                cut.angle = std::fabs(deg - straight) < kSnapDeg ? Wrap(straight) : deg;
            }
            ui.Wake();
        } else {
            frameKind = 0;
            Commit();
        }
    }
    if (!frameKind && Inside(Inset(r, -kHandleHit, -kHandleHit), ms) && input.wheel != 0) {
        if (GetKeyState(VK_SHIFT) < 0) cut.angle = Wrap(cut.angle + input.wheel * kWheelTurn);
        else cut.zoom = (std::clamp)(cut.zoom * std::pow(kWheelGrow, input.wheel), clipedit::kMinZoom, clipedit::kMaxZoom);
        input.wheel = 0;
        Commit();
    }
}

void ClipStudio::DrawVideo(const Theme& t, const D2D1_RECT_F& box, float a) {
    Canvas& c = *canvas;
    int ow = (int)vw, oh = (int)vh;
    if (vw && vh) clipedit::OutSize(cut.shape, (int)vw, (int)vh, ow, oh);
    double oa = vw && vh ? (double)ow / oh : 16.0 / 9;
    bool framing = Framing() && vw && vh;
    float room = framing ? kFramingRoom : 1; // framing: space around the frame for what's left out and the handles
    float w = Width(box) * room, h = (float)(w / oa);
    if (h > Height(box) * room) h = Height(box) * room, w = (float)(h * oa);
    D2D1_RECT_F r = Rect((box.left + box.right - w) / 2, (box.top + box.bottom - h) / 2, w, h); // the finished frame
    videoRect = r;
    float m[6] = {1, 0, 0, 0, 1, 0};
    if (vw && vh) clipedit::Place(cut, at, (int)vw, (int)vh, ow, oh, m);
    float k = vw ? w / ow : 1, det = m[0] * m[4] - m[1] * m[3];
    D2D1_MATRIX_3X2_F base;
    ctx->GetTransform(&base);
    D2D1_MATRIX_3X2_F fwd = D2D1::Matrix3x2F(m[4] / det, -m[3] / det, -m[1] / det, m[0] / det, (m[1] * m[5] - m[4] * m[2]) / det, (m[3] * m[2] - m[0] * m[5]) / det) *
                            D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(r.left, r.top) * base; // the picture's pixels onto the screen, as it's placed in the frame
    bool shown = videoBmp && haveFrame && std::fabs(det) > 1e-12f;
    auto picture = [&](float opacity) {
        ctx->SetTransform(fwd);
        ctx->DrawBitmap(videoBmp.Get(), D2D1::RectF(0, 0, (float)vw, (float)vh), opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        ctx->SetTransform(base);
    };
    if (framing && shown) picture(kGhost * a); // what falls outside the frame, faintly: what moving it would bring in
    c.Fill(r, kVideoR, Color{0, 0, 0, a});
    if (shown) { // the picture as it'll be saved, inside the frame's rounded corners
        ComPtr<ID2D1RoundedRectangleGeometry> corners;
        if (SUCCEEDED(gfx::D2D()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, kVideoR, kVideoR), &corners))) {
            ctx->PushLayer(D2D1::LayerParameters1(r, corners.Get()), nullptr);
            picture(a);
            ctx->PopLayer();
        }
    }
    if (framing) {
        DrawFraming(t, r, m, k, a);
        return; // the picture is for placing here: no click-to-play under the hand
    }
    Ui::Hit h2 = ui.Button(Ui::Id("cs.video"), r);
    ui.Name(playing ? L"Pause" : L"Play");
    if (h2.clicked) Toggle();
    float show = ui.Smooth(Ui::Id("cs.bigplay"), !playing || h2.hover ? 1.0f : 0, 10); // a big play mark while it rests
    if (show > 0.01f) {
        D2D1_POINT_2F mid = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        c.Dot(mid, kPlayBig / 2, Color{0, 0, 0, 0.45f * show * a});
        c.Icon(playing ? glyph::Pause : glyph::Play, mid, kPlayBig * 0.42f, Color{1, 1, 1, show * a}, true);
    }
}

void ClipStudio::Bubble(const Theme& t, float x, float y, const std::wstring& text) {
    Canvas& c = *canvas;
    float w = c.Measure(text, Weight::Semi, 11.5f) + kTipSlack + 2 * kBubblePad;
    D2D1_RECT_F r = Rect((std::clamp)(x - w / 2, barRect.left + 8, barRect.right - 8 - w), y, w, kBubbleH);
    c.Fill(r, kBubbleH / 2, t.text);
    c.Text(text, r, Weight::Semi, 11.5f, t.bg, Align::Center);
}

void ClipStudio::Ruler(const Theme& t, const D2D1_RECT_F& bar) {
    if (len <= 0) return;
    Canvas& c = *canvas;
    static constexpr double kSteps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
    double step = kSteps[std::size(kSteps) - 1];
    for (double s : kSteps)
        if (s / len * Width(bar) >= kRulerMinPx) {
            step = s;
            break;
        }
    auto xOf = [&](double s) { return bar.left + (float)(s / len) * Width(bar); };
    float base = bar.top - 4;
    for (int i = 0;; ++i) {
        double v = i * step;
        if (v > len + 1e-6) break;
        float x = xOf(v);
        c.Line({x, base - 5}, {x, base}, t.textLow, 1, false);
        if (v + step / 2 < len) c.Line({xOf(v + step / 2), base - 2}, {xOf(v + step / 2), base}, t.textLow.Alpha(0.6f), 1, false);
        std::wstring label = Clock(v, step < 1);
        float lw = c.Measure(label, Weight::Regular, 10.5f) + kTipSlack;
        if (x + lw / 2 > bar.right + kHandleW) continue; // the last one would hang off the end
        D2D1_RECT_F lr = i == 0 ? Rect(x, base - kRulerH, lw, 12) : Rect(x - lw / 2, base - kRulerH, lw, 12);
        c.Text(label, lr, Weight::Regular, 10.5f, t.textLow, i == 0 ? Align::Left : Align::Center);
    }
}

void ClipStudio::Timeline(const Theme& t, const D2D1_RECT_F& bar) {
    Canvas& c = *canvas;
    float span = Width(bar);
    auto xOf = [&](double s) { return bar.left + (float)(len > 0 ? s / len : 0) * span; };
    auto sOf = [&](float x) { return len > 0 ? (std::clamp)((double)(x - bar.left) / span * len, 0.0, len) : 0.0; };
    Ruler(t, bar);
    c.Fill(bar, kStripR, t.track);
    if (!strip.empty()) { // the clip along its length, so a moment can be found by eye
        c.PushClip(bar);
        float sw = span / strip.size();
        for (size_t i = 0; i < strip.size(); ++i)
            if (strip[i]) c.Image(strip[i].Get(), Rect(bar.left + i * sw, bar.top, sw + 0.5f, Height(bar)), 0);
        c.PopClip();
    }
    float xa = xOf(cut.from), xb = xOf(cut.to), h = Height(bar);
    if (xa > bar.left) c.Fill(Rect(bar.left, bar.top, xa - bar.left, h), kStripR, Color{0, 0, 0, kDim}); // what's cut away, dimmed
    if (xb < bar.right) c.Fill(Rect(xb, bar.top, bar.right - xb, h), kStripR, Color{0, 0, 0, kDim});
    int here = clipedit::PieceAt(cut, at);
    for (size_t i = 0; i < cut.pieces.size(); ++i) { // the parts: removed ones crossed out, sped or slowed ones marked
        const clipedit::Piece& pc = cut.pieces[i];
        float pa = (std::max)(xOf(pc.from), bar.left), pb = (std::min)(xOf(pc.to), bar.right);
        if (pb - pa < 1) continue;
        D2D1_RECT_F pr = Rect(pa, bar.top, pb - pa, h);
        if (pc.cut) {
            c.Fill(pr, 0, Color{0, 0, 0, kDim});
            c.PushClip(pr);
            for (float hx = pa - h; hx < pb; hx += 9) c.Line({hx, bar.bottom}, {hx + h, bar.top}, sheet::kDanger.Alpha(0.45f), 1.5f, false); // hatched: left out
            c.PopClip();
            if (pb - pa > 64) c.Text(L"Removed", pr, Weight::Semi, 11, Color{1, 1, 1, 0.9f}, Align::Center);
        }
        if (pc.speed != 1 && !pc.cut && pb - pa > 30) {
            wchar_t sp[16];
            swprintf_s(sp, pc.speed < 1 ? L"%.2g×" : L"%.3g×", pc.speed);
            float tw = c.Measure(sp, Weight::Semi, 10.5f) + 12;
            D2D1_RECT_F badge = Rect(pa + 5, bar.top + 5, tw, 18);
            c.Fill(badge, 9, Color{0, 0, 0, 0.7f});
            c.Text(sp, badge, Weight::Semi, 10.5f, t.accentHi, Align::Center);
        }
        if (i > 0) c.Fill(Rect(pa - 1, bar.top, 2, h), 0, Color{1, 1, 1, 0.9f}); // where it was split
        if ((int)i == here && cut.pieces.size() > 1) c.Stroke(Inset(pr, 1, 1), 3, t.accentHi.Alpha(0.9f), 1.5f); // the part the speed panel changes
    }
    if (cut.moving) { // the moving frame's spots: a click jumps to one
        for (size_t i = 0; i < cut.spots.size(); ++i) {
            float sx = xOf(cut.spots[i].at);
            if (sx < bar.left - 1 || sx > bar.right + 1) continue;
            D2D1_POINT_2F sc = {sx, bar.bottom - 10};
            bool on = clipedit::SpotNear(cut, at) == (int)i;
            c.Dot(sc, 6, Color{0, 0, 0, 0.6f});
            c.Dot(sc, 4.5f, on ? t.accentHi : Color{1, 1, 1, 0.95f});
            D2D1_RECT_F sh = Rect(sx - 8, sc.y - 8, 16, 16);
            Tip(sh, L"A frame spot: click to go to it");
            if (input.pressed && Inside(sh, input.mouse) && !dragging) Seek(cut.spots[i].at), input.pressed = false; // taken: not a scrub
        }
    }
    if (songPcm && !beats.empty()) { // the song's beats, where they fall on the clip
        double outLen = clipedit::OutLength(cut);
        for (double b : beats) {
            double o = cut.songAt + b;
            if (o >= outLen) break;
            float bx = xOf(clipedit::ToSrc(cut, o));
            c.Fill(Rect(bx - 0.75f, bar.bottom - 9, 1.5f, 9), 0, t.accentHi.Alpha(0.85f));
        }
    }
    D2D1_RECT_F ha = Rect(xa - kHandleW, bar.top - kFrameT, kHandleW, h + 2 * kFrameT), hb = Rect(xb, bar.top - kFrameT, kHandleW, h + 2 * kFrameT);
    D2D1_POINT_2F m = input.mouse;
    D2D1_RECT_F hit = Rect(bar.left - kHandleW, bar.top - kRulerH, span + 2 * kHandleW, h + kRulerH + kFrameT);
    bool overA = Inside(Inset(ha, -6, 0), m), overB = !overA && Inside(Inset(hb, -6, 0), m);
    float litA = ui.Smooth(Ui::Id("cs.ha"), overA || dragging == 1 ? 1.0f : 0, w::kRowFade), litB = ui.Smooth(Ui::Id("cs.hb"), overB || dragging == 2 ? 1.0f : 0, w::kRowFade);
    c.Fill(Rect(xa, bar.top - kFrameT, xb - xa, kFrameT), 1, t.accent); // what's kept, framed above and below as the grips
    c.Fill(Rect(xa, bar.bottom, xb - xa, kFrameT), 1, t.accent);
    for (int i = 0; i < 2; ++i) { // the grips, each with its arrow pointing the way it trims
        const D2D1_RECT_F& g = i ? hb : ha;
        c.Fill(g, kHandleR, t.accent.Mix(t.accentHi, i ? litB : litA));
        Head(c, {(g.left + g.right) / 2 + (i ? 2.0f : -2.0f), (g.top + g.bottom) / 2}, i ? 1.0f : -1.0f, 4.5f, t.onAccent);
    }
    if (Edited() || dragging == 1 || dragging == 2) { // the ends' times, under them
        float ly = bar.bottom + kFrameT + 3;
        std::wstring fa = Clock(cut.from, true), fb = Clock(cut.to, true);
        float wa = c.Measure(fa, Weight::Medium, 11) + kTipSlack, wb = c.Measure(fb, Weight::Medium, 11) + kTipSlack;
        float la = (std::max)(xa - kHandleW / 2 - wa / 2, bar.left - kHandleW), lb = (std::min)(xb + kHandleW / 2 - wb / 2, bar.right + kHandleW - wb);
        if (lb < la + wa + 6) lb = la + wa + 6; // close together: side by side, not over each other
        c.Text(fa, Rect(la, ly, wa, 14), Weight::Medium, 11, t.accentHi);
        c.Text(fb, Rect(lb, ly, wb, 14), Weight::Medium, 11, t.accentHi);
    }
    float xp = xOf(at);
    c.Fill(Rect(xp - kHeadW / 2, bar.top - 6, kHeadW, h + 12), 1, t.text);
    c.Dot({xp, bar.top - 6}, kKnobR, t.text);
    ui.Button(Ui::Id("cs.bar"), hit);
    ui.Name(L"Timeline");
    bool over = Inside(hit, m);
    Tip(Inset(ha, -6, 0), overA ? L"Drag to set where the clip starts  ·  I sets it at the playhead" : nullptr);
    Tip(Inset(hb, -6, 0), overB ? L"Drag to set where the clip ends  ·  O sets it at the playhead" : nullptr);
    if (input.pressed && over) {
        dragging = overA ? 1 : overB ? 2 : 3;
        if (dragging == 3 && engine && !engine->IsPaused()) engine->Pause(); // scrubbing holds the clip still
    }
    if (dragging && !input.down) {
        if (dragging != 3) Commit(); // the handle let go: a step undo can go back to
        dragging = 0;
    }
    if (dragging && len > 0) {
        double s = sOf(m.x);
        if (dragging == 1) cut.from = (std::min)(s, cut.to - kMinKeep), Seek(cut.from);
        else if (dragging == 2) cut.to = (std::max)(s, cut.from + kMinKeep), Seek(cut.to);
        else if (std::fabs(s - at) > 0.02) Seek(s);
    }
    float bubbleY = bar.top - kRulerH - kBubbleH + 6;
    if (dragging == 3) {
        Bubble(t, xp, bubbleY, Clock(at, true));
    } else if (!dragging && over && !overA && !overB && len > 0) { // where a click would land, and when that is
        float gx = (std::clamp)(m.x, bar.left, bar.right);
        c.Fill(Rect(gx - 0.5f, bar.top, 1, h), 0, t.text.Alpha(0.55f));
        Bubble(t, gx, bubbleY, Clock(sOf(gx), true));
    }
}

void ClipStudio::DrawBar(const Theme& t, float a) {
    Canvas& c = *canvas;
    float bw = (std::min)(kBarW, W - 2 * kSide), bh = kBarH;
    float rise = (1 - a) * 40; // comes up after the room, as the long pill's editor does
    D2D1_RECT_F bar = Rect((W - bw) / 2, H - kBarBottom - bh + rise, bw, bh);
    barRect = bar;
    c.Fill(Rect(bar.left, bar.top + 4, bw, bh), kBarR, Color{0, 0, 0, 0.24f * a});
    c.Fill(bar, kBarR, t.cardHi.Alpha(a));
    c.StrokeV(bar, kBarR, t.hairTop.Alpha(a), t.hairBot.Alpha(a), 1);
    float x = bar.left + kBarPad, right = bar.right - kBarPad, y = bar.top + kBarPad, cy = y + kRowH / 2;
    // the transport: back five, play, on five; where it is; what's kept
    bool ready = len > 0;
    if (IconTool(t, "cs.back5", {x + kIconBtn / 2, cy}, kDrawSkipBack, L"Back 5 seconds  ·  Shift+←", ready)) Seek(at - kNudgeBig);
    D2D1_POINT_2F pc = {x + kIconBtn + 6 + kPlayBtn / 2, cy};
    if (w::PlayBtn(ui, t, Ui::Id("cs.play"), pc, kPlayBtn, playing, ready)) Toggle();
    Tip(Rect(pc.x - kPlayBtn / 2, pc.y - kPlayBtn / 2, kPlayBtn, kPlayBtn), playing ? L"Pause  ·  Space" : L"Play  ·  Space");
    if (IconTool(t, "cs.fwd5", {pc.x + kPlayBtn / 2 + 6 + kIconBtn / 2, cy}, kDrawSkipFwd, L"On 5 seconds  ·  Shift+→", ready)) Seek(at + kNudgeBig);
    float cx = pc.x + kPlayBtn / 2 + 12 + kIconBtn + 12;
    D2D1_RECT_F code = Rect(cx, cy - kCodeH / 2, kCodeW, kCodeH);
    c.Fill(code, kCodeH / 2, t.track);
    std::wstring now = Clock(at, true), of = L"  /  " + Clock(len, false);
    float nw = c.Measure(now, Weight::Semi, 14) + kTipSlack, ow = c.Measure(of, Weight::Regular, 12.5f) + kTipSlack;
    float tx = code.left + (kCodeW - nw - ow) / 2;
    c.Text(now, Rect(tx, code.top, nw, kCodeH), Weight::Semi, 14, t.text);
    c.Text(of, Rect(tx + nw, code.top, ow, kCodeH), Weight::Regular, 12.5f, t.textDim);
    if (IconTool(t, "cs.keys", {right - kIconBtn / 2, cy}, glyph::Keys, L"Space plays  ·  ← → step  ·  I and O set the start and end  ·  S splits  ·  Delete removes a part  ·  Ctrl+Z undoes  ·  Ctrl+S saves", true))
        Say(L"Space plays  ·  ← → step a second, with Shift five  ·  I and O set the start and end  ·  S splits  ·  Delete removes a part  ·  P plays from the start  ·  Esc leaves");
    D2D1_RECT_F sum = Rect(code.right + 16, y, right - kIconBtn - 12 - code.right - 16, kRowH);
    if (Edited()) c.Text(L"Finished clip  " + Clock(clipedit::OutLength(cut), true) + (cut.shape == clipedit::kTall ? L"  ·  9:16" : cut.shape == clipedit::kSquare ? L"  ·  1:1" : L""), sum, Weight::Medium, 13, t.accentHi, Align::Right);
    else c.Text(L"To trim, drag either end of the timeline", sum, Weight::Regular, 12.5f, t.textDim, Align::Right);
    y += kRowH + kRowGap + kRulerH;
    Timeline(t, Rect(x + kHandleW, y, right - x - 2 * kHandleW, kTimelineH));
    y += kTimelineH + kLabelH + kRowGap;
    cy = y + kToolH / 2;
    // the trim's tools on the left; what to do with the clip on the right, saving last
    float pw = c.Measure(L"Split", Weight::Medium, 12) + kToolPad;
    if (Tool(t, "cs.split", Rect(x, y, pw, kToolH), L"Split", L"Cuts the timeline in two at the playhead, so each part can have its own speed or be removed  ·  S", false, len > 0)) SplitHere();
    float ix = x + pw + 12 + kIconBtn / 2;
    if (IconTool(t, "cs.undo", {ix, cy}, kDrawUndo, L"Undo  ·  Ctrl+Z", stepAt > 0)) Undo();
    if (IconTool(t, "cs.redo", {ix + kIconBtn + 2, cy}, kDrawRedo, L"Redo  ·  Ctrl+Y", stepAt + 1 < steps.size())) Redo();
    if (IconTool(t, "cs.revert", {ix + 2 * (kIconBtn + 2), cy}, glyph::Restart, L"Back to the whole clip", Edited())) Revert();
    float rx = right;
    auto rightTool = [&](const char* id, const wchar_t* label, const wchar_t* tip, bool primary, bool live) {
        float w = c.Measure(label, Weight::Medium, 12) + (primary ? 2 * kToolPad : kToolPad);
        rx -= w;
        bool hit = Tool(t, id, Rect(rx, y, w, kToolH), label, tip, primary, live);
        rx -= kToolGap;
        return hit;
    };
    if (rightTool("cs.save", L"Save", L"Keeps only the trimmed part in this clip. The original goes to the Recycle Bin  ·  Ctrl+S", true, Edited())) SaveOver();
    if (rightTool("cs.copy2", L"Save as a copy", L"Saves the trimmed part as a new clip and leaves this one as it is  ·  Ctrl+Shift+S", false, Edited())) SaveCopy();
    rx -= 10; // saving apart from sharing
    c.Fill(Rect(rx, cy - 10, 1, 20), 0, t.hairTop);
    rx -= 10 + kToolGap;
    if (rightTool("cs.small", L"Copy for Discord", L"A copy under Discord's 10 MB limit, on the clipboard to paste. Your clip stays as it is", false, true)) ShrinkCopy();
    if (rightTool("cs.copy", L"Copy", L"The clip on the clipboard as it is: paste it into a folder, or a chat that takes big files", false, true))
        Say(clips::Copy(hwnd, path) ? L"Copied. Paste it into a chat or a folder" : L"It couldn't be copied");
    D2D1_POINT_2F bc = {rx - kIconBtn / 2, cy};
    D2D1_RECT_F bin = Rect(bc.x - kIconBtn / 2, bc.y - kIconBtn / 2, kIconBtn, kIconBtn);
    float prog = 0;
    bool fire = !busy && ui.Hold(Ui::Id("cs.bin"), bin, kHoldSec, prog);
    bool over = Inside(bin, input.mouse);
    float lit = ui.Smooth(Ui::Id("cs.bin", 1), over ? 1.0f : 0, w::kRowFade);
    if (lit > 0.01f) c.Dot(bc, kIconBtn / 2, sheet::kDanger.Alpha(0.14f * lit));
    c.Icon(glyph::Delete, bc, kIconPx, t.textDim.Mix(sheet::kDanger, lit));
    if (prog > 0) c.Arc(bc, kIconBtn / 2 - 1, -kPi / 2, prog * 2 * kPi, sheet::kDanger, 2.5f); // fills round as it's held
    ui.Name(L"Hold to move this clip to the recycle bin");
    Tip(bin, L"Hold to delete. It goes to the Recycle Bin");
    if (input.pressed && over) binHeld = true, binFired = false;
    if (fire) binFired = true, Bin();
    if (input.released && binHeld) {
        binHeld = false;
        if (!binFired && over) Say(L"Hold the bin a moment to delete the clip");
    }
}

void ClipStudio::DrawTip(const Theme& t, double now) {
    if (tipNow != tipWas) tipWas = tipNow, tipSince = now;
    bool show = tipNow && now - tipSince >= kTipDelay;
    if (show) tipText = tipNow, tipAt = tipRect;
    if (tipNow && !show) ui.Wake(); // the moment runs out with the mouse still
    float a = ui.Smooth(Ui::Id("cs.tip"), show ? 1.0f : 0, w::kPopFade);
    if (a < 0.01f || tipText.empty()) return;
    Canvas& c = *canvas;
    float w = (std::min)(c.Measure(tipText, Weight::Regular, 12) + kTipSlack + 2 * kTipPad, W - 24);
    float y = tipAt.top - kTipGap - kTipH;
    if (y < 8) y = tipAt.bottom + kTipGap; // up against the top: under it instead
    D2D1_RECT_F r = Rect((std::clamp)((tipAt.left + tipAt.right - w) / 2, 12.0f, W - 12 - w), y + (1 - a) * 4, w, kTipH);
    w::Pop(c, t, r, kTipH / 2, a);
    c.Text(tipText, Inset(r, kTipPad, 0), Weight::Regular, 12, t.text.Alpha(a), Align::Center);
}

void ClipStudio::DrawToast(const Theme& t) {
    bool saying = !said.empty() && GetTickCount64() - saidAt < kSaidMs;
    int pct = work->pct;
    if (busy && pct >= 0) said = L"Making the clip…  " + std::to_wstring(pct) + L"%", saidAt = GetTickCount64(), saying = true; // stays up while it's made
    if (saying) toast = said;
    float a = ui.Smooth(Ui::Id("cs.toast"), saying ? 1.0f : 0, w::kPopFade);
    if (a < 0.01f || toast.empty()) return;
    Canvas& c = *canvas;
    float w = (std::min)(c.Measure(toast, Weight::Medium, 12.5f) + kTipSlack + 2 * kToastPad, W - 2 * kSide);
    D2D1_RECT_F r = Rect((W - w) / 2, barRect.top - kToastGap - kToastH + (1 - a) * 6, w, kToastH);
    w::Pop(c, t, r, kToastH / 2, a);
    c.Text(toast, Inset(r, kToastPad, 0), Weight::Medium, 12.5f, t.text.Alpha(a), Align::Center);
}

void ClipStudio::Frame(const Theme& t) {
    if (!ctx) {
        done = true;
        return;
    }
    double now = Seconds();
    if (leaving) { // the compositor plays the exit; the last frame stays as it was
        if (now - leaveAt >= kLeaveSec) done = true;
        return;
    }
    float dt = lastFrame == 0 ? 1 / 60.0f : (float)(std::min)(now - lastFrame, 0.05);
    lastFrame = now;
    {
        std::lock_guard<std::mutex> lock(work->mu);
        for (auto& [slot, pic] : work->strip) { // the timeline's pictures as they come, made into bitmaps here on the ui thread
            if (slot >= strip.size() || pic.px.empty()) continue;
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ComPtr<ID2D1Bitmap> bmp;
            if (SUCCEEDED(ctx->CreateBitmap({pic.w, pic.h}, pic.px.data(), pic.w * 4, bp, &bmp))) strip[slot] = bmp;
        }
        work->strip.clear();
    }
    TakeFrame();
    if (engine) {
        if (!dragging) at = engine->GetCurrentTime();
        playing = !engine->IsPaused() && !engine->IsEnded();
        if (playing && !dragging && Edited()) { // it plays as it'll be saved: past removed parts, each part at its speed, stopping at the end
            if (at >= cut.to - 0.02) {
                engine->Pause();
                playing = false;
                Seek(cut.to);
            } else if (!clipedit::Kept(cut, at)) {
                double next = clipedit::NextKept(cut, at);
                if (next >= cut.to - 0.02) engine->Pause(), playing = false;
                Seek(next);
            }
        }
        int pi = clipedit::PieceAt(cut, at);
        float rate = pi >= 0 && Edited() ? cut.pieces[(size_t)pi].speed : 1.0f;
        if (rate != rateNow && SUCCEEDED(engine->SetPlaybackRate(rate))) rateNow = rate;
    }
    if (cut.song != songFor) { // the edit's song changed, or undo brought one back: its sound and beats
        songFor = cut.song;
        auto it = songs.find(cut.song);
        songPcm = it == songs.end() ? nullptr : it->second;
        auto bt = songBeats.find(cut.song);
        beats = bt == songBeats.end() ? std::vector<double>{} : bt->second;
    }
    if (mixDue > 0 && now >= mixDue && !mixBusy) MixNow();
    SyncMix();
    tipNow = nullptr;
    canvas->NewFrame();
    bool hear = access.Listening();
    canvas->Listen(hear);
    ui.Listen(hear);
    ui.Keyboard(model.a11yKeys != 0);
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ui.Begin(*canvas, input, dt, now);
    float a = anim::EaseOut(ui.SmoothFrom(Ui::Id("cs.in"), 1, 0, kIntroRate)); // the room fades up, the bar rises into place
    canvas->Fill({0, 0, W, H}, 0, t.bg);
    canvas->Fill({0, 0, W, H * 0.6f}, 0, t.accent.Alpha(0.035f)); // a breath of the theme's colour over the top of the room
    float stageBottom = H - kBarBottom - kBarH - 28;
    D2D1_RECT_F panel = {W - kSide - kPanelW, kTopH + 16, W - kSide, stageBottom};
    D2D1_RECT_F stage = {kSide, kTopH + 16, panel.left - kPanelGap, stageBottom};
    DrawVideo(t, stage, a);
    DrawTop(t, a);
    DrawPanel(t, panel, a);
    DrawBar(t, a);
    DrawToast(t);
    DrawTip(t, now); // last: over everything
    ui.DrawFocus(t.accentHi, t.onAccent);
    ui.End();
    HRESULT hr = ctx->EndDraw();
    if (hear) access.Publish(ui, *canvas, scale);
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    if (poll > 0) --poll;
    bool saying = !said.empty() && GetTickCount64() - saidAt < kSaidMs;
    dirty = ui.Busy() || ui.Captured() || dragging || frameKind || poll > 0 || naming || saying || busy || mixDue > 0 || (take && take->Live()); // otherwise it sleeps until a key, the mouse or the engine wakes it
    if (FAILED(hr) || FAILED(swap->Present(1, 0))) done = true; // device lost: leave, the clip is safe on disk
}
