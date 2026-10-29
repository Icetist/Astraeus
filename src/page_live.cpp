// The live page: one card per picked sport, a match in progress with its clock and score, else the last result, and the world's
// headlines. Cards open the full match or story in the browser.
#include <algorithm>
#include <cmath>
#include <ctime>

#include "glyphs.h"
#include "live.h"
#include "live_page.h"
#include "page_sheet.h"

namespace {

constexpr float kHeadH = 26; // the page's title row
constexpr float kGap = 10; // between cards
constexpr float kCardMinH = 128; // a card needs this for a header, two sides and a footer
constexpr float kPad = 14; // inside a card
constexpr int kWideCols = 3;
constexpr float kRowH = 22; // a side or a place
constexpr float kHeadLineH = 31; // a headline and its source
inline const Color kLiveRed = Color::Rgb(0xFF5A5F); // the live dot, the same red every score app has taught

std::wstring When(int64_t at) { // "Today 21:00", "Yesterday", "Sun 25 Oct"
    if (at <= 0) return {};
    time_t t = (time_t)at, now = time(nullptr);
    std::tm a{}, b{};
    localtime_s(&a, &t);
    localtime_s(&b, &now);
    std::tm a0 = a, b0 = b;
    a0.tm_hour = b0.tm_hour = 12, a0.tm_min = b0.tm_min = a0.tm_sec = b0.tm_sec = 0; // noon to noon: whole calendar days, dst or not
    int days = (int)std::lround(difftime(mktime(&b0), mktime(&a0)) / 86400.0);
    wchar_t buf[48];
    static const wchar_t* const kDay[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    static const wchar_t* const kMon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (days == 0) swprintf_s(buf, L"Today %02d:%02d", a.tm_hour, a.tm_min);
    else if (days == 1) swprintf_s(buf, L"Yesterday");
    else if (days == -1) swprintf_s(buf, L"Tomorrow %02d:%02d", a.tm_hour, a.tm_min);
    else if (days < 0 && days > -7) swprintf_s(buf, L"%ls %02d:%02d", kDay[a.tm_wday], a.tm_hour, a.tm_min);
    else swprintf_s(buf, L"%ls %d %ls", kDay[a.tm_wday], a.tm_mday, kMon[a.tm_mon]);
    return buf;
}

std::wstring Ago(int64_t at) { // a headline's age
    int64_t s = live::Now() - at;
    if (at <= 0 || s < 0) return {};
    if (s < 3600) return std::to_wstring((std::max)((int64_t)1, s / 60)) + L" min ago";
    if (s < 86400) return std::to_wstring(s / 3600) + L" h ago";
    return std::to_wstring(s / 86400) + L" d ago";
}

void LiveDot(Canvas& c, D2D1_POINT_2F o, double now) { // a dot that breathes while the match runs
    float k = 0.5f + 0.5f * (float)std::sin(now * 4.0);
    c.Dot(o, 7 + 3 * k, kLiveRed.Alpha(0.16f * (1 - k)));
    c.Dot(o, 3.6f, kLiveRed);
}

void Badge(Ui& ui, const Theme& t, const live::Card& d, float right, float cy) { // top right: live, the date, or what the card is
    Canvas& c = ui.C();
    if (d.state == live::kLive) {
        float w = c.Measure(L"LIVE", Weight::Semi, 9.5f) + 2;
        c.TextSp(L"LIVE", Rect(right - w, cy - 7, w + 4, 14), Weight::Semi, 9.5f, kLiveRed, 1.2f);
        LiveDot(c, {right - w - 9, cy}, ui.Now());
        ui.KeepAlive();
        return;
    }
    std::wstring s = d.state == live::kHeadlines ? std::wstring(L"Headlines") : When(d.at);
    if (d.state == live::kNext && !s.empty()) s = L"Next · " + s;
    c.Text(s, Rect(right - 130, cy - 8, 130, 16), Weight::Regular, 10.5f, t.textLow, Align::Right);
}

void Shimmer(Ui& ui, const Theme& t, const D2D1_RECT_F& r) { // a card still loading: soft bars where the score will be
    Canvas& c = ui.C();
    float k = 0.5f + 0.5f * (float)std::sin(ui.Now() * 3);
    Color bar = t.track.Alpha(0.5f + 0.4f * k);
    c.Fill(Rect(r.left, r.top, Width(r) * 0.7f, 12), 6, bar);
    c.Fill(Rect(r.left, r.top + 22, Width(r) * 0.5f, 12), 6, bar);
    c.Fill(Rect(r.left, r.top + 44, Width(r) * 0.6f, 10), 5, bar);
    ui.KeepAlive();
}

void Sides(Canvas& c, const Theme& t, const live::Card& d, const D2D1_RECT_F& r) { // two teams or players, scores on the right
    float y = r.top;
    bool done = d.state == live::kResult;
    for (const live::Row& row : d.rows) {
        if (y + kRowH > r.bottom + 2) break;
        bool lose = done && !row.win && std::any_of(d.rows.begin(), d.rows.end(), [](const live::Row& x) { return x.win; });
        Color ink = lose ? t.textDim : t.text;
        float sw = row.score.empty() ? 0 : c.Measure(row.score, Weight::Semi, 15) + 6;
        c.Text(row.name, Rect(r.left, y, Width(r) - sw - 8, kRowH), row.win ? Weight::Medium : Weight::Regular, 13, ink);
        if (sw > 0) c.Text(row.score, Rect(r.right - sw, y, sw, kRowH), Weight::Semi, 15, lose ? t.textDim : d.state == live::kLive ? t.accentHi : t.text, Align::Right);
        y += kRowH + 2;
    }
}

void Places(Canvas& c, const Theme& t, const live::Card& d, const D2D1_RECT_F& r) { // a field: who's first, second, third
    float y = r.top;
    int place = 0;
    for (const live::Row& row : d.rows) {
        if (y + kRowH > r.bottom + 2) break;
        ++place;
        c.Text(std::to_wstring(place), Rect(r.left, y, 14, kRowH), Weight::Medium, 11, place == 1 ? t.accentHi : t.textLow);
        float sw = row.score.empty() ? 0 : c.Measure(row.score, Weight::Medium, 12) + 6;
        float nw = (std::min)(c.Measure(row.name, Weight::Regular, 12.5f) + 2, Width(r) - 22 - sw - 8);
        c.Text(row.name, Rect(r.left + 20, y, nw, kRowH), place == 1 ? Weight::Medium : Weight::Regular, 12.5f, t.text);
        if (!row.sub.empty()) c.Text(row.sub, Rect(r.left + 26 + nw, y, Width(r) - nw - 30 - sw, kRowH), Weight::Light, 10.5f, t.textLow);
        if (sw > 0) c.Text(row.score, Rect(r.right - sw, y, sw, kRowH), Weight::Medium, 12, t.textDim, Align::Right);
        y += kRowH;
    }
}

bool Heads(PageCtx& p, const live::Card& d, const D2D1_RECT_F& r, uint32_t id) { // headlines, each its own link; true when one was opened
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    float y = r.top;
    int i = 0;
    for (const live::Head& h : d.heads) {
        if (y + kHeadLineH > r.bottom + 4) break;
        D2D1_RECT_F line = Rect(r.left - 6, y - 2, Width(r) + 12, kHeadLineH);
        Ui::Hit hit = ui.Button(id + 16 + i++, line);
        if (hit.hover) c.Fill(line, 8, t.text.Alpha(0.05f));
        c.Text(h.title, Rect(r.left, y, Width(r), 16), Weight::Regular, 12, hit.hover ? t.text : t.text.Mix(t.textDim, 0.15f));
        std::wstring meta = h.source + (h.at ? L" · " + Ago(h.at) : L"");
        c.Text(meta, Rect(r.left, y + 15, Width(r), 13), Weight::Light, 10, t.textLow);
        if (hit.clicked && !h.link.empty()) {
            sys::OpenUri(h.link.c_str());
            p.wantClose = true;
            return true;
        }
        y += kHeadLineH;
    }
    return false;
}

void DrawCard(PageCtx& p, const live::Card& d, const D2D1_RECT_F& r, int index) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    uint32_t id = Ui::Id("live.card", index);
    bool news = d.key == "#news";
    int sport = news ? -1 : [&] {
        size_t colon = d.key.find(':');
        return live::FindSport(d.key.substr(0, colon).c_str());
    }();
    bool links = d.state == live::kHeadlines;
    Ui::Hit h = !links && !d.link.empty() ? ui.Button(id, r) : Ui::Hit{}; // headline cards link each line instead
    float hover = ui.Smooth(id, h.hover ? 1.0f : 0, 16);
    w::Card(ui, t, r, 18);
    if (hover > 0.001f) c.Fill(r, 18, t.text.Alpha(0.035f * hover));
    if (d.state == live::kLive) c.Stroke(Inset(r, 0.5f, 0.5f), 18, kLiveRed.Alpha(0.35f), 1); // a running match draws the eye
    else if (d.major) c.Stroke(Inset(r, 0.5f, 0.5f), 18, t.accent.Alpha(0.4f), 1);
    D2D1_RECT_F in = Inset(r, kPad, 0);
    float hy = r.top + 18;
    const live::Sport* sp = sport >= 0 ? &live::SportAt(sport) : nullptr;
    c.Icon(news ? glyph::Globe : sp->glyph, {in.left + 8, hy}, 16, t.accent);
    std::wstring name = news ? L"World news" : sp->name;
    c.TextSp(fmt::Upper(name), Rect(in.left + 22, hy - 7, Width(in) * 0.55f, 14), Weight::Medium, 9.5f, t.textLow, 1.3f);
    Badge(ui, t, d, in.right, hy);
    float y = r.top + 34;
    if (!d.title.empty() && !links) {
        float tx = in.left;
        if (d.major) {
            c.Icon(glyph::Trophy, {tx + 7, y + 8}, 14, t.accentHi, true);
            tx += 18;
        }
        c.Text(d.title, Rect(tx, y, in.right - tx, 16), Weight::Medium, 11.5f, t.textDim);
        y += 22;
    }
    D2D1_RECT_F body = {in.left, y, in.right, r.bottom - 26};
    if (links) body.bottom = r.bottom - 8;
    switch (d.state) {
    case live::kLoading: Shimmer(ui, t, body); break;
    case live::kFailed:
        c.Icon(glyph::Offline, {in.left + 9, body.top + 10}, 17, t.textLow);
        c.Text(L"Can't reach the scores", Rect(in.left + 24, body.top + 1, Width(in) - 24, 18), Weight::Regular, 12, t.textDim);
        c.Text(L"Trying again in a minute", Rect(in.left + 24, body.top + 19, Width(in) - 24, 16), Weight::Light, 10.5f, t.textLow);
        break;
    case live::kHeadlines: Heads(p, d, body, id); break;
    default:
        if (d.rows.empty()) {
            c.Text(d.status.empty() ? L"Nothing on right now" : d.status, Rect(in.left, body.top, Width(in), 18), Weight::Regular, 12, t.textDim);
            break;
        }
        if (d.list) Places(c, t, d, body);
        else Sides(c, t, d, body);
        break;
    }
    if (d.state == live::kResult || d.state == live::kLive || d.state == live::kNext) { // the clock or final word, then a key stat
        std::wstring foot = d.rows.empty() ? d.note : d.status;
        if (!d.rows.empty() && !d.note.empty()) foot += (foot.empty() ? L"" : L"  ·  ") + d.note;
        c.Text(foot, Rect(in.left, r.bottom - 26, Width(in) - (h.hover ? 16 : 0), 18), Weight::Regular, 10.5f, d.state == live::kLive ? t.accentHi : t.textLow);
        if (hover > 0.01f) c.Icon(glyph::OpenNew, {in.right - 6, r.bottom - 17}, 13, t.textLow.Alpha(hover));
    }
    if (h.clicked) {
        sys::OpenUri(d.link.c_str());
        p.wantClose = true;
    }
}

}

void DrawLive(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    const D2D1_RECT_F& a = p.area;
    live::Seen(p.m.notify);
    uint64_t version = 0;
    std::vector<live::Card> cards = live::Snapshot(version);
    c.TextSp(L"LIVE", Rect(a.left + 4, a.top + 4, 80, 14), Weight::Medium, 10, t.textLow, 1.4f);
    bool busy = live::Busy();
    if (busy) sheet::Spinner(c, {a.right - 76, a.top + 11}, 6, t.textLow, ui.Now()), ui.Wake();
    if (w::IconBtn(ui, t, Ui::Id("live.refresh"), {a.right - 46, a.top + 11}, 26, glyph::Refresh, 16, !busy)) live::Refresh();
    if (w::IconBtn(ui, t, Ui::Id("live.edit"), {a.right - 16, a.top + 11}, 26, glyph::Edit, 15)) live::OpenSetup();
    if (live::Config().picks.empty()) { // turned on without picking anything yet
        D2D1_RECT_F v = {a.left, a.top + kHeadH, a.right, a.bottom - 40};
        sheet::Empty(c, t, v, glyph::Live, L"Pick the sports to follow", L"Three to five, with world news if you like");
        D2D1_RECT_F b = Rect((a.left + a.right) / 2 - 80, a.bottom - 44, 160, 32);
        if (sheet::Act(ui, t, Ui::Id("live.start"), b, L"Choose sports", true, false, true)) live::OpenSetup();
        return;
    }
    D2D1_RECT_F view = {a.left, a.top + kHeadH, a.right, a.bottom};
    bool tall = Height(a) > Width(a);
    int n = (int)cards.size();
    int cols = tall ? 1 : n <= 3 ? (std::max)(1, n) : n == 4 ? 2 : kWideCols;
    int rows = (n + cols - 1) / cols;
    float ch = tall ? kCardMinH + 18 : (std::max)(kCardMinH, (Height(view) - (rows - 1) * kGap) / rows);
    float content = rows * ch + (rows - 1) * kGap;
    static float scrollTo = 0;
    float scroll = sheet::Scroll(ui, "live.scroll", scrollTo, view, content);
    c.PushClip(Inset(view, -2, 0));
    for (int i = 0; i < n; ++i) {
        int row = i / cols, col = i % cols;
        int inRow = (std::min)(cols, n - row * cols); // the last row's cards share its width
        float w = (Width(view) - (inRow - 1) * kGap) / inRow;
        D2D1_RECT_F r = Rect(view.left + col * (w + kGap), view.top + row * (ch + kGap) - scroll, w, ch);
        if (r.bottom < view.top || r.top > view.bottom) continue;
        DrawCard(p, cards[i], r, i);
    }
    c.PopClip();
    if (content > Height(view)) sheet::Fades(c, t, view, scroll, content);
}
