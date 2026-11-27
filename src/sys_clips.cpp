// The clips on disk. Listing reads the folders and windows' own idea of each file's length; pictures come from windows'
// thumbnails, or from decoding the frame wanted when scrubbing; a trim copies the kept stretch as it is, without encoding it
// again, so it's instant and loses nothing. Deleting sends to the recycle bin.
#include "sys_clips.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

namespace sys::clips {
namespace {

constexpr LONGLONG kSec = 10000000;
constexpr int64_t kFileTimeToUnix = 11644473600LL; // seconds from 1601 to 1970
constexpr size_t kNameMax = 120;
constexpr DWORD kSettleMs = 400;
constexpr double kShrinkFill = 0.9; // of the limit the bitrate aims for: the container and a busy scene need room
constexpr UINT kShrinkAudio = 12000; // aac at 96 kbit/s
constexpr UINT kShrinkMinVideo = 250000, kShrinkMaxVideo = 8000000;
constexpr UINT kTall1080 = 3500000, kTall720 = 1400000; // bitrates that still look good at those heights; below, 480 // a burst of changes, a clip being written or a folder emptied, counts once

void StartMf() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

int64_t Unix(const FILETIME& ft) {
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return (int64_t)(u.QuadPart / kSec) - kFileTimeToUnix;
}

double ShellLength(const std::wstring& path) { // windows keeps it in the file's properties: no decoding
    ComPtr<IShellItem2> item;
    ULONGLONG d = 0;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || FAILED(item->GetUInt64(PKEY_Media_Duration, &d))) return 0;
    return (double)d / kSec;
}

void Gather(const std::wstring& dir, const std::wstring& game, std::vector<Clip>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.mp4").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Clip c;
        c.path = dir + L"\\" + fd.cFileName;
        c.game = game;
        c.name = fd.cFileName;
        if (c.name.size() > 4) c.name.resize(c.name.size() - 4);
        c.when = Unix(fd.ftLastWriteTime);
        c.bytes = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out.push_back(std::move(c));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool FromBitmap(HBITMAP bmp, Picture& out) {
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return false;
    UINT w = (UINT)bm.bmWidth, h = (UINT)std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB}; // top-down
    out.px.assign((size_t)w * h, 0);
    HDC dc = GetDC(nullptr);
    bool ok = GetDIBits(dc, bmp, 0, h, out.px.data(), &bi, DIB_RGB_COLORS) == (int)h;
    ReleaseDC(nullptr, dc);
    if (!ok) return false;
    for (uint32_t& p : out.px) p |= 0xFF000000; // a video's picture is opaque, whatever the alpha says
    out.w = w, out.h = h;
    return true;
}

}

std::wstring Root() {
    PWSTR videos = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos))) dir = std::wstring(videos) + L"\\Astraeus";
    CoTaskMemFree(videos);
    return dir;
}

std::wstring SafeName(std::wstring s) {
    for (wchar_t& ch : s)
        if (wcschr(L"\\/:*?\"<>|", ch) || ch < 32) ch = L' ';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back(); // windows drops a trailing dot or space anyway
    while (!s.empty() && s.front() == L' ') s.erase(s.begin());
    if (s.size() > kNameMax) s.resize(kNameMax);
    return s.empty() ? L"Game" : s;
}

std::wstring FolderOf(const std::wstring& game) {
    std::wstring root = Root();
    return root.empty() ? root : root + L"\\" + SafeName(game);
}

std::vector<Clip> List(const std::wstring& game, size_t max) {
    std::vector<Clip> out;
    std::wstring root = Root();
    if (root.empty()) return out;
    if (!game.empty()) {
        Gather(FolderOf(game), SafeName(game), out);
    } else {
        Gather(root, L"", out); // clips kept before they went in folders
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') Gather(root + L"\\" + fd.cFileName, fd.cFileName, out);
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::sort(out.begin(), out.end(), [](const Clip& a, const Clip& b) { return a.when > b.when; });
    if (out.size() > max) out.resize(max);
    for (Clip& c : out) c.secs = ShellLength(c.path);
    return out;
}

bool Thumb(const std::wstring& path, UINT px, Picture& out) {
    ComPtr<IShellItemImageFactory> f;
    HBITMAP bmp = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) || FAILED(f->GetImage({(LONG)px, (LONG)px}, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &bmp))) return false;
    bool ok = FromBitmap(bmp, out);
    DeleteObject(bmp);
    return ok;
}

namespace {

ComPtr<IMFSourceReader> RgbReader(const std::wstring& path, UINT maxW, UINT32& w, UINT32& h) { // decoded and turned into rgb for us, made small by windows when it can
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attr.Get(), &r))) return nullptr;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> native, want, got;
    UINT32 nw = 0, nh = 0;
    if (SUCCEEDED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    bool scaled = false;
    if (nw && nh && nw > maxW) { // scaled as it's converted: far less to copy and read
        UINT32 sw = maxW & ~1u, sh = (UINT32)((double)nh * sw / nw) & ~1u;
        MFSetAttributeSize(want.Get(), MF_MT_FRAME_SIZE, sw, (std::max)(sh, 2u));
        scaled = SUCCEEDED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get()));
        if (!scaled) want->DeleteItem(MF_MT_FRAME_SIZE);
    }
    if (!scaled && FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get()))) return nullptr;
    if (FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got)) || FAILED(MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) return nullptr;
    return r;
}

bool Grab(IMFSourceReader* r, UINT32 w, UINT32 h, double at, UINT maxW, Picture& out);

}

void Frames(const std::wstring& path, const std::vector<double>& at, UINT maxW, const std::function<void(size_t, Picture&&)>& got) {
    StartMf();
    UINT32 w = 0, h = 0;
    ComPtr<IMFSourceReader> r = RgbReader(path, maxW, w, h);
    for (size_t i = 0; i < at.size(); ++i) {
        Picture p;
        if (!r || !Grab(r.Get(), w, h, at[i], maxW, p)) { // a reader that lost its way: a fresh one, once, for this picture
            r = RgbReader(path, maxW, w, h);
            if (!r || !Grab(r.Get(), w, h, at[i], maxW, p)) continue;
        }
        got(i, std::move(p));
    }
}

bool Frame(const std::wstring& path, double at, UINT maxW, Picture& out) {
    StartMf();
    UINT32 w = 0, h = 0;
    ComPtr<IMFSourceReader> r = RgbReader(path, maxW, w, h);
    return r && Grab(r.Get(), w, h, at, maxW, out);
}

namespace {

bool Grab(IMFSourceReader* r, UINT32 w, UINT32 h, double at, UINT maxW, Picture& out) {
    LONGLONG target = (LONGLONG)((std::max)(at, 0.0) * kSec);
    PROPVARIANT pos;
    InitPropVariantFromInt64(target, &pos);
    r->SetCurrentPosition(GUID_NULL, pos); // lands on the keyframe before: decoded forward from there
    PropVariantClear(&pos);
    ComPtr<IMFSample> keep;
    for (int guard = 0; guard < 600; ++guard) { // ten seconds of frames at most
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!s) continue;
        keep = s;
        if (ts >= target) break;
    }
    if (!keep) return false;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(keep->ConvertToContiguousBuffer(&buf))) return false;
    BYTE* data = nullptr;
    LONG pitch = (LONG)w * 4;
    ComPtr<IMF2DBuffer> b2;
    bool locked2d = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch));
    DWORD len = 0;
    if (!locked2d && FAILED(buf->Lock(&data, nullptr, &len))) return false;
    float scale = (std::min)(1.0f, (float)maxW / w);
    out.w = (std::max)(1u, (UINT)(w * scale)), out.h = (std::max)(1u, (UINT)(h * scale));
    out.px.resize((size_t)out.w * out.h);
    for (UINT y = 0; y < out.h; ++y) { // nearest pixel: a scrub preview, not the clip
        const BYTE* row = data + (LONG)((UINT)(y / scale)) * pitch;
        for (UINT x = 0; x < out.w; ++x) {
            uint32_t p;
            std::memcpy(&p, row + (size_t)(UINT)(x / scale) * 4, 4);
            out.px[(size_t)y * out.w + x] = p | 0xFF000000;
        }
    }
    if (locked2d) b2->Unlock2D();
    else buf->Unlock();
    return true;
}

}

double Length(const std::wstring& path) {
    StartMf();
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) return 0;
    PROPVARIANT d;
    PropVariantInit(&d);
    double secs = SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &d)) ? (double)d.uhVal.QuadPart / kSec : 0;
    PropVariantClear(&d);
    return secs;
}

bool Trim(const std::wstring& path, double from, double to, const std::wstring& out, std::wstring& why) {
    StartMf();
    if (to - from < 0.5) return why = L"Too short to keep", false;
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) return why = L"The clip couldn't be opened", false;
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> w;
    if (FAILED(MFCreateSinkWriterFromURL(out.c_str(), nullptr, attr.Get(), &w))) return why = L"The new clip couldn't be written", false;
    DWORD map[8];
    DWORD streams = 0;
    for (DWORD s = 0; s < 8; ++s) { // every stream as it is: the same type in and out, so nothing is encoded again
        ComPtr<IMFMediaType> t;
        if (FAILED(r->GetNativeMediaType(s, 0, &t))) break;
        r->SetStreamSelection(s, TRUE);
        if (FAILED(w->AddStream(t.Get(), &map[s])) || FAILED(w->SetInputMediaType(map[s], t.Get(), nullptr))) return why = L"This clip's format can't be trimmed", false;
        streams = s + 1;
    }
    if (!streams || FAILED(w->BeginWriting())) return why = L"The new clip couldn't be started", false;
    PROPVARIANT pos;
    InitPropVariantFromInt64((LONGLONG)(from * kSec), &pos);
    r->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    LONGLONG end = (LONGLONG)(to * kSec), base = -1;
    bool wrote = false;
    auto anyOn = [&] {
        for (DWORD i = 0; i < streams; ++i) {
            BOOL on = FALSE;
            r->GetStreamSelection(i, &on);
            if (on) return true;
        }
        return false;
    };
    for (;;) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &idx, &flags, &ts, &s))) break;
        bool done = (flags & MF_SOURCE_READERF_ENDOFSTREAM) || (s && ts > end); // this stream has given all it will
        if (done && idx < streams) {
            r->SetStreamSelection(idx, FALSE);
            if (!anyOn()) break;
            continue;
        }
        if (!s || idx >= streams) continue;
        if (base < 0) base = ts; // the first sample, on the keyframe the seek landed on: time zero
        if (ts < base) continue;
        s->SetSampleTime(ts - base);
        if (FAILED(w->WriteSample(map[idx], s.Get()))) return why = L"The disk is full or the clip couldn't be written", false;
        wrote = true;
    }
    if (!wrote || FAILED(w->Finalize())) return why = L"The new clip couldn't be finished", false;
    return true;
}

bool Rename(const std::wstring& path, const std::wstring& name, std::wstring& newPath, std::wstring& why) {
    std::wstring clean = SafeName(name);
    size_t slash = path.find_last_of(L'\\');
    newPath = (slash == std::wstring::npos ? L"" : path.substr(0, slash + 1)) + clean + L".mp4";
    if (newPath == path) return true;
    if (GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) return why = L"A clip already has that name", false;
    if (!MoveFileExW(path.c_str(), newPath.c_str(), 0)) return why = L"It couldn't be renamed: is it open somewhere?", false;
    return true;
}

bool Recycle(const std::wstring& path) {
    std::wstring from = path + L'\0'; // the list ends with a second nul
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool Copy(HWND owner, const std::wstring& path) {
    size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL files = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (!files || !effect) {
        if (files) GlobalFree(files);
        if (effect) GlobalFree(effect);
        return false;
    }
    auto* df = static_cast<DROPFILES*>(GlobalLock(files));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    std::memcpy(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(files);
    *static_cast<DWORD*>(GlobalLock(effect)) = DROPEFFECT_COPY; // pasted, it's copied: the clip stays here
    GlobalUnlock(effect);
    if (!OpenClipboard(owner)) {
        GlobalFree(files), GlobalFree(effect);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_HDROP, files) != nullptr;
    if (!ok) GlobalFree(files);
    if (!SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect)) GlobalFree(effect);
    CloseClipboard();
    return ok;
}

void Watch(HWND notify, UINT msg) {
    static std::once_flag once;
    std::call_once(once, [notify, msg] {
        std::wstring root = Root();
        if (root.empty()) return;
        SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
        std::thread([root, notify, msg] { // asleep in windows until the folder changes: costs nothing meanwhile
            HANDLE h = FindFirstChangeNotificationW(root.c_str(), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
            if (h == INVALID_HANDLE_VALUE) return;
            while (WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0) {
                Sleep(kSettleMs);
                if (!FindNextChangeNotification(h)) break;
                while (WaitForSingleObject(h, 0) == WAIT_OBJECT_0 && FindNextChangeNotification(h)) {
                }
                PostMessageW(notify, msg, 1, 0);
            }
            FindCloseChangeNotification(h);
        }).detach();
    });
}

namespace {

bool ShrinkOnce(const std::wstring& path, const std::wstring& out, double secs, double fill, uint64_t maxBytes, std::wstring& why) {
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 1);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE); // decoded, scaled and turned to nv12 for the encoder
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), ra.Get(), &r))) return why = L"The clip couldn't be opened", false;
    ComPtr<IMFMediaType> native;
    UINT32 w = 0, h = 0, fn = 60, fd = 1;
    if (FAILED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native)) || FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h))) return why = L"The clip has no picture", false;
    MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fn, &fd);
    double budget = (double)maxBytes * 8 * fill / (std::max)(secs, 1.0) - kShrinkAudio * 8.0;
    UINT vbit = (UINT)(std::clamp)(budget, (double)kShrinkMinVideo, (double)kShrinkMaxVideo);
    UINT tall = vbit >= kTall1080 ? 1080u : vbit >= kTall720 ? 720u : 480u;
    if (h > tall) w = (UINT)((double)w * tall / h), h = tall; // fewer pixels for fewer bits: it stays sharp
    w &= ~1u, h &= ~1u;
    ComPtr<IMFMediaType> nv12, pcm, h264, aac, readerPcm, nvIn;
    MFCreateMediaType(&nv12);
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, w, h);
    if (FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get())) || FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &nvIn)))
        return why = L"The clip couldn't be decoded", false;
    MFCreateMediaType(&pcm);
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    bool sound = SUCCEEDED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcm.Get())) &&
                 SUCCEEDED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &readerPcm));
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (sound) r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE); // the first track: the game and the voice together
    ComPtr<IMFAttributes> wa;
    MFCreateAttributes(&wa, 2);
    wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); // the graphics card encodes
    wa->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> sw;
    if (FAILED(MFCreateSinkWriterFromURL(out.c_str(), nullptr, wa.Get(), &sw))) return why = L"The small copy couldn't be written", false;
    MFCreateMediaType(&h264);
    h264->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    h264->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    h264->SetUINT32(MF_MT_AVG_BITRATE, vbit);
    h264->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(h264.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(h264.Get(), MF_MT_FRAME_RATE, fn, fd);
    MFSetAttributeRatio(h264.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD vs = 0, as = 0;
    if (FAILED(sw->AddStream(h264.Get(), &vs)) || FAILED(sw->SetInputMediaType(vs, nvIn.Get(), nullptr))) return why = L"No encoder took the clip", false;
    if (sound) {
        UINT32 rate = MFGetAttributeUINT32(readerPcm.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000), ch = MFGetAttributeUINT32(readerPcm.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
        MFCreateMediaType(&aac);
        aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kShrinkAudio);
        sound = SUCCEEDED(sw->AddStream(aac.Get(), &as)) && SUCCEEDED(sw->SetInputMediaType(as, readerPcm.Get(), nullptr));
        if (!sound) r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
    }
    if (FAILED(sw->BeginWriting())) return why = L"The small copy couldn't be started", false;
    bool videoDone = false, audioDone = !sound;
    while (!videoDone || !audioDone) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &idx, &flags, &ts, &s))) return why = L"The clip couldn't be read", false;
        ComPtr<IMFMediaType> t;
        GUID major{};
        if (SUCCEEDED(r->GetCurrentMediaType(idx, &t))) t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        bool video = major == MFMediaType_Video;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            (video ? videoDone : audioDone) = true;
            continue;
        }
        if (flags & MF_SOURCE_READERF_STREAMTICK) sw->SendStreamTick(video ? vs : as, ts);
        if (!s) continue;
        if (FAILED(sw->WriteSample(video ? vs : as, s.Get()))) return why = L"The small copy couldn't be written", false;
    }
    if (FAILED(sw->Finalize())) return why = L"The small copy couldn't be finished", false;
    return true;
}

}

bool Shrink(const std::wstring& path, uint64_t maxBytes, std::wstring& out, std::wstring& why) {
    StartMf();
    double secs = Length(path);
    if (secs <= 0) return why = L"The clip couldn't be read", false;
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return why = L"There's nowhere to put the small copy", false;
    std::wstring dir = std::wstring(tmp) + L"Astraeus clips";
    CreateDirectoryW(dir.c_str(), nullptr);
    size_t slash = path.find_last_of(L'\\');
    out = dir + L"\\" + path.substr(slash + 1, path.size() - slash - 5) + L" (small).mp4";
    for (double fill = kShrinkFill; fill > 0.4; fill *= 0.8) { // a busy clip that came out too big is made again, smaller
        DeleteFileW(out.c_str());
        if (!ShrinkOnce(path, out, secs, fill, maxBytes, why)) return DeleteFileW(out.c_str()), false;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa) && ((((uint64_t)fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) <= maxBytes) return true;
    }
    DeleteFileW(out.c_str());
    return why = L"It couldn't be made small enough. Trim it shorter and try again", false;
}

void Show(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) return;
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
}

}
