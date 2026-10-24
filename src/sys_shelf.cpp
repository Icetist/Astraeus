#include "shelf.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <mutex>
#include <thread>

#include "sys.h"

using Microsoft::WRL::ComPtr;

namespace sys {

namespace {

constexpr size_t kTextMax = 64 * 1024; // a longer note is cut: it's a stash, not an editor
constexpr size_t kSaveTextMax = 2000; // notes are written to disk this long at most, and only when keeping is on
constexpr size_t kNameMax = 80;
constexpr wchar_t kKeepVal[] = L"ShelfKeep";

std::wstring ShelfFile() { // %LOCALAPPDATA%\Astraeus\shelf.txt
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) path = std::wstring(base) + L"\\Astraeus";
    CoTaskMemFree(base);
    return path.empty() ? path : path + L"\\shelf.txt";
}

std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::wstring Escape(const std::wstring& s) { // one note per line on disk
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') o += L"\\r";
        else if (c == L'\t') o += L"\\t";
        else o += c;
    }
    return o;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 == s.size()) {
            o += s[i];
            continue;
        }
        wchar_t e = s[++i];
        o += e == L'n' ? L'\n' : e == L'r' ? L'\r' : e == L't' ? L'\t' : e;
    }
    return o;
}

bool Thumb(const std::wstring& path, UINT px, UINT& w, UINT& h, std::vector<uint32_t>& out) { // explorer's thumbnail, or the type's icon
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false; // moved or deleted since the drop
    HBITMAP bmp = nullptr;
    DWORD attr = GetFileAttributesW(path.c_str());
    bool remote = PathIsNetworkPathW(path.c_str()) || (attr != INVALID_FILE_ATTRIBUTES && (attr & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)));
    SIIGBF flags = (SIIGBF)(SIIGBF_RESIZETOFIT | (remote ? SIIGBF_INCACHEONLY : 0)); // a share or a cloud placeholder: only what's cached, never a download or a hung server
    if (FAILED(f->GetImage({(LONG)px, (LONG)px}, flags, &bmp)) || !bmp) return false;
    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    bool ok = bm.bmWidth > 0 && bm.bmHeight > 0;
    if (ok) {
        w = (UINT)bm.bmWidth;
        h = (UINT)bm.bmHeight;
        out.assign((size_t)w * h, 0);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB}; // top-down
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, bmp, 0, h, out.data(), &bi, DIB_RGB_COLORS) == (int)h;
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(bmp);
    if (!ok) return false;
    bool alpha = std::any_of(out.begin(), out.end(), [](uint32_t p) { return (p >> 24) != 0; });
    for (uint32_t& p : out) {
        if (!alpha) { // photos come back with no alpha at all: opaque
            p |= 0xFF000000u;
            continue;
        }
        uint32_t a = p >> 24, r = (std::min)((p >> 16) & 0xFF, a), g = (std::min)((p >> 8) & 0xFF, a), b = (std::min)(p & 0xFF, a); // stays a valid premultiplied pixel
        p = a << 24 | r << 16 | g << 8 | b;
    }
    return true;
}

ComPtr<IDataObject> FileData(const std::wstring& path) { // the shell's own data object: file drop, id list, drag image
    ComPtr<IDataObject> obj;
    PIDLIST_ABSOLUTE full = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &full, 0, nullptr)) || !full) return obj; // gone since it was dropped
    PIDLIST_ABSOLUTE parent = ILClone(full);
    PCUITEMID_CHILD child = ILFindLastID(full);
    if (parent && ILRemoveLastID(parent)) SHCreateDataObject(parent, 1, &child, nullptr, IID_PPV_ARGS(&obj));
    ILFree(parent);
    ILFree(full);
    return obj;
}

ComPtr<IDataObject> TextData(const std::wstring& text) {
    ComPtr<IDataObject> obj;
    if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&obj)))) return obj;
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void* p = mem ? GlobalLock(mem) : nullptr;
    if (!p) {
        if (mem) GlobalFree(mem);
        return nullptr;
    }
    memcpy(p, text.c_str(), bytes);
    GlobalUnlock(mem);
    FORMATETC fe = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm = {TYMED_HGLOBAL};
    sm.hGlobal = mem;
    if (FAILED(obj->SetData(&fe, &sm, TRUE))) { // the object owns mem on success
        GlobalFree(mem);
        return nullptr;
    }
    return obj;
}

FORMATETC Fmt(CLIPFORMAT cf) { return {cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }

}

std::wstring ShelfItem::Name() const {
    if (File()) return PathFindFileNameW(path.c_str());
    size_t a = text.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = text.find_first_of(L"\r\n", a);
    std::wstring line = text.substr(a, (std::min)(b == std::wstring::npos ? text.size() - a : b - a, kNameMax));
    return line;
}

struct Shelf::Work {
    struct Done {
        uint64_t id;
        UINT w = 0, h = 0;
        std::vector<uint32_t> px;
    };
    std::mutex lock; // guards everything below
    std::vector<std::pair<uint64_t, std::wstring>> todo;
    std::vector<Done> done;
    bool running = false;
    HWND notify = nullptr;
    UINT msg = 0;
};

Shelf::Shelf() : work(std::make_shared<Work>()) {}

Shelf::~Shelf() {
    std::lock_guard<std::mutex> g(work->lock);
    work->todo.clear(); // a running worker finishes its current file and leaves
    work->notify = nullptr;
}

void Shelf::Start(HWND hwnd, UINT message) {
    notify = hwnd;
    msg = message;
    {
        std::lock_guard<std::mutex> g(work->lock);
        work->notify = hwnd;
        work->msg = message;
    }
    keep = ReadNum(kKeepVal, 0) != 0; // off unless asked for: nothing about what you stash touches the disk
    if (keep) Load();
    Previews();
}

bool Shelf::Accepts(IDataObject* data) {
    if (!data) return false;
    FORMATETC drop = Fmt(CF_HDROP), text = Fmt(CF_UNICODETEXT);
    return data->QueryGetData(&drop) == S_OK || data->QueryGetData(&text) == S_OK;
}

bool Shelf::Add(IDataObject* data) {
    if (!data) return false;
    bool added = false;
    FORMATETC drop = Fmt(CF_HDROP), text = Fmt(CF_UNICODETEXT);
    STGMEDIUM m{};
    if (SUCCEEDED(data->GetData(&drop, &m))) { // files win over text: explorer offers both
        if (auto h = static_cast<HDROP>(GlobalLock(m.hGlobal))) {
            UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = (std::min)(n, (UINT)kMax); i-- > 0;) { // backwards, so the first dropped ends up first
                UINT len = DragQueryFileW(h, i, nullptr, 0);
                std::wstring path(len, L'\0');
                if (!len || DragQueryFileW(h, i, path.data(), len + 1) != len) continue;
                ShelfItem it;
                it.path = std::move(path);
                Push(std::move(it));
                added = true;
            }
            GlobalUnlock(m.hGlobal);
        }
        ReleaseStgMedium(&m);
    } else if (SUCCEEDED(data->GetData(&text, &m))) {
        if (auto p = static_cast<const wchar_t*>(GlobalLock(m.hGlobal))) {
            size_t cap = (std::min)(kTextMax, GlobalSize(m.hGlobal) / sizeof(wchar_t));
            ShelfItem it;
            it.text.assign(p, wcsnlen(p, cap));
            GlobalUnlock(m.hGlobal);
            if (it.text.find_first_not_of(L" \t\r\n") != std::wstring::npos) {
                Push(std::move(it));
                added = true;
            }
        }
        ReleaseStgMedium(&m);
    }
    if (!added) return false;
    Previews();
    if (keep) Save();
    return true;
}

void Shelf::Push(ShelfItem item) {
    auto same = std::find_if(items.begin(), items.end(), [&](const ShelfItem& o) {
        return item.File() ? o.File() && _wcsicmp(o.path.c_str(), item.path.c_str()) == 0 : !o.File() && o.text == item.text;
    });
    if (same != items.end()) { // dropped again: it moves to the front, keeping its preview
        item = std::move(*same);
        items.erase(same);
    } else {
        item.id = nextId++;
        if (item.File()) {
            DWORD a = GetFileAttributesW(item.path.c_str());
            item.dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        }
    }
    items.insert(items.begin(), std::move(item));
    if (items.size() > kMax) items.pop_back(); // the oldest falls off
}

ShelfItem* Shelf::Find(uint64_t id) {
    for (auto& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

void Shelf::Remove(uint64_t id) {
    items.erase(std::remove_if(items.begin(), items.end(), [id](const ShelfItem& o) { return o.id == id; }), items.end());
    {
        std::lock_guard<std::mutex> g(work->lock);
        auto& todo = work->todo; // a preview not made yet isn't needed any more
        todo.erase(std::remove_if(todo.begin(), todo.end(), [id](const auto& j) { return j.first == id; }), todo.end());
    }
    if (keep) Save();
}

void Shelf::Clear() {
    {
        std::lock_guard<std::mutex> g(work->lock);
        work->todo.clear();
    }
    items.clear();
    items.shrink_to_fit(); // previews are the bulk of it
    if (keep) Save();
}

void Shelf::Previews() {
    std::shared_ptr<Work> w = work;
    std::lock_guard<std::mutex> g(w->lock);
    for (auto& it : items)
        if (it.File() && !it.asked) {
            it.asked = true;
            w->todo.emplace_back(it.id, it.path);
        }
    if (w->todo.empty() || w->running) return;
    w->running = true;
    try {
        std::thread([w] {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // thumbnail providers want an sta
            for (;;) {
                std::pair<uint64_t, std::wstring> job;
                {
                    std::lock_guard<std::mutex> g2(w->lock);
                    if (w->todo.empty()) {
                        w->running = false; // nothing left: the thread is gone, no idle worker
                        break;
                    }
                    job = std::move(w->todo.front());
                    w->todo.erase(w->todo.begin());
                }
                Work::Done d{job.first};
                if (!Thumb(job.second, kThumbPx, d.w, d.h, d.px)) continue; // the card keeps its type glyph
                std::lock_guard<std::mutex> g2(w->lock);
                w->done.push_back(std::move(d));
                if (w->notify) PostMessageW(w->notify, w->msg, 0, 0);
            }
            if (SUCCEEDED(hr)) CoUninitialize();
        }).detach();
    } catch (...) {
        w->running = false;
        OutputDebugStringW(L"[shelf] preview thread failed\n");
    }
}

void Shelf::OnPreview() {
    std::vector<Work::Done> done;
    {
        std::lock_guard<std::mutex> g(work->lock);
        done.swap(work->done);
    }
    for (auto& d : done)
        if (ShelfItem* it = Find(d.id)) {
            it->thumbW = d.w;
            it->thumbH = d.h;
            it->thumb = std::move(d.px);
        }
}

void Shelf::SetKeep(bool on) {
    keep = on;
    SaveNum(kKeepVal, on ? 1 : 0);
    if (on) Save();
    else DeleteFileW(ShelfFile().c_str()); // nothing of the shelf stays on disk
}

void Shelf::Save() const {
    std::wstring file = ShelfFile();
    if (file.empty()) return;
    SHCreateDirectoryExW(nullptr, file.substr(0, file.find_last_of(L'\\')).c_str(), nullptr);
    std::wstring body;
    for (const auto& it : items) {
        if (it.File()) body += L"F\t" + it.path + L"\n"; // just where it is
        else body += L"T\t" + Escape(it.text.substr(0, kSaveTextMax)) + L"\n"; // a long note keeps its start, it doesn't vanish
    }
    std::string bytes = Utf8(body);
    std::wstring tmp = file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str()); // a torn write never replaces the last good one
}

void Shelf::Load() {
    HANDLE f = CreateFileW(ShelfFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    std::string bytes;
    constexpr LONGLONG kFileMax = 1 << 20; // ten short entries never come near this
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < kFileMax) {
        bytes.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr)) got = 0;
        bytes.resize(got);
    }
    CloseHandle(f);
    std::wstring all = Wide(bytes);
    for (size_t at = 0; at < all.size() && items.size() < kMax;) {
        size_t end = all.find(L'\n', at);
        if (end == std::wstring::npos) end = all.size();
        std::wstring line = all.substr(at, end - at);
        at = end + 1;
        if (line.size() < 3 || line[1] != L'\t') continue;
        ShelfItem it;
        if (line[0] == L'F') {
            it.path = line.substr(2);
            DWORD a = GetFileAttributesW(it.path.c_str());
            if (a == INVALID_FILE_ATTRIBUTES) {
                std::wstring folder = it.path.substr(0, it.path.find_last_of(L'\\'));
                if (GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES) continue; // deleted since: dropped quietly
                a = 0; // its drive or share isn't there yet at sign-in: kept, or the next save would lose it for good
            }
            it.dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        } else if (line[0] == L'T') {
            it.text = Unescape(line.substr(2));
        } else {
            continue;
        }
        it.id = nextId++;
        items.push_back(std::move(it)); // the file is newest first already
    }
}

bool Shelf::DragOut(uint64_t id, HWND owner) {
    ShelfItem* it = Find(id);
    if (!it) return false;
    ComPtr<IDataObject> obj = it->File() ? FileData(it->path) : TextData(it->text);
    if (!obj) return false;
    DWORD effect = DROPEFFECT_NONE;
    HRESULT hr = SHDoDragDrop(owner, obj.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect); // never a move: the file stays where it is
    return hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
}

bool Shelf::Copy(uint64_t id, HWND owner) {
    ShelfItem* it = Find(id);
    if (!it) return false;
    if (it->File()) { // the file itself, pasteable in explorer
        ComPtr<IDataObject> obj = FileData(it->path);
        return obj && SUCCEEDED(OleSetClipboard(obj.Get())) && SUCCEEDED(OleFlushClipboard()); // flushed: it outlives this object
    }
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (it->text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (void* p = mem ? GlobalLock(mem) : nullptr) {
        memcpy(p, it->text.c_str(), bytes);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr; // the clipboard owns mem now
    }
    if (!ok && mem) GlobalFree(mem);
    CloseClipboard();
    return ok;
}

}
