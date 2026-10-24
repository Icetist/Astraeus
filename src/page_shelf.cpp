// The shelf: whatever was dropped on the island, newest first. Drag a card out to put it anywhere, tap it to copy it.
#include <cmath>
#include <unordered_map>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"

namespace {

constexpr float kHeadH = 34; // title, count, keep switch, clear
constexpr float kKeepW = 200, kSwitchW = 46, kKeepGap = 14; // the keep switch and its words, a clear gap between
constexpr float kCardGap = 10;
constexpr float kThumb = 64; // preview size in dips
constexpr float kDragStart = 6; // moved this far while pressed: a drag out, not a tap
constexpr double kCopiedSec = 1.4; // the tick that says it's on the clipboard
constexpr float kRadius = 18;

std::unordered_map<uint64_t, ComPtr<ID2D1Bitmap>> thumbs; // previews on the current device, made once per item
uint64_t draggedId = 0, copiedId = 0; // the card being dragged out this press; the one just copied
double copiedAt = -10;

ID2D1Bitmap* Thumb(Canvas& c, const sys::ShelfItem& it) {
    if (!it.thumbW || it.thumb.empty()) return nullptr;
    ComPtr<ID2D1Bitmap>& slot = thumbs[it.id];
    if (!slot)
        c.Target()->CreateBitmap(D2D1::SizeU(it.thumbW, it.thumbH), it.thumb.data(), it.thumbW * 4,
                                 D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &slot);
    return slot.Get();
}

std::wstring Detail(const sys::ShelfItem& it) {
    if (!it.File()) return L"Note · " + std::to_wstring(it.text.size()) + L" characters";
    size_t cut = it.path.find_last_of(L'\\');
    std::wstring parent = cut == std::wstring::npos || cut == 0 ? it.path : it.path.substr(0, cut);
    size_t from = parent.find_last_of(L'\\');
    std::wstring where = from == std::wstring::npos ? parent : parent.substr(from + 1); // just the folder it sits in
    return (it.dir ? L"Folder · " : L"in ") + where;
}

bool Card(PageCtx& p, const D2D1_RECT_F& r, const sys::ShelfItem& it) { // false once it's been removed
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    uint32_t id = Ui::Id("sh.card", (int)it.id);
    bool over = Inside(r, ui.In().mouse);
    D2D1_POINT_2F xc = {r.right - 16, r.top + 16};
    Ui::Hit xh = over ? ui.Button(Ui::Id("sh.x", (int)it.id), {xc.x - 12, xc.y - 12, xc.x + 12, xc.y + 12}) : Ui::Hit{}; // first, so a press on it isn't the card's
    Ui::Hit h = ui.Button(id, r);
    h.hover = h.hover && !xh.hover;
    D2D1_POINT_2F ms = ui.In().mouse, ps = ui.In().pressPos;
    if (ui.IsActive(id) && ui.In().down && draggedId != it.id && std::hypot(ms.x - ps.x, ms.y - ps.y) > kDragStart) {
        draggedId = it.id; // the drawer starts the shell's drag once this frame is done
        m.shelfDrag = it.id;
    }
    float hover = ui.Smooth(id + 1, h.hover || ui.IsActive(id) ? 1.0f : 0, 16);
    float press = ui.Smooth(id + 2, h.down ? 1.0f : 0, 26);
    D2D1_RECT_F body = Inset(r, 1.5f * press, 1.5f * press);
    c.Fill(body, kRadius, t.cardHi.Mix(t.track, 0.5f * hover));
    c.StrokeV(body, kRadius, t.hairTop.Mix(t.accent.Alpha(0.6f), hover), t.hairBot, 1);

    float cx = (r.left + r.right) / 2, ty = r.top + 14;
    D2D1_RECT_F pic = Rect(cx - kThumb / 2, ty, kThumb, kThumb);
    if (ID2D1Bitmap* bmp = Thumb(c, it)) {
        D2D1_SIZE_F s = bmp->GetSize();
        float k = (std::min)(kThumb / (std::max)(s.width, 1.0f), kThumb / (std::max)(s.height, 1.0f)); // fit, keep its shape
        float w = s.width * k, hh = s.height * k;
        D2D1_RECT_F fit = Rect(cx - w / 2, ty + (kThumb - hh) / 2, w, hh);
        c.Target()->DrawBitmap(bmp, fit, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        c.Dot({cx, ty + kThumb / 2}, kThumb * 0.42f, t.accent.Alpha(0.14f));
        c.Icon(!it.File() ? glyph::Note : it.dir ? glyph::Folder : glyph::File, {cx, ty + kThumb / 2}, 30, t.accentHi, !it.File());
    }
    bool copied = copiedId == it.id && ui.Now() - copiedAt < kCopiedSec;
    if (copied) { // a tick over the preview while it's fresh on the clipboard
        c.Dot({cx, ty + kThumb / 2}, 20, t.accent);
        c.Icon(glyph::Check, {cx, ty + kThumb / 2}, 22, t.onAccent, true);
        ui.KeepAlive();
    }
    float x0 = r.left + 10, tw = Width(r) - 20;
    c.Text(it.Name(), Rect(x0, ty + kThumb + 8, tw, 18), Weight::Medium, 12.5f, t.text, Align::Center);
    c.Text(copied ? std::wstring(L"Copied") : Detail(it), Rect(x0, ty + kThumb + 26, tw, 15), Weight::Light, 10.5f, copied ? t.accentHi : t.textDim, Align::Center);
    if (over || xh.hover) {
        float xa = ui.Smooth(Ui::Id("sh.x", (int)it.id), xh.hover ? 1.0f : 0.6f, 16);
        c.Dot(xc, 11, t.track.Alpha(0.9f));
        c.Icon(glyph::Close, xc, 14, t.textDim.Mix(t.text, xa));
    }
    if (xh.clicked) {
        thumbs.erase(it.id);
        m.shelf.Remove(it.id); // the item goes, the file stays where it was
        ui.Wake();
        return false;
    }
    if (h.clicked && draggedId != it.id && m.shelf.Copy(it.id, m.notify)) { // a tap that never became a drag
        copiedId = it.id;
        copiedAt = ui.Now();
    }
    return true;
}

}

void ForgetShelfArt() { thumbs.clear(); } // the device is going: its bitmaps with it

void DrawShelf(PageCtx& p) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    if (ui.In().pressed) draggedId = 0; // a new press starts clean
    const auto& items = m.shelf.Items();
    for (auto it = thumbs.begin(); it != thumbs.end();) { // previews of items that left the shelf
        bool kept = false;
        for (const auto& i : items) kept = kept || i.id == it->first;
        it = kept ? std::next(it) : thumbs.erase(it);
    }

    c.TextSp(L"SHELF", Rect(a.left + 4, a.top + 8, 80, 14), Weight::Medium, 10, t.textLow, 1.4f);
    std::wstring count = std::to_wstring(items.size()) + L" of " + std::to_wstring(sys::Shelf::kMax);
    float right = a.right, hintW = (std::min)(360.0f, a.right - (items.empty() ? 0 : 82) - kKeepW - 12 - (a.left + 62)); // the sidebar is narrow: the hint ends before the keep switch
    if (!items.empty() && hintW > 30) c.Text(count + L"  ·  drag a card out to use it, tap to copy", Rect(a.left + 62, a.top + 5, hintW, 18), Weight::Light, 11.5f, t.textDim);
    if (!items.empty()) {
        if (sheet::Act(ui, t, Ui::Id("sh.clear"), Rect(right - 70, a.top + 2, 70, 26), L"Clear", false, false, true)) {
            thumbs.clear();
            m.shelf.Clear();
            ui.Wake();
        }
        right -= 82;
    }
    bool keep = m.shelf.Keep();
    D2D1_RECT_F keepR = Rect(right - kKeepW, a.top + 6, kKeepW - kSwitchW - kKeepGap, 18); // the words end a clear gap before the switch
    Ui::Hit kh = ui.Button(Ui::Id("sh.keep", 1), keepR); // the words flip it too
    if (w::Switch(ui, t, Ui::Id("sh.keep"), {right - kSwitchW / 2 - 1, a.top + 15}, keep) || kh.clicked) m.shelf.SetKeep(!keep); // off by default: nothing of it reaches the disk
    c.Text(L"Keep after restart", keepR, Weight::Regular, 11.5f, kh.hover ? t.text : t.textDim, Align::Right);

    D2D1_RECT_F grid = {a.left, a.top + kHeadH + 6, a.right, a.bottom};
    float hot = ui.Smooth(Ui::Id("sh.hot"), m.shelfHot ? 1.0f : 0, 14);
    if (hot > 0.01f) { // something is being carried over: the whole shelf lights up to take it
        c.PushClip(grid);
        c.Glow({(grid.left + grid.right) / 2, grid.bottom}, Width(grid) * 0.6f, Height(grid) * 0.9f, t.accent.Alpha(0.22f * hot));
        c.PopClip();
        c.Stroke(Inset(grid, 1, 1), 22, t.accent.Alpha(0.8f * hot), 2);
    }
    if (items.empty()) {
        if (hot < 0.99f) c.Stroke(Inset(grid, 1, 1), 22, t.hairTop, 1);
        sheet::Empty(c, t, grid, m.shelfHot ? glyph::Drop : glyph::Shelf, m.shelfHot ? L"Let go to keep it here" : L"Drop files or text here",
                     L"Onto the top of the screen, anytime. Ten at most, newest first");
        return;
    }
    bool tall = Height(a) > Width(a);
    int cols = tall ? 2 : 5, rows = tall ? 5 : 2;
    float cw = (Width(grid) - (cols - 1) * kCardGap) / cols, ch = (Height(grid) - (rows - 1) * kCardGap) / rows;
    for (size_t i = 0; i < items.size(); ++i) {
        int col = (int)i % cols, row = (int)i / cols;
        if (!Card(p, Rect(grid.left + col * (cw + kCardGap), grid.top + row * (ch + kCardGap), cw, ch), items[i])) break; // one went: the rest moves up next frame
    }
}
