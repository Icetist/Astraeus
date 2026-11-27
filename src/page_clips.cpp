// The games page's clips: the newest as cards on the page, a game's own in a view of their own, and the editor where one is
// scrubbed, trimmed, renamed, shared or binned. Reading folders, thumbnails, frames and trims happen on one worker thread that
// ends when it has nothing to do; the page only draws what's come back.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "glyphs.h"
#include "page_sheet.h"
#include "sys_clips.h"

using namespace sheet;
namespace clips = sys::clips;

namespace {

constexpr float kCardW = 176, kThumbH = 99, kCardTextH = 38, kCardGap = 12, kCardR = 12; // a clip's card: a 16:9 picture, its name and when
constexpr float kRowHead = 34;
constexpr float kBtnH = 28, kBtnW = 112;
constexpr float kBarH = 40, kHandleW = 10, kHeadW = 3; // the editor's timeline, its handles and the playhead
constexpr float kEditPad = 14;
constexpr size_t kNewest = 30, kPerGame = 500;
constexpr float kArrow = 28; // the newest clips' paging arrows
constexpr UINT kThumbPx = 256, kFrameW = 960;
constexpr double kThumbAt = 1.0; // a card's own picture: a second in, past any black first frame
constexpr float kHoldSec = 0.9f; // binning a clip: held
constexpr double kMinKeep = 1.0; // a trim keeps at least this
const Color kBlack = Color::Rgb(0x101014); // behind a picture that doesn't fill its box

struct Shared { // what the worker hands back; read and written under mu
    std::mutex mu;
    std::map<std::wstring, std::vector<clips::Clip>> lists; // by game, "" all
    int listVersion = 0; // goes up with every list stored
    std::map<std::wstring, clips::Picture> thumbs;
    std::set<std::wstring> asked;
    clips::Picture frame;
    std::wstring frameFor, frameWantFor;
    double frameAt = -1, frameWant = -1;
    bool frameBusy = false;
    std::map<std::wstring, double> lengths;
    std::wstring said; // how the last edit went
    bool edited = false; // a trim wrote a new clip: the lists are read again
    std::deque<std::function<void(Shared&)>> jobs;
    bool running = false;
    HWND notify = nullptr;
};
std::shared_ptr<Shared> shared = std::make_shared<Shared>();

std::map<std::wstring, int> listedAt; // the clips epoch each list was asked at
std::map<std::wstring, std::vector<clips::Clip>> lists; // the page's copy, taken only when the worker stored a new one
int listsSeen = -1;
std::map<std::wstring, ComPtr<ID2D1Bitmap>> bitmaps; // made from the worker's pictures, on this device
std::map<std::wstring, int> missedAt; // no picture could be had: the clips epoch then


enum View { kMain, kGame };
View view = kMain;
std::wstring viewGame; // the game whose recordings are shown
float scroll = 0;
size_t rowStart = 0; // the first of the newest clips shown in the row


void Run(HWND notify, std::function<void(Shared&)> job) { // one worker: jobs in turn, then it ends. never called with mu held
    std::lock_guard<std::mutex> lock(shared->mu);
    shared->notify = notify;
    shared->jobs.push_back(std::move(job));
    if (shared->running) return;
    shared->running = true;
    std::thread([s = shared] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // windows' thumbnail makers want an apartment of their own
        for (;;) {
            std::function<void(Shared&)> next;
            {
                std::lock_guard<std::mutex> lock(s->mu);
                if (s->jobs.empty()) {
                    s->running = false;
                    break;
                }
                next = std::move(s->jobs.front());
                s->jobs.pop_front();
            }
            next(*s);
            PostMessageW(s->notify, WM_APP_CLIPS, 0, 0);
        }
        CoUninitialize();
    }).detach();
}

const std::vector<clips::Clip>& List(Model& m, const std::wstring& game) { // as last read; read again off the ui thread when clips changed
    clips::Watch(m.notify, WM_APP_CLIPS); // a clip deleted or renamed in explorer leaves the page too
    {
        std::lock_guard<std::mutex> lock(shared->mu);
        if (shared->edited) shared->edited = false, ++m.clipsEpoch;
        if (shared->listVersion != listsSeen) lists = shared->lists, listsSeen = shared->listVersion;
    }
    auto at = listedAt.find(game);
    if (at == listedAt.end() || at->second != m.clipsEpoch) {
        listedAt[game] = m.clipsEpoch;
        size_t max = game.empty() ? kNewest : kPerGame;
        Run(m.notify, [game, max](Shared& s) {
            auto got = clips::List(game, max);
            std::lock_guard<std::mutex> lock(s.mu);
            s.lists[game] = std::move(got);
            ++s.listVersion;
        });
    }
    return lists[game];
}

ID2D1Bitmap* Bitmap(Canvas& c, const clips::Picture& pic) {
    if (pic.px.empty()) return nullptr;
    ComPtr<ID2D1Bitmap> bmp;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(c.Target()->CreateBitmap({pic.w, pic.h}, pic.px.data(), pic.w * 4, bp, &bmp))) return nullptr;
    return bmp.Detach();
}

ID2D1Bitmap* Thumb(Model& m, Canvas& c, const std::wstring& path) {
    auto it = bitmaps.find(path);
    if (it != bitmaps.end()) {
        auto miss = missedAt.find(path);
        if (it->second || miss == missedAt.end() || miss->second == m.clipsEpoch) return it->second.Get();
        bitmaps.erase(it); // none last time, and the folder has changed since: it may have finished being written
        missedAt.erase(miss);
        std::lock_guard<std::mutex> lock(shared->mu);
        shared->asked.erase(path);
    }
    clips::Picture pic;
    bool ask = false, got = false;
    {
        std::lock_guard<std::mutex> lock(shared->mu);
        auto t = shared->thumbs.find(path);
        if (t != shared->thumbs.end()) pic = std::move(t->second), shared->thumbs.erase(t), got = true;
        else ask = shared->asked.insert(path).second; // false: asked already, not back yet
    }
    if (!got && !ask) return nullptr;
    if (ask) {
        Run(m.notify, [path](Shared& s) {
            clips::Picture got;
            if (!clips::Thumb(path, kThumbPx, got)) clips::Frame(path, kThumbAt, kThumbPx, got); // windows has no thumbnail yet (a new clip, or no maker for it): a frame of our own
            std::lock_guard<std::mutex> lock(s.mu);
            s.thumbs[path] = std::move(got); // empty when neither worked: the card shows a plain face
        });
        return nullptr;
    }
    ComPtr<ID2D1Bitmap> bmp;
    bmp.Attach(Bitmap(c, pic));
    bitmaps[path] = bmp; // a picture, or none to be had (null): asked again only once the folder changes
    if (!bmp) missedAt[path] = m.clipsEpoch;
    return bmp.Get();
}

void Fit(Canvas& c, ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float radius) { // the whole picture, centred on dark: a clip is never stretched
    c.Fill(r, radius, kBlack);
    if (!bmp) return;
    D2D1_SIZE_F s = bmp->GetSize();
    float k = (std::min)(Width(r) / (std::max)(s.width, 1.0f), Height(r) / (std::max)(s.height, 1.0f));
    float w = s.width * k, h = s.height * k;
    c.Image(bmp, Rect((r.left + r.right - w) / 2, (r.top + r.bottom - h) / 2, w, h), radius * (w >= Width(r) - 1 && h >= Height(r) - 1 ? 1.0f : 0.4f));
}

std::wstring Clock(double secs) { // 1:05
    int s = (int)std::lround((std::max)(secs, 0.0));
    wchar_t buf[16];
    swprintf_s(buf, L"%d:%02d", s / 60, s % 60);
    return buf;
}

std::wstring Day(int64_t when) { // "today 14:32", or the date
    time_t t = (time_t)when, nowT = time(nullptr);
    tm a{}, n{};
    localtime_s(&a, &t), localtime_s(&n, &nowT);
    wchar_t buf[40];
    if (a.tm_year == n.tm_year && a.tm_yday == n.tm_yday) swprintf_s(buf, L"today %02d:%02d", a.tm_hour, a.tm_min);
    else wcsftime(buf, 40, L"%d %b %H:%M", &a);
    return buf;
}

bool Card(PageCtx& p, const clips::Clip& clip, const D2D1_RECT_F& r, int i, bool showGame) { // true: opened
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    uint32_t id = Ui::Id("cl.card", i);
    Ui::Hit h = ui.Button(id, r);
    ui.Name(L"Clip: " + clip.name);
    float hv = ui.Smooth(id, h.hover ? 1.0f : 0, w::kRowFade);
    c.Fill(Inset(r, -4, -4), kCardR + 2, t.text.Alpha(0.05f * hv));
    D2D1_RECT_F pic = Rect(r.left, r.top, Width(r), kThumbH);
    Fit(c, Thumb(p.m, c, clip.path), pic, kCardR);
    if (clip.secs > 0) {
        std::wstring len = Clock(clip.secs);
        float lw = c.Measure(len, Weight::Medium, 10.5f) + 12;
        D2D1_RECT_F badge = Rect(pic.right - lw - 6, pic.bottom - 24, lw, 18);
        c.Fill(badge, 9, Color::Rgb(0x000000).Alpha(0.6f));
        c.Text(len, badge, Weight::Medium, 10.5f, Color::Rgb(0xFFFFFF), Align::Center);
    }
    c.Text(clip.name, Rect(r.left + 2, pic.bottom + 5, Width(r) - 4, 17), Weight::Medium, 12, t.text); // its own name: a renamed or edited clip is told apart
    c.Text((showGame && !clip.game.empty() ? clip.game + L"  ·  " : L"") + Day(clip.when), Rect(r.left + 2, pic.bottom + 22, Width(r) - 4, 15), Weight::Light, 11, t.textDim); // the game beside when, outside its own view
    return h.clicked;
}

int PerRow(float width) { return (std::max)(1, (int)((width + kCardGap) / (kCardW + kCardGap))); }

float GridH(size_t count, float width) {
    int per = PerRow(width);
    return (float)((count + per - 1) / per) * (kThumbH + kCardTextH + kCardGap);
}

void Grid(PageCtx& p, const std::vector<clips::Clip>& list, float& y, const D2D1_RECT_F& col, bool showGame, size_t max) {
    int per = PerRow(Width(col));
    float cw = (Width(col) - (per - 1) * kCardGap) / per; // the row filled edge to edge
    size_t n = (std::min)(list.size(), max);
    for (size_t i = 0; i < n; ++i) {
        float x = col.left + (float)(i % per) * (cw + kCardGap), top = y + (float)(i / per) * (kThumbH + kCardTextH + kCardGap);
        D2D1_RECT_F r = Rect(x, top, cw, kThumbH + kCardTextH);
        if (r.bottom < p.area.top || r.top > p.area.bottom) continue;
        if (Card(p, list[i], r, (int)i, showGame)) p.wantClip = list[i].path; // opened full screen, in the studio
    }
    y += GridH(n, Width(col));
}

void GameView(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    if (w::IconBtn(ui, t, Ui::Id("cl.gback"), {a.left + 14, a.top + 14}, 28, glyph::Prev, 16)) {
        view = kMain, viewGame.clear();
        return;
    }
    ui.Name(L"Back");
    const auto& list = List(m, viewGame);
    c.Text(viewGame, Rect(a.left + 36, a.top + 2, Width(a) - 36 - kBtnW - 12, 24), Weight::Semi, 15, t.text);
    std::wstring count = std::to_wstring(list.size()) + (list.size() == 1 ? L" clip" : L" clips");
    c.Text(count, Rect(a.left + 36, a.top + 24, 200, 16), Weight::Light, 11.5f, t.textDim);
    if (Act(ui, t, Ui::Id("cl.gfolder"), Rect(a.right - kBtnW, a.top, kBtnW, kBtnH), L"Open folder", false, false, true)) {
        std::wstring dir = clips::FolderOf(viewGame);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    D2D1_RECT_F box = {a.left, a.top + 48, a.right, a.bottom};
    float content = list.empty() ? 40 : GridH(list.size(), Width(box));
    float shown = Scroll(ui, "cl.gscroll", scroll, box, content, true);
    c.PushClip(box);
    float y = box.top - shown;
    if (list.empty()) c.Text(L"No clips of this game yet. While it's played, the save shortcut keeps the last stretch", Rect(box.left + 4, y + 8, Width(box) - 8, 18), Weight::Light, 12, t.textDim);
    else Grid(p, list, y, box, false, list.size());
    c.PopClip();
    Fades(c, t, box, shown, content);
}

}

void ForgetClipArt() { // the device is going: its pictures go, and each is asked for again on the next one, or the cards stay blank
    bitmaps.clear();
    missedAt.clear();
    std::lock_guard<std::mutex> lock(shared->mu);
    shared->asked.clear();
}

void OpenRecordings(const std::wstring& game) { viewGame = game, view = kGame, scroll = 0; }

size_t ClipCount(Model& m, const std::wstring& game) { return List(m, game).size(); } // read off the ui thread, again whenever the folder changes

bool ClipViews(PageCtx& p) {
    if (view == kGame && !viewGame.empty()) return GameView(p), true;
    view = kMain;
    return false;
}

void ClipsClose() { view = kMain, viewGame.clear(); }

float ClipsRowH(Model& m, float width) {
    const auto& list = List(m, L"");
    if (list.empty()) return 0;
    return kRowHead + GridH((std::min)(list.size(), (size_t)PerRow(width)), width);
}

void ClipsRow(PageCtx& p, float& y, const D2D1_RECT_F& col) {
    const auto& list = List(p.m, L"");
    if (list.empty()) return;
    Head(p.ui, p.shell, Rect(col.left, y, Width(col), kRowHead), L"NEWEST CLIPS", false);
    size_t per = (size_t)PerRow(Width(col));
    if (rowStart >= list.size()) rowStart = 0; // fewer than before: back to the start
    if (list.size() > per) { // more than fit: arrows page through them, a row at a time
        float ax = col.right - kBtnW - 12 - 2 * kArrow, ay = y + 3 + kBtnH / 2;
        bool back = rowStart > 0, more = rowStart + per < list.size();
        if (w::IconBtn(p.ui, p.shell, Ui::Id("cl.prev"), {ax + kArrow / 2, ay}, kArrow, glyph::Prev, 15) && back) rowStart -= (std::min)(rowStart, per);
        p.ui.Name(L"Newer clips");
        if (w::IconBtn(p.ui, p.shell, Ui::Id("cl.next"), {ax + kArrow * 1.5f, ay}, kArrow, glyph::Next, 15) && more) rowStart += per;
        p.ui.Name(L"Older clips");
        std::wstring at = std::to_wstring(rowStart + 1) + L"–" + std::to_wstring((std::min)(rowStart + per, list.size())) + L" of " + std::to_wstring(list.size());
        p.ui.C().Text(at, Rect(ax - 110, y, 104, kRowHead), Weight::Light, 11, p.shell.textDim, Align::Right);
    }
    if (Act(p.ui, p.shell, Ui::Id("cl.folder"), Rect(col.right - kBtnW, y + 3, kBtnW, kBtnH), L"Open folder", false, false, true)) {
        std::wstring root = clips::Root();
        SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    y += kRowHead;
    viewGame.clear(); // opened from here, back comes here
    std::vector<clips::Clip> shown(list.begin() + (ptrdiff_t)rowStart, list.begin() + (ptrdiff_t)(std::min)(rowStart + per, list.size()));
    Grid(p, shown, y, col, true, per);
}
