// The tools page's to-do list and server monitor: a field on top, the rows under it, scrolled when they don't fit.
#include <algorithm>

#include "glyphs.h"
#include "page_sheet.h"
#include "pages.h"

namespace {

constexpr float kFieldH = 32, kRowH = 30, kGap = 8, kXSize = 22;
constexpr Color kUp = Color{0.36f, 0.84f, 0.56f, 1}, kDown = Color{1.0f, 0.45f, 0.45f, 1}; // a host answering, or not

void Field(PageCtx& p, const D2D1_RECT_F& r, int kind, const wchar_t* hint) { // click to type; enter adds, esc lets go
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    bool typing = m.field == kind;
    Ui::Hit h = ui.Button(Ui::Id("ls.field", kind), r);
    float hov = ui.Smooth(Ui::Id("ls.field", kind + 10), h.hover || typing ? 1.0f : 0, 16);
    c.Fill(r, Height(r) / 2, t.track.Mix(t.cardHi, 0.3f + 0.3f * hov));
    c.Stroke(r, Height(r) / 2, typing ? t.accent.Alpha(0.8f) : t.hairTop, 1);
    c.Icon(glyph::Add, {r.left + 18, (r.top + r.bottom) / 2}, 16, typing ? t.accentHi : t.textDim);
    D2D1_RECT_F text = {r.left + 34, r.top, r.right - 14, r.bottom};
    if (typing) {
        bool caret = (int)(ui.Now() * 2) % 2 == 0;
        c.Text(m.fieldEdit + (caret ? L"|" : L""), text, Weight::Regular, 12.5f, t.text);
        ui.KeepAlive(); // the caret blinks
    } else {
        c.Text(hint, text, Weight::Light, 12, t.textLow);
    }
    if (h.clicked && !typing) m.field = kind, m.fieldEdit.clear();
}

bool X(PageCtx& p, uint32_t id, D2D1_POINT_2F o) { return w::IconBtn(p.ui, p.shell, id, o, kXSize, glyph::Close, 13); }

}

void TodoList(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    Field(p, Rect(r.left, r.top, Width(r), kFieldH), Model::kFieldTodo, L"Add a to-do, then Enter");
    bool anyDone = std::any_of(m.todos.begin(), m.todos.end(), [](const Model::Todo& x) { return x.done; });
    float foot = anyDone ? 24 : 0;
    D2D1_RECT_F view = {r.left, r.top + kFieldH + kGap, r.right, r.bottom - foot};
    if (m.todos.empty()) {
        c.Text(L"Nothing to do", Rect(view.left, view.top + 8, Width(view), 18), Weight::Light, 12, t.textDim, Align::Center);
        return;
    }
    static float target = 0;
    float content = m.todos.size() * kRowH, off = sheet::Scroll(ui, "ls.todo", target, view, content);
    c.PushClip(view);
    int remove = -1;
    for (int i = 0; i < (int)m.todos.size(); ++i) {
        auto& todo = m.todos[i];
        float y = view.top + i * kRowH - off;
        if (y + kRowH < view.top || y > view.bottom) continue;
        D2D1_RECT_F row = Rect(view.left, y, Width(view), kRowH);
        Ui::Hit h = ui.Button(Ui::Id("ls.todo.row", i), {row.left, row.top, row.right - kXSize - 6, row.bottom});
        float hov = ui.Smooth(Ui::Id("ls.todo.row", i + 1000), h.hover ? 1.0f : 0, 16);
        if (hov > 0.01f) c.Fill(row, 10, t.text.Alpha(0.05f * hov));
        D2D1_POINT_2F box = {row.left + 14, (row.top + row.bottom) / 2};
        c.Icon(todo.done ? glyph::BoxOn : glyph::Box, box, 18, todo.done ? t.accentHi : t.textDim, todo.done);
        D2D1_RECT_F words = {row.left + 32, row.top, row.right - kXSize - 10, row.bottom};
        c.Text(todo.text, words, Weight::Regular, 12.5f, todo.done ? t.textLow : t.text);
        if (todo.done) { // struck through
            float tw = (std::min)(c.Measure(todo.text, Weight::Regular, 12.5f), Width(words));
            c.Fill(Rect(words.left, box.y, tw, 1), 0.5f, t.textLow);
        }
        if (h.clicked) todo.done = !todo.done, m.SaveTodos();
        if (X(p, Ui::Id("ls.todo.x", i), {row.right - kXSize / 2 - 2, box.y})) remove = i;
    }
    c.PopClip();
    sheet::Fades(c, t, view, off, content);
    if (remove >= 0) m.todos.erase(m.todos.begin() + remove), m.SaveTodos();
    if (anyDone) {
        D2D1_RECT_F clear = Rect(r.right - 110, r.bottom - 20, 110, 20);
        Ui::Hit h = ui.Button(Ui::Id("ls.todo.clear"), clear);
        c.Text(L"Clear done", clear, Weight::Medium, 11, h.hover ? t.accentHi : t.textDim, Align::Right);
        if (h.clicked) {
            m.todos.erase(std::remove_if(m.todos.begin(), m.todos.end(), [](const Model::Todo& x) { return x.done; }), m.todos.end());
            m.SaveTodos();
        }
    }
}

void HostList(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    Field(p, Rect(r.left, r.top, Width(r), kFieldH), Model::kFieldHost, L"Add a server: a name, an address or https://…");
    D2D1_RECT_F view = {r.left, r.top + kFieldH + kGap, r.right, r.bottom - 24};
    if (m.hosts.empty()) {
        c.Text(L"Each one is checked once a minute", Rect(view.left, view.top + 8, Width(view), 18), Weight::Light, 12, t.textDim, Align::Center);
        return;
    }
    static float target = 0;
    float content = m.hosts.size() * kRowH, off = sheet::Scroll(ui, "ls.host", target, view, content);
    c.PushClip(view);
    int remove = -1;
    for (int i = 0; i < (int)m.hosts.size(); ++i) {
        float y = view.top + i * kRowH - off;
        if (y + kRowH < view.top || y > view.bottom) continue;
        D2D1_RECT_F row = Rect(view.left, y, Width(view), kRowH);
        float cy = (row.top + row.bottom) / 2;
        const sys::extras::HostHealth* h = i < (int)m.hostNow.size() && m.hostNow[i].host == m.hosts[i] ? &m.hostNow[i] : nullptr;
        bool known = h && h->known;
        c.Dot({row.left + 12, cy}, 5, !known ? t.textLow : h->up ? kUp : kDown);
        c.Text(m.hosts[i], {row.left + 28, row.top, row.right - 110, row.bottom}, Weight::Regular, 12.5f, t.text);
        std::wstring state = !known ? L"Checking…" : !h->up ? L"Down" : h->ms >= 0 ? std::to_wstring(h->ms) + L" ms" : L"Up";
        c.Text(state, {row.right - 106, row.top, row.right - kXSize - 10, row.bottom}, Weight::Medium, 11.5f, !known ? t.textLow : h->up ? t.textDim : kDown, Align::Right);
        if (X(p, Ui::Id("ls.host.x", i), {row.right - kXSize / 2 - 2, cy})) remove = i;
    }
    c.PopClip();
    sheet::Fades(c, t, view, off, content);
    if (remove >= 0) m.hosts.erase(m.hosts.begin() + remove), m.SaveHosts();
    D2D1_RECT_F again = Rect(r.right - 110, r.bottom - 20, 110, 20);
    Ui::Hit ah = ui.Button(Ui::Id("ls.host.now"), again);
    c.Text(L"Check now", again, Weight::Medium, 11, ah.hover ? t.accentHi : t.textDim, Align::Right);
    if (ah.clicked) m.pinger.Now();
}
