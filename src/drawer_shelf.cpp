// The island as a drop target: files and text dragged onto the top strip, the pill or the open panel land on the shelf page.
#include "drawer.h"

#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

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
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; // copy: a file is only remembered by its path, the source keeps it
        POINT p = {pt.x, pt.y};
        if (helper && d) helper->DragEnter(d->hwnd, data, &p, *effect); // d is gone once the drawer is: ole may still call in
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
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
    model.shelf.Add(data);
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
    model.shelf.DragOut(id, hwnd); // returns once it's dropped or called off
    dragSelf = false;
    if (!MouseIn()) SetTimer(hwnd, kCloseTimer, (UINT)cfg.closeMs, nullptr);
    Redraw();
}
