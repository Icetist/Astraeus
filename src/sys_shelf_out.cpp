// What leaves the shelf: drags (files as the originals, images as image files, text and links as text), copies,
// the colour and link variants, saving images to downloads, and showing a file in its folder.
#include "shelf_impl.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace sys {

namespace {

constexpr int kNameTries = 999; // name (2) .. name (999) before giving up on a free one

ComPtr<IDataObject> FilesData(const std::vector<std::wstring>& paths) { // the shell's own object: file drop, id lists, drag image; any mix of folders
    ComPtr<IDataObject> obj;
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &id, 0, nullptr)) && id) ids.push_back(id); // gone since it was dropped: left out
    }
    ComPtr<IShellItemArray> arr;
    if (!ids.empty() && SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &arr)))
        arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&obj));
    for (auto id : ids) ILFree(id);
    return obj;
}

bool Put(IDataObject* obj, CLIPFORMAT cf, const void* data, size_t bytes) { // one more format on an object
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void* p = mem ? GlobalLock(mem) : nullptr;
    if (!p) {
        if (mem) GlobalFree(mem);
        return false;
    }
    memcpy(p, data, bytes);
    GlobalUnlock(mem);
    FORMATETC fe = {cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm = {TYMED_HGLOBAL};
    sm.hGlobal = mem;
    if (SUCCEEDED(obj->SetData(&fe, &sm, TRUE))) return true; // the object owns mem now
    GlobalFree(mem);
    return false;
}

ComPtr<IDataObject> TextData(const std::wstring& text) {
    ComPtr<IDataObject> obj;
    if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&obj)))) return nullptr;
    return Put(obj.Get(), CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t)) ? obj : nullptr;
}

std::wstring Safe(std::wstring name) { // a file name windows takes
    for (wchar_t& c : name)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    return name.empty() ? L"Image.png" : name;
}

std::wstring Free(const std::wstring& dir, const std::wstring& name) { // name, or name (2), never over something already there
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot), ext = dot == std::wstring::npos ? L"" : name.substr(dot);
    std::wstring path = dir + L"\\" + name;
    for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        if (i > kNameTries) return L"";
        path = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    }
    return path;
}

std::vector<size_t> Which(const ShelfItem& it, const std::vector<size_t>& picks) {
    std::vector<size_t> out;
    for (size_t i = 0; i < it.files.size(); ++i)
        if (picks.empty() || std::find(picks.begin(), picks.end(), i) != picks.end()) out.push_back(i);
    return out;
}

std::wstring Lines(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (const auto& p : v) s += (s.empty() ? L"" : L"\r\n") + p;
    return s;
}

}

std::wstring Shelf::Materialize(const ShelfFile& f) {
    if (!f.owned) return f.path;
    wchar_t sub[24];
    swprintf_s(sub, L"\\%016llx", (unsigned long long)f.hash); // its own folder: the name it had stays the name it drops as
    std::wstring dir = shelfio::TempDir() + sub, path = dir + L"\\" + Safe(f.name);
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    std::string bytes;
    if (!shelfio::LoadImage(f.path, bytes)) return L"";
    bool ok = shelfio::WriteAtomic(path, bytes); // a plain file, as long as this run: swept at the next launch
    SecureZeroMemory(bytes.data(), bytes.size());
    return ok ? path : L"";
}

bool Shelf::CanPlain(const ShelfItem& it) const {
    return !it.HasFiles() || std::any_of(it.files.begin(), it.files.end(), [](const ShelfFile& f) { return !f.owned; });
}

bool Shelf::Copy(uint64_t id, CopyHow how, const std::vector<size_t>& picks) {
    const ShelfItem* it = Find(id);
    if (!it) return false;
    std::wstring text = it->text;
    ComPtr<IDataObject> obj;
    if (how == kHex || how == kRgb || how == kHsl) {
        text = shelfkind::ColorText(it->rgb, how - kHex);
    } else if (how == kClean) {
        text = shelfkind::CleanLink(it->text);
    } else if (it->HasFiles()) {
        std::vector<std::wstring> paths;
        for (size_t i : Which(*it, picks))
            if (how == kCopy || !it->files[i].owned) paths.push_back(how == kCopy ? Materialize(it->files[i]) : it->files[i].path);
        paths.erase(std::remove(paths.begin(), paths.end(), std::wstring()), paths.end());
        if (paths.empty()) return false;
        if (how != kCopy) {
            text = Lines(paths); // paths as text
        } else {
            obj = FilesData(paths); // pasteable in explorer and in anything that takes files
            std::string dib;
            if (obj && paths.size() == 1 && it->kind == ShelfKind::Image && shelfio::Dib({}, paths[0], dib)) Put(obj.Get(), CF_DIB, dib.data(), dib.size()); // and as a picture, for apps that take one
            if (!obj) return false;
        }
    }
    if (!obj) obj = TextData(text);
    bool ok = obj && SUCCEEDED(OleSetClipboard(obj.Get())) && SUCCEEDED(OleFlushClipboard()); // flushed: it outlives this object and the app
    if (ok) MarkOwn(); // the listener hears about it: this copy isn't news
    return ok;
}

bool Shelf::DragOut(uint64_t id, const std::vector<size_t>& picks, HWND owner) {
    const ShelfItem* it = Find(id);
    if (!it) return false;
    ComPtr<IDataObject> obj;
    if (it->HasFiles()) {
        std::vector<std::wstring> paths;
        for (size_t i : Which(*it, picks))
            if (std::wstring p = Materialize(it->files[i]); !p.empty()) paths.push_back(p); // images kept by the shelf go out as image files
        obj = FilesData(paths);
    } else {
        obj = TextData(it->text);
    }
    if (!obj) return false;
    DWORD effect = DROPEFFECT_NONE;
    HRESULT hr = SHDoDragDrop(owner, obj.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect); // never a move: the file stays where it is
    return hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
}

int Shelf::SaveImages(uint64_t id, const std::vector<size_t>& picks) {
    const ShelfItem* it = Find(id);
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    if (!it || dir.empty()) return 0;
    int n = 0;
    for (size_t i : Which(*it, picks)) {
        const ShelfFile& f = it->files[i];
        if (f.dir || !(f.owned || shelfkind::IsImageName(f.path))) continue;
        std::wstring to = Free(dir, Safe(f.name));
        if (to.empty()) continue;
        if (!f.owned) {
            n += CopyFileW(f.path.c_str(), to.c_str(), TRUE) ? 1 : 0;
            continue;
        }
        std::string bytes;
        if (shelfio::LoadImage(f.path, bytes) && shelfio::WriteAtomic(to, bytes)) ++n;
        SecureZeroMemory(bytes.data(), bytes.size());
    }
    return n;
}

void Shelf::ShowInFolder(uint64_t id, size_t pick) {
    const ShelfItem* it = Find(id);
    if (!it || pick >= it->files.size() || it->files[pick].owned) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(it->files[pick].path.c_str());
    if (!pidl) return; // gone since it was dropped
    AllowSetForegroundWindow(ASFW_ANY); // the panel never activates: explorer may come to the front on its own
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
}

bool Shelf::OpenLink(uint64_t id) {
    const ShelfItem* it = Find(id);
    if (!it || it->kind != ShelfKind::Link) return false;
    std::wstring url = it->text;
    if (_wcsnicmp(url.c_str(), L"www.", 4) == 0) url = L"https://" + url;
    if (_wcsnicmp(url.c_str(), L"http://", 7) != 0 && _wcsnicmp(url.c_str(), L"https://", 8) != 0) return false; // never another scheme
    AllowSetForegroundWindow(ASFW_ANY);
    return (INT_PTR)ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

}
