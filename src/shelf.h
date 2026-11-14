#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct IDataObject;

namespace sys {

enum class ShelfKind : uint8_t { Text, Link, Image, File, Color, Code, kCount }; // the filter bar's order, after all

struct ShelfFile { // one file of a card: a path and never a copy, or an image the shelf keeps itself
    std::wstring path; // the original; an owned image's encrypted copy in the shelf's own folder
    std::wstring name; // what it's called on the card
    uint64_t hash = 0; // owned images: their bytes, so the same picture twice is one card
    bool dir = false, owned = false;
};

struct ShelfItem {
    uint64_t id = 0; // stable while it's on the shelf: previews and ui ids follow it
    ShelfKind kind = ShelfKind::Text;
    std::vector<ShelfFile> files; // files and images; more than one is a stack
    std::wstring text; // text, a link, a colour or code
    std::wstring note; // the user's own words, short
    bool pinned = false;
    int count = 1; // how often it was added: re-adding moves it up instead of doubling
    int64_t at = 0; // last added, unix seconds
    uint32_t rgb = 0; // a colour's value
    bool HasFiles() const { return !files.empty(); }
    bool Stack() const { return files.size() > 1; }
    bool Kept() const { return pinned || !note.empty(); } // never aged out or cleared
    std::wstring Name() const; // file name, the first line, or the stack's size
};

namespace shelfkind { // offline reading of what was copied: nothing is ever fetched
bool ParseColor(const std::wstring& s, uint32_t& rgb); // #rgb, #rrggbb(aa), rgb(), rgba(), hsl(), hsla()
std::wstring ColorText(uint32_t rgb, int how); // 0 hex, 1 rgb(), 2 hsl()
bool IsLink(const std::wstring& s); // one http(s) or www address and nothing else
struct Link {
    std::wstring site, title, type; // example.com, "How to bake bread", "Article"
};
Link ReadLink(const std::wstring& url); // from the address alone
std::wstring CleanLink(const std::wstring& url); // tracking parameters taken off
bool LooksLikeCode(const std::wstring& s);
ShelfKind Classify(const std::wstring& text, uint32_t& rgb);
bool IsImageName(const std::wstring& name); // by extension
enum Role : uint8_t { kPlain, kKeyword, kString, kComment, kNumber, kPunct };
struct Span {
    uint32_t at, len;
    Role role;
};
std::vector<Span> Highlight(const std::wstring& code); // a light tokenizer, good enough for a preview
}

class Shelf { // the shelf's history, newest first; costs nothing while nobody drops or copies
public:
    static constexpr size_t kStackMax = 10; // files or images in one card
    static constexpr int kHistMin = 100, kHistMax = 1000, kHistDef = 250, kHistStep = 50; // cards kept
    static constexpr int kExpireNever = 4; // auto-delete steps: 1h, 6h, 24h, 7d, never
    static constexpr size_t kNoteMax = 120;
    static constexpr UINT kThumbPx = 128, kBigPx = 1024; // card previews, and the one open full size
    struct Opts {
        bool capture = false; // everything copied lands here too
        int limit = kHistDef;
        int expire = kExpireNever;
        bool clearStart = false; // unkept cards go at launch
    };
    struct Pixels {
        UINT w = 0, h = 0;
        std::vector<uint32_t> px; // premultiplied bgra
        bool icon = false; // a type's icon rather than the thing itself: drawn on frosted glass
    };
    enum CopyHow { kCopy, kPlain, kPath, kHex, kRgb, kHsl, kClean };

    Shelf();
    ~Shelf();
    Shelf(const Shelf&) = delete;
    Shelf& operator=(const Shelf&) = delete;

    void Start(HWND notify, UINT msg); // loads a kept shelf; msg is posted when a preview or a stored image is ready
    void Configure(const Opts& o); // starts or stops listening to the clipboard, trims to the limit
    static bool Accepts(IDataObject* data); // files, text, links or images
    bool AddClip(); // what's on the clipboard right now, asked for by the user: true when something landed
    bool Add(IDataObject* data); // true when something landed
    void OnWork(); // ui thread: previews and stored images the worker finished
    void Age(); // the auto-delete pass; cheap, also run on a timer
    void OnClip(); // the clipboard changed: read it a moment later

    const std::vector<ShelfItem>& Items() const { return items; }
    const ShelfItem* Find(uint64_t id) const;
    uint64_t Version() const { return version; } // bumps on every change: views re-filter only then
    void Remove(uint64_t id);
    void Pin(uint64_t id, bool on);
    void SetNote(uint64_t id, const std::wstring& note);
    bool CanMerge(uint64_t from, uint64_t into) const;
    bool Merge(uint64_t from, uint64_t into); // one card dropped on another: one stack
    void Split(uint64_t id, const std::vector<size_t>& picks); // picks empty: every file its own card
    void DropFiles(uint64_t id, const std::vector<size_t>& picks); // out of a stack, gone from the shelf
    size_t ClearWhere(const std::function<bool(const ShelfItem&)>& match); // unkept matches go; returns how many
    bool Keep() const { return keep; }
    void SetKeep(bool on); // off deletes what's on disk

    bool DragOut(uint64_t id, const std::vector<size_t>& picks, HWND owner); // modal shell drag, true if dropped somewhere
    bool Copy(uint64_t id, CopyHow how, const std::vector<size_t>& picks); // onto the clipboard, never read back into the shelf
    int SaveImages(uint64_t id, const std::vector<size_t>& picks); // into downloads, returns how many
    void ShowInFolder(uint64_t id, size_t pick);
    bool OpenLink(uint64_t id); // http and https only
    bool CanPlain(const ShelfItem& it) const; // paste as plain text means something different here

    static uint64_t ThumbKey(uint64_t id, size_t slot, bool big) { return id << 8 | (uint64_t)(slot & 0x7F) << 1 | (big ? 1 : 0); }
    void WantThumb(uint64_t key); // queued once; the worker posts when it's ready
    bool TakeThumb(uint64_t key, Pixels& out); // a finished one, handed over: the page keeps its bitmap, not the pixels
    void ForgetThumb(uint64_t key); // its bitmap went: it may be asked for again
    void ForgetThumbs();

    struct Haul; // what one drop or copy brought, before it becomes cards; both only in shelf_impl.h
    struct Work;

private:
    bool Land(Haul& h); // cards from a haul; images go to the worker first
    void Push(ShelfItem item); // to the front; a repeat moves up with its count instead of doubling
    void Trim(); // over the limit: the oldest unkept go
    void Erase(size_t at); // one card, and any image the shelf owned for it
    void Changed(); // version, a save soon, the next age check
    void ScheduleAge();
    void Previews(); // starts the worker if there's work; it's gone once the queue is empty
    void Save(); // encrypted, atomic
    void Load();
    void ReadClip(); // the clipboard, unless it's private or our own
    bool OwnClip(); // what's on the clipboard is what the shelf just put there
    void MarkOwn(); // remember the clipboard as ours after a copy
    void Window(); // the message-only window: clipboard notices and the timers
    static LRESULT CALLBACK ClipProc(HWND, UINT, WPARAM, LPARAM);
    ShelfItem* Get(uint64_t id);
    std::wstring Materialize(const ShelfFile& f); // an owned image as a real file in temp, for drags and pastes

    std::vector<ShelfItem> items;
    bool keep = false, started = false, listening = false, saveDue = false;
    Opts opts;
    uint64_t nextId = 1, version = 1;
    DWORD ownSeq = 0; // the clipboard's sequence number right after our own copy
    int clipTries = 0;
    HWND notify = nullptr, clipWnd = nullptr;
    UINT msg = 0;
    std::shared_ptr<Work> work; // shared with the worker so a late finish never touches a freed shelf
};

void ShelfWipe(); // reset: the kept shelf and its images, gone from disk

}
