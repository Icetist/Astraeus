#pragma once
// the shelf's insides, shared by its sys_shelf*.cpp files only
#include "shelf.h"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace sys {

struct Shelf::Work {
    struct Img { // an image that arrived as bytes, not as a file
        std::wstring name;
        std::string bytes;
        bool dib = false; // a raw clipboard bitmap: made into a png before it's kept
    };
    struct Job { // a preview to make
        uint64_t key = 0;
        std::wstring path;
        bool owned = false;
        UINT px = 0;
    };
    struct Store { // images to encrypt and keep, then one card or a few
        uint64_t ticket = 0;
        std::vector<Img> imgs;
    };
    struct Stored {
        uint64_t ticket = 0;
        std::vector<ShelfFile> files; // the ones that were kept
    };
    struct Done {
        uint64_t key = 0;
        Pixels pix;
    };
    std::mutex lock; // guards everything below
    std::deque<Job> todo; // newest wanted first: the cards on screen now
    std::deque<Store> stores; // before any preview: the card is waiting for them
    std::vector<Done> done;
    std::vector<Stored> stored;
    bool running = false;
    HWND notify = nullptr;
    UINT msg = 0;
    // the ui thread's alone, no lock: previews handed over but not yet taken, and the ones asked for
    std::unordered_map<uint64_t, Pixels> ready;
    std::unordered_set<uint64_t> asked;
    uint64_t tickets = 1;
};

struct Shelf::Haul {
    std::vector<std::wstring> files;
    std::vector<Work::Img> images;
    std::wstring text;
};

namespace shelfio {
constexpr UINT_PTR kClipTimer = 1, kSaveTimer = 2, kAgeTimer = 3; // the shelf window's one-shot timers
constexpr UINT kSaveDelayMs = 800; // a burst of copies is one write
constexpr size_t kImageMax = 48u << 20; // a bigger picture isn't kept: the shelf is no photo library
constexpr size_t kTextMax = 64 * 1024; // a longer text is cut: it's a stash, not an editor
std::wstring Dir(); // %LOCALAPPDATA%\Astraeus, empty if unknown
std::wstring ImageDir(); // where owned images live, encrypted
std::wstring TempDir(); // where an owned image becomes a real file for a drag or a paste
std::string Utf8(const std::wstring& s);
std::wstring Wide(const std::string& s);
bool Protect(const std::string& plain, std::string& blob); // dpapi, this windows user only
bool Unprotect(const std::string& blob, std::string& plain);
bool ReadAll(const std::wstring& path, std::string& out, size_t max);
bool WriteAtomic(const std::wstring& path, const std::string& bytes); // temp and replace: a torn write never replaces the last good one
int64_t Now(); // unix seconds
uint64_t Hash(const void* data, size_t n);
bool StoreImage(Shelf::Work::Img& img, ShelfFile& out); // worker: png if needed, encrypt, write
bool LoadImage(const std::wstring& path, std::string& bytes); // decrypted
bool Preview(const std::wstring& path, bool owned, UINT px, Shelf::Pixels& out); // worker: shell thumbnail, or a decoded owned image
bool Dib(const std::string& encoded, const std::wstring& file, std::string& dib); // a 32 bit bottom-up dib for pasting into apps
std::wstring Serialize(const std::vector<ShelfItem>& items, size_t textMax);
bool Parse(const std::wstring& body, std::vector<ShelfItem>& out); // false: not this format
void ParseOld(const std::wstring& body, std::vector<ShelfItem>& out); // the plain file of before
void Sweep(bool images); // at launch: last run's temp copies, and every owned image when nothing was kept
bool Kept(); // the keep switch as saved, for a last save at exit after a reset
std::vector<ShelfFile> Files(const std::vector<std::wstring>& paths); // dropped paths as card files, folders marked
}

}
