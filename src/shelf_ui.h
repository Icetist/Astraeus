#pragma once
// the shelf page's insides, shared by page_shelf*.cpp only
#include <string>
#include <vector>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"

namespace shelfui {

constexpr float kRadius = 16; // card corners
constexpr float kGap = 10; // between cards
constexpr float kDragStart = 6; // moved this far while pressed: a carry, not a tap
constexpr double kToastSec = 1.6; // a short word that something happened
constexpr int kLinkGlyph = 0xE80B, kColorGlyph = 0xE91C, kSearchGlyph = 0xE8B6, kOpenGlyph = 0xE5CC, kSaveGlyph = 0xF090; // in the bundled subset

enum Action { // what a right-click menu, the preview's buttons and the clear menu do
    kPaste, kPastePlain, kCopy, kNote, kPin, kPreview, kDelete, kOpen, kCopyClean, kHex, kRgb, kHsl, kShowFolder, kCopyPath, kSaveImages, kUngroup,
    kClearHour, kClearDay, kClearAll, kSplitPicked, kDropPicked
};

struct View { // what the page remembers between frames
    int filter = 0; // 0 all, else a ShelfKind + 1
    float scroll = 0;
    uint64_t preview = 0; // the card open full size
    std::vector<bool> picks; // its files picked
    float pvScroll = 0;
    uint64_t menu = 0; // the card a right-click menu is open for
    bool clearMenu = false;
    D2D1_POINT_2F menuAt{};
    double menuSince = -10; // the frame it opened on: that frame's own click doesn't close it
    uint64_t carry = 0; // a card carried inside the panel, onto another to stack them
    int carrySlot = -1; // a file carried out of the open stack
    bool carried = false; // this press became a carry: its release is no tap
    uint64_t copiedId = 0;
    double copiedAt = -10;
    std::wstring toast;
    double toastAt = -10;
    double agedAt = -10;
    anim::Spring bin; // how far the recycle bin has opened sideways, in dips: the panel's own spring
    uint64_t seenVer = 0; // what the lists below were made from
    int seenFilter = -1;
    std::wstring seenQuery;
    std::vector<size_t> pinned, recent; // indexes into the shelf's items, in order
};
View& V();

struct Art { // a preview on this device
    ComPtr<ID2D1Bitmap> bmp, soft; // the picture, and a tiny copy that blurs into a wash when stretched
    ComPtr<ID2D1BitmapBrush> brush, softBrush; // their own brushes: the canvas keeps only three
    bool icon = false; // a type's icon: shown on frosted glass
    uint32_t used = 0;
};
const Art* Thumb(Canvas& c, Model& m, uint64_t id, size_t slot, bool big); // null until the worker has it, or when there's none
void Cover(Canvas& c, ID2D1Bitmap* bmp, ID2D1BitmapBrush* b, const D2D1_RECT_F& r, float radius, float opacity, bool contain = false);
void Forget(); // the device is going
void Tick(Model& m); // once a frame: previews unused for a while let go

void Para(Canvas& c, const std::wstring& s, const D2D1_RECT_F& r, Weight w, float size, Color col, uint64_t key, bool mono = false); // wrapped, cached by key
float ParaHeight(const std::wstring& s, float width, Weight w, float size, uint64_t key, bool mono = false);
void Code(Canvas& c, const Theme& t, const std::wstring& s, D2D1_POINT_2F at, float width, uint64_t key, float& height); // highlighted, for the open preview

void Frosted(Canvas& c, const D2D1_RECT_F& r, float radius, const Art* art, int glyph, const Theme& t); // a file's icon on frosted glass
void PinMark(Canvas& c, D2D1_POINT_2F at, float size, Color col);
int KindGlyph(const sys::ShelfItem& it);
std::wstring Detail(const sys::ShelfItem& it); // the quiet line under a card's name
std::wstring Folder(const std::wstring& path); // the folder a file sits in, by name
void Card(PageCtx& p, const D2D1_RECT_F& r, const sys::ShelfItem& it, float hover, bool target); // the card's face
void Toast(View& v, double now, const std::wstring& s);

void Preview(PageCtx& p, const D2D1_RECT_F& area, bool live); // the card open full size, over the grid
void Menu(PageCtx& p, const D2D1_RECT_F& area); // the right-click menu, or the clear menu
void NoteBar(PageCtx& p, const D2D1_RECT_F& area); // a note being typed
void Do(PageCtx& p, uint64_t id, Action a, const std::vector<size_t>& picks = {}); // one action on one card
bool Matches(const sys::ShelfItem& it, int filter, const std::wstring& query);

}
