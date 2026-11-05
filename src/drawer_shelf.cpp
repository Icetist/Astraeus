// The island as a drop target: files and text dragged onto the top strip, the pill or the open panel land on the shelf page.
#include "drawer.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <shobjidl.h>

#include <thread>
#include <vector>

#include "glyphs.h"

namespace {
constexpr UINT kPasteDelayMs = 60; // the app in front settles with the keyboard before ctrl+v is sent
constexpr size_t kQueryMax = 80; // the shelf's search field

std::vector<std::wstring> Files(IDataObject* data) { // the paths a drag carries, if it carries files
    std::vector<std::wstring> out;
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med = {};
    if (!data || FAILED(data->GetData(&fmt, &med))) return out;
    if (HDROP drop = (HDROP)GlobalLock(med.hGlobal)) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 4] = {};
            if (DragQueryFileW(drop, i, path, (UINT)std::size(path))) out.push_back(path);
        }
        GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
    return out;
}

void Recycle(std::vector<std::wstring> files) { // into the recycle bin, through windows' own copy engine: its progress, its prompts, undo from the bin
    std::thread([files = std::move(files)] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::wstring list;
        for (const auto& f : files) list += f + L'\0';
        list += L'\0'; // the list ends with two nulls
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = list.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_WANTNUKEWARNING; // the bin, never gone for good: too big for the bin, windows asks first
        SHFileOperationW(&op);
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}
}

struct Drawer::DropSink final : IDropTarget {
    explicit DropSink(Drawer* owner) : d(owner) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)); // the shell keeps drawing the drag image over us
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        ok = d && !d->dragSelf && sys::Shelf::Accepts(data) && d->DropEnter();
        FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        files = ok && data->QueryGetData(&fmt) == S_OK; // only files can go in the bin
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; // copy: a file is only remembered by its path, the source keeps it
        POINT p = {pt.x, pt.y};
        if (helper && d) helper->DragEnter(d->hwnd, data, &p, *effect); // d is gone once the drawer is: ole may still call in
        return S_OK;
    }
    void Track(POINTL pt) { // over the shelf's recycle bin or not: it lights up
        if (!d || !ok) return;
        POINT p = {pt.x, pt.y};
        ScreenToClient(d->hwnd, &p);
        D2D1_POINT_2F org = d->BodyOrg((std::clamp)(d->spring.value, 0.0f, d->Depth() + d->overshoot));
        float x = p.x / d->scale - org.x, y = p.y / d->scale - org.y;
        const float* z = d->model.binZone;
        bool over = files && z[2] > z[0] && x >= z[0] && x < z[2] && y >= z[1] && y < z[3];
        if (over != d->model.binOver) d->model.binOver = over, d->Redraw();
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        Track(pt);
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; // copy even into the bin: the source never deletes anything on our word
        POINT p = {pt.x, pt.y};
        if (helper) helper->DragOver(&p, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper) helper->DragLeave();
        if (ok && d) d->DropLeave();
        ok = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        POINT p = {pt.x, pt.y};
        if (helper) helper->Drop(data, &p, *effect);
        if (ok && d) d->DropDone(data);
        ok = false;
        return S_OK;
    }

    Drawer* d; // cleared before the drawer goes, ole may still hold a reference
    LONG refs = 1;
    bool ok = false; // this drag carries something the shelf takes
    bool files = false; // and it's files
    ComPtr<IDropTargetHelper> helper;
};

void Drawer::StartShelf() {
    if (drop) return;
    drop = new DropSink(this);
    if (FAILED(RegisterDragDrop(hwnd, drop))) { // ole not up on this thread: the shelf still works, it just can't be dropped on
        drop->d = nullptr;
        drop->Release();
        drop = nullptr;
    }
}

void Drawer::StopShelf() {
    if (!drop) return;
    RevokeDragDrop(hwnd);
    drop->d = nullptr;
    drop->Release();
    drop = nullptr;
}

bool Drawer::DropEnter() {
    if (hidden || stage || !model.HasPage(kShelfPage)) return false; // a game has the screen, the fullscreen player is up, or the shelf page was taken off
    KillTimer(hwnd, kHoverTimer);
    KillTimer(hwnd, kCloseTimer);
    armed = false;
    if (!opened) { // closed: it drops open straight onto the shelf, no slide from another page
        if (page != kShelfPage) prevPage = page, page = kShelfPage, pageT = 1;
        pageSet = true; // a fixed opening page picked in settings mustn't take the drop's place
        Open();
    } else {
        SetPage(kShelfPage);
    }
    model.shelfHot = opened; // lit only if the panel really came down
    Redraw();
    return opened;
}

void Drawer::DropLeave() {
    model.shelfHot = false;
    if (opened && !MouseIn()) SetTimer(hwnd, kCloseTimer, (UINT)cfg.closeMs, nullptr); // carried past it: it goes again
    Redraw();
}

void Drawer::DropDone(IDataObject* data) {
    model.shelfHot = false;
    if (model.binOver) { // let go over the bin: recycled, not kept
        model.binOver = false;
        std::vector<std::wstring> files = Files(data);
        if (!files.empty()) {
            size_t n = files.size();
            Recycle(std::move(files));
            ShelfToast(n == 1 ? L"Sent to the Recycle Bin" : (std::to_wstring(n) + L" items sent to the Recycle Bin").c_str());
        }
    } else {
        model.shelf.Add(data);
    }
    TrackLeave(); // the drag kept mouse messages from us: the panel learns where the mouse is now
    if (!MouseIn()) SetTimer(hwnd, kCloseTimer, (UINT)cfg.closeMs, nullptr);
    Redraw();
}

void Drawer::ShelfOut() {
    uint64_t id = model.shelfDrag;
    model.shelfDrag = 0;
    if (!id || !opened) return;
    if (GetCapture() == hwnd) ReleaseCapture(); // ole's drag loop takes the mouse from here
    dragSelf = true;
    model.shelf.DragOut(id, model.shelfPicks, hwnd); // returns once it's dropped or called off
    model.shelfPicks.clear();
    dragSelf = false;
    if (!MouseIn()) SetTimer(hwnd, kCloseTimer, (UINT)cfg.closeMs, nullptr);
    Redraw();
}

void Drawer::ShelfPaste() {
    uint64_t id = model.shelfPaste;
    model.shelfPaste = 0;
    auto how = (sys::Shelf::CopyHow)model.shelfPasteHow;
    bool ok = id && model.shelf.Copy(id, how, model.shelfPicks);
    model.shelfPicks.clear();
    if (!ok) return;
    HWND front = GetForegroundWindow(); // the panel never activates on a click: the app in front still has the keyboard
    if (front == hwnd) front = typing ? typingFrom : keys ? keysFrom : nullptr; // the search or the shortcut took it: it goes back where it came from
    model.shelfField = Model::kShelfNone;
    Close(); // hands the keyboard back first, then the paste lands where the user was
    if (!front || !IsWindow(front) || front == hwnd) return; // nowhere sensible: it's on the clipboard
    pasteTo = front;
    SetTimer(hwnd, kPasteTimer, kPasteDelayMs, nullptr);
}

void Drawer::ShelfKey(WPARAM wp) {
    wchar_t ch = (wchar_t)wp;
    bool note = model.shelfField == Model::kShelfNote;
    std::wstring& s = note ? model.shelfEdit : model.shelfQuery;
    size_t cap = note ? sys::Shelf::kNoteMax : kQueryMax;
    if (ch == 22) { // ctrl+v, the first line
        for (wchar_t c : sys::ClipText()) {
            if (c == L'\r' || c == L'\n') break;
            if (c >= 32 && s.size() < cap) s += c;
        }
    } else if (ch == 8) {
        if (!s.empty()) s.pop_back();
    } else if (ch == 127) { // ctrl+backspace: the last word
        while (!s.empty() && s.back() == L' ') s.pop_back();
        while (!s.empty() && s.back() != L' ') s.pop_back();
    } else if (ch >= 32 && s.size() < cap) {
        s += ch;
    }
}
