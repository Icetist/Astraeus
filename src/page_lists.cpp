// The tools page's to-do list and server monitor: a field on top, the rows under it, scrolled when they don't fit.
#include <algorithm>
#include <cmath>
#include <windows.h>

#include "duration.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "pages.h"

namespace {

constexpr float kFieldH = 32, kRowH = 30, kGap = 8, kXSize = 22, kReadW = 70; // kReadW: the timer field's reading of what's typed
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
    bool isTimer = kind == Model::kFieldTimer;
    c.Icon(isTimer ? glyph::Timer : glyph::Add, {r.left + 18, (r.top + r.bottom) / 2}, 16, typing ? t.accentHi : t.textDim);
    D2D1_RECT_F text = {r.left + 34, r.top, r.right - 14 - (isTimer && typing ? kReadW : 0), r.bottom};
    if (typing) {
        if (isTimer && !m.fieldEdit.empty()) { // what enter would start, so 1:30 is never a guess
            int seconds = duration::Parse(m.fieldEdit);
            c.Text(seconds ? fmt::Clock(seconds) : L"Not a time", {r.right - 14 - kReadW, r.top, r.right - 14, r.bottom}, Weight::Medium, 12, seconds ? t.accentHi : kDown, Align::Right);
        }
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

void TimerField(PageCtx& p, const D2D1_RECT_F& r) { Field(p, r, Model::kFieldTimer, L"Custom time: 20, 1h 30m or 1:30"); }

namespace {

constexpr float kGripW = 22, kChipW = 70, kPickH = 32, kLift = 6; // the drag handle, the date chip, the picker's row; how far a held row may float past the list
int pickFor = -1; // the to-do whose date picker is open
int heldTodo = -1; // the one being dragged
float heldDy = 0; // where in its row it was taken

SYSTEMTIME Day(int ymd) {
    SYSTEMTIME st{};
    st.wYear = (WORD)(ymd / 10000), st.wMonth = (WORD)(ymd / 100 % 100), st.wDay = (WORD)(ymd % 100);
    return st;
}

std::wstring DueText(int due, int today) { // today and tomorrow by name, this week by day, further by date
    if (due == today) return L"Today";
    if (due == Model::TodoAddDays(today, 1)) return L"Tomorrow";
    if (due == Model::TodoAddDays(today, -1)) return L"Yesterday";
    SYSTEMTIME st = Day(due);
    bool soon = due > today && due <= Model::TodoAddDays(today, 6);
    wchar_t buf[32] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, soon ? L"ddd d" : L"d MMM", buf, 32, nullptr);
    return buf;
}

std::wstring DayName(int ymd) {
    SYSTEMTIME st = Day(ymd);
    wchar_t buf[16] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd", buf, 16, nullptr);
    return buf;
}

bool PickChip(PageCtx& p, uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, bool on) {
    Ui& ui = p.ui;
    const Theme& t = p.shell;
    Ui::Hit h = ui.Button(id, r);
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    ui.C().Fill(r, Height(r) / 2, on ? t.accent.Alpha(0.3f) : t.text.Alpha(0.06f + 0.06f * hov));
    ui.C().Text(label, r, Weight::Medium, 11.5f, on ? t.text : t.textDim.Mix(t.text, hov), Align::Center);
    return h.clicked;
}

void Grip(Canvas& c, D2D1_POINT_2F o, Color ink) { // two columns of three dots: it can be picked up
    for (int col = 0; col < 2; ++col)
        for (int row = 0; row < 3; ++row) c.Dot({o.x - 2.5f + col * 5, o.y - 5 + row * 5.0f}, 1.2f, ink);
}

}

void TodoList(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const Input& in = ui.In();
    Field(p, Rect(r.left, r.top, Width(r), kFieldH), Model::kFieldTodo, L"Add a to-do, then Enter");
    bool anyDone = std::any_of(m.todos.begin(), m.todos.end(), [](const Model::Todo& x) { return x.done; });
    float foot = anyDone ? 24 : 0;
    D2D1_RECT_F view = {r.left, r.top + kFieldH + kGap, r.right, r.bottom - foot};
    int n = (int)m.todos.size();
    if (pickFor >= n) pickFor = -1;
    if (heldTodo >= n) heldTodo = -1;
    if (m.todos.empty()) {
        c.Text(L"Nothing to do", Rect(view.left, view.top + 8, Width(view), 18), Weight::Light, 12, t.textDim, Align::Center);
        return;
    }
    int today = Model::TodoToday();
    static float target = 0;
    float content = n * kRowH + (pickFor >= 0 ? kPickH : 0), off = sheet::Scroll(ui, "ls.todo", target, view, content);
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    const std::vector<int> asIs = order;
    if (heldTodo >= 0) { // the rest make room where it would land
        int slot = (std::clamp)((int)std::lround((in.mouse.y - heldDy - (view.top - off)) / kRowH), 0, n - 1);
        order.erase(std::find(order.begin(), order.end(), heldTodo));
        order.insert(order.begin() + slot, heldTodo);
    }
    int remove = -1;
    auto drawRow = [&](int idx, D2D1_RECT_F row, bool lifted) {
        auto& todo = m.todos[idx];
        float cy = (row.top + row.bottom) / 2;
        bool rowHot = lifted || (heldTodo < 0 && Inside(row, in.mouse));
        float hov = ui.Smooth(Ui::Id("ls.todo.hov", idx), rowHot ? 1.0f : 0, 16);
        if (lifted) {
            c.Fill(Inset(row, -2, 0), 10, t.cardHi);
            c.Stroke(Inset(row, -2, 0), 10, t.hairTop, 1);
        } else if (hov > 0.01f) {
            c.Fill(row, 10, t.text.Alpha(0.05f * hov));
        }
        uint32_t gid = Ui::Id("ls.todo.grip", idx);
        Ui::Hit g = lifted ? Ui::Hit{} : ui.Button(gid, {row.left, row.top, row.left + kGripW, row.bottom});
        Grip(c, {row.left + kGripW / 2, cy}, t.textLow.Mix(t.text, g.hover || lifted ? 0.8f : 0.0f).Alpha(0.5f + 0.5f * hov));
        if (heldTodo < 0 && ui.IsActive(gid) && in.down && std::hypot(in.mouse.x - in.pressPos.x, in.mouse.y - in.pressPos.y) > 4) // picked up
            heldTodo = idx, heldDy = in.pressPos.y - row.top, pickFor = -1;
        bool overdue = todo.due && todo.due < today && !todo.done;
        float chipX = row.right - kXSize - 8 - kChipW;
        D2D1_RECT_F words = {row.left + kGripW + 30, row.top, chipX - 6, row.bottom};
        D2D1_POINT_2F box = {row.left + kGripW + 12, cy};
        if (!lifted) {
            Ui::Hit h = ui.Button(Ui::Id("ls.todo.row", idx), {row.left + kGripW, row.top, chipX - 4, row.bottom});
            if (h.clicked && heldTodo < 0) todo.done = !todo.done, m.SaveTodos();
        }
        c.Icon(todo.done ? glyph::BoxOn : glyph::Box, box, 18, todo.done ? t.accentHi : t.textDim, todo.done);
        c.Text(todo.text, words, Weight::Regular, 12.5f, todo.done ? t.textLow : t.text);
        if (todo.done) { // struck through
            float tw = (std::min)(c.Measure(todo.text, Weight::Regular, 12.5f), Width(words));
            c.Fill(Rect(words.left, cy, tw, 1), 0.5f, t.textLow);
        }
        D2D1_RECT_F chip = Rect(chipX, cy - 11, kChipW, 22);
        if (todo.due) { // the date, red once it's passed
            Ui::Hit h = lifted ? Ui::Hit{} : ui.Button(Ui::Id("ls.todo.due", idx), chip);
            Color ink = overdue ? kDown : todo.done ? t.textLow : t.accentHi;
            c.Fill(chip, 11, ink.Alpha(h.hover || pickFor == idx ? 0.22f : 0.12f));
            c.Text(DueText(todo.due, today), chip, Weight::Medium, 11, ink, Align::Center);
            if (h.clicked) pickFor = pickFor == idx ? -1 : idx;
        } else if (!lifted && (hov > 0.01f || pickFor == idx)) { // no date yet: a calendar to give it one
            if (w::IconBtn(ui, t, Ui::Id("ls.todo.cal", idx), {chip.right - 12, cy}, 24, glyph::Calendar, 15)) pickFor = pickFor == idx ? -1 : idx;
        }
        if (!lifted && X(p, Ui::Id("ls.todo.x", idx), {row.right - kXSize / 2 - 2, cy})) remove = idx;
    };
    c.PushClip(view);
    float y = view.top - off;
    for (int idx : order) {
        D2D1_RECT_F row = Rect(view.left, y, Width(view), kRowH);
        if (idx != heldTodo && y + kRowH >= view.top && y <= view.bottom) drawRow(idx, row, false);
        y += kRowH;
        if (idx == pickFor && heldTodo < 0) { // the date picker, under its row
            auto& todo = m.todos[idx];
            float x = view.left + kGripW + 30;
            auto pick = [&](int k, const std::wstring& label, int due) {
                float w = c.Measure(label, Weight::Medium, 11.5f) + 22;
                if (x + w > view.right - 4) return; // what doesn't fit on a narrow panel is left off
                if (PickChip(p, Ui::Id("ls.todo.pick", k), Rect(x, y + 4, w, kPickH - 10), label, due != 0 && todo.due == due)) todo.due = due, pickFor = -1, m.SaveTodos();
                x += w + 6;
            };
            pick(0, L"Today", today);
            pick(1, L"Tomorrow", Model::TodoAddDays(today, 1));
            if (todo.due) pick(8, L"Clear", 0); // early, so a narrow card never leaves it off
            for (int d = 2; d <= 6; ++d) pick(d, DayName(Model::TodoAddDays(today, d)), Model::TodoAddDays(today, d));
            pick(7, L"Next week", Model::TodoAddDays(today, 7));
            y += kPickH;
        }
    }
    if (heldTodo >= 0) { // the held one floats under the mouse
        float hy = (std::clamp)(in.mouse.y - heldDy, view.top - kLift, view.bottom - kRowH + kLift);
        drawRow(heldTodo, Rect(view.left, hy, Width(view), kRowH), true);
        ui.Button(Ui::Id("ls.todo.grip", heldTodo), Rect(view.left, hy, kGripW, kRowH)); // the press stays with it
        if (!in.down) { // let go: the list takes the new order
            if (order != asIs) {
                std::vector<Model::Todo> next;
                for (int idx : order) next.push_back(m.todos[idx]);
                m.todos = std::move(next);
                m.SaveTodos();
            }
            heldTodo = -1;
        }
        ui.KeepAlive();
    }
    c.PopClip();
    sheet::Fades(c, t, view, off, content);
    if (remove >= 0) {
        m.todos.erase(m.todos.begin() + remove);
        m.SaveTodos();
        if (pickFor == remove) pickFor = -1;
        else if (pickFor > remove) --pickFor;
    }
    if (anyDone) {
        D2D1_RECT_F clear = Rect(r.right - 110, r.bottom - 20, 110, 20);
        Ui::Hit h = ui.Button(Ui::Id("ls.todo.clear"), clear);
        c.Text(L"Clear done", clear, Weight::Medium, 11, h.hover ? t.accentHi : t.textDim, Align::Right);
        if (h.clicked) {
            m.todos.erase(std::remove_if(m.todos.begin(), m.todos.end(), [](const Model::Todo& x) { return x.done; }), m.todos.end());
            m.SaveTodos();
            pickFor = -1;
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
