// What lands on the shelf: drops onto the island, and, only while the setting is on, whatever is copied anywhere.
// The clipboard is heard about from windows, never polled, and content marked private or our own copies are left alone.
#include "shelf_impl.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <wrl/client.h>

#include <algorithm>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace sys {

namespace {

constexpr size_t kDropMax = 100; // files from one drop: ten stacks of ten
constexpr UINT kClipDelayMs = 120; // the copying app finishes writing every format first
constexpr UINT kClipRetryMs = 60;
constexpr int kClipTries = 5; // another app holds the clipboard open: a few short tries, never a wait on the ui thread
constexpr wchar_t kClass[] = L"AstraeusShelf";

FORMATETC Fmt(CLIPFORMAT cf, LONG index = -1, DWORD tymed = TYMED_HGLOBAL) { return {cf, nullptr, DVASPECT_CONTENT, index, tymed}; }

CLIPFORMAT Reg(const wchar_t* name) { return (CLIPFORMAT)RegisterClipboardFormatW(name); }

struct Formats { // registered once, the first time they're needed
    CLIPFORMAT png, url, group, contents, exclude, history, cloud, ignore;
    static const Formats& Get() {
        static const Formats f{Reg(L"PNG"), Reg(CFSTR_INETURLW), Reg(CFSTR_FILEDESCRIPTORW), Reg(CFSTR_FILECONTENTS),
                               Reg(L"ExcludeClipboardContentFromMonitorProcessing"), Reg(L"CanIncludeInClipboardHistory"),
                               Reg(L"CanUploadToCloudClipboard"), Reg(L"Clipboard Viewer Ignore")};
        return f;
    }
};

bool Has(IDataObject* d, CLIPFORMAT cf) {
    FORMATETC fe = Fmt(cf);
    return cf && d->QueryGetData(&fe) == S_OK;
}

bool Bytes(IDataObject* d, CLIPFORMAT cf, LONG index, std::string& out, size_t max, bool cut = false) { // a format's bytes, from memory or a stream; cut: a longer one keeps its start
    FORMATETC fe = Fmt(cf, index, TYMED_HGLOBAL | TYMED_ISTREAM);
    STGMEDIUM m{};
    if (!cf || FAILED(d->GetData(&fe, &m))) return false;
    bool ok = false;
    if (m.tymed == TYMED_HGLOBAL && m.hGlobal) {
        size_t n = GlobalSize(m.hGlobal);
        if (cut) n = (std::min)(n, max);
        if (const void* p = n && n <= max ? GlobalLock(m.hGlobal) : nullptr) {
            out.assign((const char*)p, n);
            GlobalUnlock(m.hGlobal);
            ok = true;
        }
    } else if (m.tymed == TYMED_ISTREAM && m.pstm) {
        char buf[64 * 1024];
        ULONG got = 0;
        out.clear();
        while (SUCCEEDED(m.pstm->Read(buf, sizeof(buf), &got)) && got > 0 && out.size() + got <= max) out.append(buf, got);
        ok = !out.empty() && got == 0; // read to the end, not cut off at the cap
    }
    ReleaseStgMedium(&m);
    return ok;
}

bool Flag(IDataObject* d, CLIPFORMAT cf, DWORD& v) {
    std::string b;
    if (!Bytes(d, cf, -1, b, 64) || b.size() < sizeof(DWORD)) return false;
    memcpy(&v, b.data(), sizeof(v));
    return true;
}

bool Private(IDataObject* d) { // the copying app asked for it not to be kept: password fields, managers, private windows
    const Formats& f = Formats::Get();
    DWORD v = 1;
    return Has(d, f.exclude) || Has(d, f.ignore) || (Flag(d, f.history, v) && v == 0) || (Flag(d, f.cloud, v) && v == 0);
}

std::wstring Stamp() { // an image that came as bytes gets a name of when it came
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[48];
    swprintf_s(buf, L"Image %04u-%02u-%02u %02u%02u%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

void Read(IDataObject* d, Shelf::Haul& h) { // files win, then pictures sent as files, then text, then a bare bitmap
    const Formats& f = Formats::Get();
    FORMATETC drop = Fmt(CF_HDROP);
    STGMEDIUM m{};
    if (SUCCEEDED(d->GetData(&drop, &m))) {
        if (auto hd = static_cast<HDROP>(GlobalLock(m.hGlobal))) {
            UINT n = (std::min)(DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0), (UINT)kDropMax);
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(hd, i, nullptr, 0);
                std::wstring path(len, L'\0');
                if (len && DragQueryFileW(hd, i, path.data(), len + 1) == len) h.files.push_back(std::move(path));
            }
            GlobalUnlock(m.hGlobal);
        }
        ReleaseStgMedium(&m);
        if (!h.files.empty()) return;
    }
    std::string group;
    if (Bytes(d, f.group, -1, group, 1 << 20) && group.size() >= sizeof(FILEGROUPDESCRIPTORW)) { // a picture dragged out of a web page
        const auto* g = reinterpret_cast<const FILEGROUPDESCRIPTORW*>(group.data());
        UINT n = (std::min)(g->cItems, (UINT)((group.size() - sizeof(UINT)) / sizeof(FILEDESCRIPTORW))); // never past what was really sent
        for (UINT i = 0; i < n && h.images.size() < Shelf::kStackMax; ++i) {
            const FILEDESCRIPTORW& fd = g->fgd[i];
            if ((fd.dwFlags & FD_ATTRIBUTES) && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!shelfkind::IsImageName(fd.cFileName)) continue; // only pictures come in as bytes: other files stay where they are
            Shelf::Work::Img img{fd.cFileName, {}, false};
            if (Bytes(d, f.contents, (LONG)i, img.bytes, shelfio::kImageMax)) h.images.push_back(std::move(img));
        }
        if (!h.images.empty()) return;
    }
    std::string text;
    constexpr size_t kTextBytes = shelfio::kTextMax * sizeof(wchar_t);
    if (Bytes(d, CF_UNICODETEXT, -1, text, kTextBytes, true) || Bytes(d, f.url, -1, text, kTextBytes, true)) {
        const auto* p = reinterpret_cast<const wchar_t*>(text.data());
        h.text.assign(p, wcsnlen(p, text.size() / sizeof(wchar_t)));
        SecureZeroMemory(text.data(), text.size());
        if (h.text.find_first_not_of(L" \t\r\n") != std::wstring::npos) return;
        h.text.clear();
    }
    Shelf::Work::Img img{Stamp(), {}, false};
    if (!Bytes(d, f.png, -1, img.bytes, shelfio::kImageMax)) {
        img.dib = Bytes(d, CF_DIBV5, -1, img.bytes, shelfio::kImageMax) || Bytes(d, CF_DIB, -1, img.bytes, shelfio::kImageMax); // made into a png by the worker
        if (!img.dib) return;
    }
    h.images.push_back(std::move(img));
}

}

bool Shelf::Accepts(IDataObject* d) {
    if (!d) return false;
    const Formats& f = Formats::Get();
    return Has(d, CF_HDROP) || Has(d, CF_UNICODETEXT) || Has(d, f.url) || Has(d, f.group) || Has(d, f.png) || Has(d, CF_DIB);
}

bool Shelf::Add(IDataObject* d) {
    if (!d) return false;
    Haul h;
    Read(d, h);
    return Land(h);
}

bool Shelf::AddClip() { // pasted on purpose: the shelf's own copy or a private one lands too
    ComPtr<IDataObject> d;
    if (FAILED(OleGetClipboard(&d)) || !d || !Accepts(d.Get())) return false;
    Haul h;
    Read(d.Get(), h);
    d.Reset(); // the clipboard is let go before anything is written
    return Land(h);
}

void Shelf::OnClip() {
    clipTries = 0;
    if (clipWnd) SetTimer(clipWnd, shelfio::kClipTimer, kClipDelayMs, nullptr); // a burst of notices is one read
}

bool Shelf::OwnClip() { return ownSeq && GetClipboardSequenceNumber() == ownSeq; }

void Shelf::MarkOwn() { ownSeq = GetClipboardSequenceNumber(); }

void Shelf::ReadClip() {
    if (!listening || OwnClip()) return; // switched off meanwhile, or it's what the shelf just copied: never read back as new
    ComPtr<IDataObject> d;
    if (FAILED(OleGetClipboard(&d)) || !d) {
        if (++clipTries < kClipTries) SetTimer(clipWnd, shelfio::kClipTimer, kClipRetryMs, nullptr);
        return;
    }
    if (Private(d.Get())) return;
    Haul h;
    Read(d.Get(), h);
    d.Reset(); // the clipboard is let go before anything is written
    if (Land(h) && notify) PostMessageW(notify, msg, 0, 0);
}

LRESULT CALLBACK Shelf::ClipProc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* self = reinterpret_cast<Shelf*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, m, wp, lp);
    if (m == WM_CLIPBOARDUPDATE) {
        self->OnClip();
        return 0;
    }
    if (m == WM_TIMER) {
        KillTimer(hwnd, wp); // all one-shot
        if (wp == shelfio::kClipTimer) self->ReadClip();
        else if (wp == shelfio::kSaveTimer && self->saveDue) self->Save();
        else if (wp == shelfio::kAgeTimer) self->Age(), self->ScheduleAge();
        return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
}

void Shelf::Window() {
    if (clipWnd) return;
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ClipProc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        registered = RegisterClassW(&wc) != 0;
    }
    clipWnd = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, this); // message-only: never seen, never focused
    if (!clipWnd) OutputDebugStringW(L"[shelf] no message window: saves are immediate, no clipboard capture\n");
}

}
