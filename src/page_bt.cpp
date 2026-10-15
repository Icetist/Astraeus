#include <algorithm>
#include <cwchar>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"

using namespace sheet;

namespace {

float scrollTo = 0; // wheel target, eased into "bt.scroll"

int IconOf(int kind) {
    switch (kind) {
    case sys::kBtAudio: return glyph::Headphones;
    case sys::kBtKeys: return glyph::Keys;
    case sys::kBtMouse: return glyph::Mouse;
    case sys::kBtPhone: return glyph::Phone;
    case sys::kBtPc: return glyph::Pc;
    case sys::kBtPad: return glyph::Pad;
    case sys::kBtWatch: return glyph::Watch;
    default: return glyph::Devices;
    }
}

const wchar_t* KindName(int kind) {
    switch (kind) {
    case sys::kBtAudio: return L"Audio";
    case sys::kBtKeys: return L"Keyboard";
    case sys::kBtMouse: return L"Mouse";
    case sys::kBtPhone: return L"Phone";
    case sys::kBtPc: return L"Computer";
    case sys::kBtPad: return L"Controller";
    case sys::kBtWatch: return L"Wearable";
    default: return L"Device";
    }
}

bool Tab(Ui& ui, const Theme& t, const char* idName, const D2D1_RECT_F& r, const wchar_t* label, bool on) { // list header tab
    uint32_t id = Ui::Id(idName);
    Ui::Hit h = ui.Button(id, r);
    float lit = ui.Smooth(id, on ? 1.0f : 0, 14);
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    Canvas& c = ui.C();
    c.TextSp(label, {r.left + 4, r.top + 8, r.right, r.top + 22}, Weight::Medium, 10, t.textLow.Mix(t.text, 0.5f * hover).Mix(t.accent, lit), 1.4f);
    float len = (r.right - r.left - 8) * lit; // underline grows in under the active tab
    if (len > 2) c.Line({r.left + 4, r.top + 27}, {r.left + 4 + len, r.top + 27}, t.accent, 2);
    return h.clicked;
}

void Row(PageCtx& p, const D2D1_RECT_F& r, const D2D1_RECT_F& view, const sys::BtDev& d, uint64_t nowMs) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    bool live = Inside(view, ui.In().mouse); // parts scrolled under the edges stay inert
    bool hov = live && Inside(r, ui.In().mouse); // plain test, inner buttons own the clicks
    uint32_t id = Ui::Id("bt.row", Key(d.id));
    float hover = ui.Smooth(id, hov ? 1.0f : 0, 16);
    float lit = ui.Smooth(id + 1, d.on ? 1.0f : 0, 10);
    bool busy = d.pairing || (d.until > nowMs && d.on != d.want); // pending until state flips or deadline
    float cy = (r.top + r.bottom) / 2;

    c.Fill(r, 16, t.cardHi.Alpha(0.35f + 0.65f * hover)); // row lifts on hover
    if (lit > 0.01f) c.FillH(r, 16, t.accent.Alpha(0.16f * lit), t.accent.Alpha(0.02f * lit)); // connected wash
    c.StrokeV(r, 16, t.hairTop.Mix(t.accentHi.Alpha(0.5f), lit), t.hairBot, 1);
    D2D1_POINT_2F ic = {r.left + 28, cy};
    c.Dot(ic, 17, t.track.Mix(t.accent.Alpha(0.28f), lit));
    c.Icon(IconOf(d.kind), ic, 19, t.textDim.Mix(t.accentHi, lit), d.on);

    const wchar_t* label = !d.paired ? L"Pair" : d.on ? L"Disconnect" : L"Connect";
    float aw = d.on ? 96.0f : 82.0f; // fits the longer label
    D2D1_RECT_F ar = {r.right - 12 - aw, cy - 15, r.right - 12, cy + 15};
    if (Act(ui, t, id + 2, ar, label, !d.on, busy, live)) {
        if (d.paired) p.m.bt.Connect(d.id, !d.on); // toggle link
        else p.m.bt.Pair(d.id);
        p.m.OnBt(); // show the spinner this frame
    }

    float textR = ar.left - 10;
    if (d.paired) { // forget: press and hold so it can't happen by accident
        D2D1_POINT_2F fc = {ar.left - 24, cy};
        D2D1_RECT_F fr = {fc.x - 15, fc.y - 15, fc.x + 15, fc.y + 15};
        float prog = 0;
        bool fire = live && ui.Hold(id + 4, fr, 0.8f, prog);
        float show = ui.Smooth(id + 6, hov || prog > 0 ? 1.0f : 0, 14); // only on row hover
        if (show > 0.01f) {
            bool fh = live && Inside(fr, ui.In().mouse);
            if (fh) c.Dot(fc, 15, kDanger.Alpha(0.12f * show));
            c.Icon(glyph::Delete, fc, 18, (fh ? kDanger : t.textLow).Alpha(show));
            if (prog > 0) c.Arc(fc, 14, -kPi / 2, prog * 2 * kPi, kDanger, 2); // hold progress from 12 o'clock
        }
        if (fire) p.m.bt.Forget(d.id); // unpair, row vanishes via watcher
        textR = fc.x - 20;
    }

    float tx = r.left + 56;
    c.Text(d.name, {tx, cy - 19, textR, cy + 1}, Weight::Medium, 13.5f, t.text); // ellipsis if too long
    std::wstring st = d.paired ? L"Not connected" : std::wstring(KindName(d.kind)) + L"  ·  new";
    Color sc = t.textLow;
    if (d.pairing) st = L"Pairing…", sc = t.accentHi;
    else if (busy) st = d.want ? L"Connecting…" : L"Disconnecting…", sc = t.accentHi;
    else if (d.on) st = L"Connected", sc = t.accent;
    if (d.on && !busy) { // live dot before "connected"
        c.Dot({tx + 3, cy + 10}, 3, t.accent);
        tx += 11;
    }
    c.Text(st, {tx, cy + 1, textR, cy + 19}, Weight::Light, 11.5f, sc);
    if (busy) ui.KeepAlive(); // spinner + deadline expiry need frames
}

}

void DrawBt(PageCtx& p, float k) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& s = p.area;
    Frame frame = Begin(p, k);

    bool on = m.radios.BtOn();
    int paired = 0, conn = 0;
    for (const auto& d : m.btDevs)
        if (d.paired) ++paired, conn += d.on ? 1 : 0;

    Radar(ui, t, "bt.radar", frame.badge, on, on && m.btScan, glyph::Bluetooth, glyph::BluetoothOff, glyph::BtSearch);
    c.Text(L"Bluetooth", frame.title, Weight::Medium, frame.titleSize, t.text, frame.align);
    std::wstring sub = !on ? L"Turned off" : paired == 0 ? L"No devices yet"
                                                          : std::to_wstring(conn) + L" connected  ·  " + std::to_wstring(paired) + L" paired";
    c.Text(sub, frame.sub, Weight::Light, 12, t.textDim, frame.align);
    if (w::Switch(ui, t, Ui::Id("bt.power"), frame.sw, on, m.radios.HasBt())) {
        if (on) m.bt.Scan(false); // no point scanning with the radio off
        m.radios.SetBt(!on);
    }
    D2D1_RECT_F pill = frame.pill;
    if (m.shareTab) {
        bool sharing = m.share.Active();
        if (ScanBtn(ui, t, "bt.stopshare", pill, false, sharing, glyph::Stop, sharing ? L"Stop sharing" : L"Not sharing", L"")) {
            m.share.StopAll(); // thread ends, every extra device is released
            m.OnShare();
        }
    } else if (ScanBtn(ui, t, "bt.add", pill, m.btScan, on, glyph::Add, L"Add device", L"Stop scanning")) {
        m.bt.Scan(!m.btScan);
        m.OnBt();
    }

    D2D1_RECT_F list = frame.list;
    if (Tab(ui, t, "bt.tab.dev", {list.left, list.top, list.left + 80, list.top + 30}, L"DEVICES", !m.shareTab)) m.shareTab = false;
    if (Tab(ui, t, "bt.tab.share", {list.left + 88, list.top, list.left + 214, list.top + 30}, L"SHARED AUDIO", m.shareTab) && !m.shareTab) {
        m.shareTab = true;
        m.OnShare();
    }
    if (w::IconBtn(ui, t, Ui::Id("bt.close"), {list.right - 14, list.top + 14}, 30, glyph::Close, 18)) m.CloseBt();
    if (w::IconBtn(ui, t, Ui::Id("bt.more"), {list.right - 48, list.top + 14}, 30, glyph::OpenNew, 16)) {
        p.wantClose = true; // hand off to windows settings
        sys::OpenUri(L"ms-settings:bluetooth");
    }
    D2D1_RECT_F view = {list.left, list.top + 34, list.right, list.bottom}; // scrollable area under header

    if (m.shareTab) {
        DrawShare(p, view); // plays what the main output plays on more devices
    } else if (!on) {
        Empty(c, t, view, glyph::BluetoothOff, L"Bluetooth is off", L"Turn it on to see your devices");
    } else {
        struct Item { int type; const sys::BtDev* d; const wchar_t* text; }; // 0 head, 1 device, 2 note
        std::vector<Item> items;
        bool anyNew = false;
        for (const auto& d : m.btDevs)
            if (d.paired) items.push_back({1, &d, nullptr});
            else anyNew = true;
        if (paired == 0) items.push_back({2, nullptr, L"No paired devices yet. Tap Add device to find one."});
        if (m.btScan || anyNew) {
            items.push_back({0, nullptr, L"AVAILABLE"});
            for (const auto& d : m.btDevs)
                if (!d.paired) items.push_back({1, &d, nullptr});
            if (!anyNew) items.push_back({2, nullptr, L"Looking for devices nearby. Put yours in pairing mode."});
        }
        float total = 0; // content height for scroll clamp
        for (const Item& it : items) total += it.type == 1 ? kRowH + kRowGap : it.type == 0 ? kHeadH : kNoteH;
        float scroll = Scroll(ui, "bt.scroll", scrollTo, view, total);

        uint64_t nowMs = GetTickCount64(); // for pending op deadlines
        c.PushClip(view);
        float y = view.top - scroll; // first item position
        for (const Item& it : items) {
            float h = it.type == 1 ? kRowH + kRowGap : it.type == 0 ? kHeadH : kNoteH;
            if (y + h > view.top && y < view.bottom) { // skip offscreen items entirely
                D2D1_RECT_F r = {view.left, y, view.right, y + h};
                if (it.type == 1) Row(p, {r.left, r.top, r.right, r.top + kRowH}, view, *it.d, nowMs);
                else if (it.type == 0) Head(ui, t, r, it.text, m.btScan);
                else c.Text(it.text, Inset(r, 6, 0), Weight::Light, 12, t.textDim);
            }
            y += h;
        }
        c.PopClip();
        Fades(c, t, view, scroll, total);
    }

    if (ui.RClick(s)) m.CloseBt(); // right click again closes
    End(p, frame);
}
