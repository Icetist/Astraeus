// The island as a drop target: files and text dragged onto the top strip, the pill or the open panel land on the shelf page.
#include "drawer.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <shobjidl.h>

#include <vector>

#include "deskpill.h"
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

}

struct Drawer::DropSink final : IDropTarget {
    explicit DropSink(Drawer* owner, HWND pill = nullptr) : d(owner), at(pill) {
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
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; // copy: a file is only remembered by its path, the source keeps it
        POINT p = {pt.x, pt.y};
        if (helper && d) helper->DragEnter(at ? at : d->hwnd, data, &p, *effect); // d is gone once the drawer is: ole may still call in
        return S_OK;
    }
    void Track(POINTL pt) { // over the shelf's recycle bin or not: it lights up
        if (!d || !ok || at) return; // over the long pill: the bin is in the panel, not here
        POINT p = {pt.x, pt.y};
        ScreenToClient(d->hwnd, &p);
        D2D1_POINT_2F org = d->BodyOrg((std::clamp)(d->spring.value, 0.0f, d->Depth() + d->overshoot));
        float x = p.x / d->scale - org.x, y = p.y / d->scale - org.y;
        const float* z = d->model.binZone;
        bool over = z[2] > z[0] && x >= z[0] && x < z[2] && y >= z[1] && y < z[3];
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
        if (ok && d) d->DropLeave(at != nullptr);
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
    HWND at; // the long pill's window, or null on the island's own
    LONG refs = 1;
    bool ok = false; // this drag carries something the shelf takes
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

void Drawer::ShelfOnPill() {
    HWND w = desk ? desk->Window() : nullptr;
    if (!w || dropPill) return;
    dropPill = new DropSink(this, w);
    if (FAILED(RegisterDragDrop(w, dropPill))) {
        dropPill->d = nullptr;
        dropPill->Release();
        dropPill = nullptr;
        return;
    }
    dropPillWnd = w;
}

bool Drawer::HoldForCarry() {
    bool held = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
    if (dropHold && opened && held) return true;
    dropHold = false;
    return false;
}

void Drawer::StopShelf() {
    SyncShelfKeys(false);
    if (dropPill) {
        RevokeDragDrop(dropPillWnd);
        dropPill->d = nullptr;
        dropPill->Release();
        dropPill = nullptr;
    }
    if (!drop) return;
    RevokeDragDrop(hwnd);
    drop->d = nullptr;
    drop->Release();
    drop = nullptr;
}

bool Drawer::DropEnter() {
    if (hidden || stage || !model.HasPage(kShelfPage) || (desk && desk->Editing())) return false; // a game has the screen, the fullscreen player is up, the shelf page was taken off, or the long pill is being arranged
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

void Drawer::DropLeave(bool hold) {
    model.shelfHot = false;
    if (hold && opened) dropHold = true; // off the long pill, which made way: the panel is where it's headed
    if (opened && !MouseIn()) SetTimer(hwnd, kCloseTimer, (UINT)cfg.closeMs, nullptr); // carried past it: it goes again
    Redraw();
}

void Drawer::DropDone(IDataObject* data) {
    model.shelfHot = false;
    dropHold = false;
    if (model.binOver) { // let go over the bin: not kept; files are recycled once the page's question is answered
        model.binOver = false;
        model.binAskCard = 0;
        model.binAsk = Files(data);
        if (model.binAsk.empty()) ShelfToast(L"Not kept");
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

void Drawer::CloseOrTray() {
    bool tray = opened && page == kShelfPage && !Pinned() && !stage && !hidden && (!overFull || model.pillFullOpen) && model.HasPage(kShelfPage);
    if (tray) ShelfTray(); // left the shelf, maybe to fetch something: a tray stays to drop it on
    else Close();
}

void Drawer::ShelfTray() {
    Close(); // the keyboard, the fields and the live polling go as with any close
    if (hidden || !EnsureDev()) return; // no tray: it just closed
    island = islandLook = grow = false;
    peeking = peekShelf = true;
    peekLevel = -1;
    trayNote.clear();
    float reach = Side() ? kPeekSide : kPeekH;
    Resize(reach + overshoot, true);
    spring.target = reach; // retracts only as far as the tray
    spring.damp = 0.92f;
    trayApp = AppBelow(); // one look as it shrinks, nothing polls
    trayLeft = trayApp ? kTrayAppMs : kTrayLingerMs;
    trayUntil = GetTickCount64() + trayLeft;
    SetTimer(hwnd, kPeekTimer, kTrayPollMs, nullptr);
    Redraw();
}

ULONGLONG Drawer::TrayLeft() {
    ULONGLONG now = GetTickCount64();
    bool held = !trayApp && (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0; // something may be on its way here; over an app a held button is mostly that app's own business
    if (held || MouseIn()) trayUntil = now + trayLeft; // its clock stands still: nothing runs down or starts over while something is carried
    else trayLeft = trayUntil > now ? trayUntil - now : 0;
    return trayLeft;
}

void Drawer::ShelfPasteIn() {
    bool ok = model.shelf.AddClip();
    const wchar_t* word = ok ? L"Pasted onto the shelf" : L"Nothing to paste";
    if (opened) ShelfToast(word);
    else if (peeking && peekShelf) { // on the tray: said there, it stays a moment longer
        trayNote = word;
        trayNoteAt = GetTickCount64();
        trayLeft = (std::max)(TrayLeft(), (ULONGLONG)kTrayGraceMs);
        trayUntil = GetTickCount64() + trayLeft;
    }
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
