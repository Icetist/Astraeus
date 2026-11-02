// The shelf's history: cards newest first, repeats moved up with a count, stacks, pins and notes, the limit and the auto-delete,
// and one worker that makes previews and keeps images. Nothing here runs unless something is dropped, copied or looked at.
#include "shelf_impl.h"

#include <objbase.h>
#include <shlwapi.h>

#include <algorithm>
#include <thread>

#include "sys.h"

namespace sys {

namespace {

constexpr size_t kNameMax = 80;
constexpr int64_t kExpireSecs[Shelf::kExpireNever] = {3600, 6 * 3600, 24 * 3600, 7 * 24 * 3600}; // 1h, 6h, 24h, 7d
constexpr int64_t kAgeSlackMs = 500; // the timer lands just after the oldest is due

bool AllImages(const std::vector<ShelfFile>& files) {
    return !files.empty() && std::all_of(files.begin(), files.end(), [](const ShelfFile& f) { return !f.dir && (f.owned || shelfkind::IsImageName(f.path)); });
}

bool SameFile(const ShelfFile& a, const ShelfFile& b) {
    if (a.owned || b.owned) return a.owned && b.owned && a.hash == b.hash;
    return _wcsicmp(a.path.c_str(), b.path.c_str()) == 0;
}

bool Same(const ShelfItem& a, const ShelfItem& b) {
    if (a.HasFiles() != b.HasFiles()) return false;
    if (!a.HasFiles()) return a.text == b.text;
    if (a.files.size() != b.files.size()) return false;
    for (size_t i = 0; i < a.files.size(); ++i)
        if (!SameFile(a.files[i], b.files[i])) return false;
    return true;
}

ShelfKind FileKind(const std::vector<ShelfFile>& files) { return AllImages(files) ? ShelfKind::Image : ShelfKind::File; }

void ForgetId(Shelf::Work& w, uint64_t id) { // previews of a card that changed or went
    for (auto it = w.ready.begin(); it != w.ready.end();) it = (it->first >> 8) == id ? w.ready.erase(it) : std::next(it);
    for (auto it = w.asked.begin(); it != w.asked.end();) it = (*it >> 8) == id ? w.asked.erase(it) : std::next(it);
}

void DropOwned(const std::vector<ShelfFile>& files) {
    for (const auto& f : files)
        if (f.owned) DeleteFileW(f.path.c_str()); // an image the shelf kept for this card alone
}

}

namespace shelfio {
std::vector<ShelfFile> Files(const std::vector<std::wstring>& paths) {
    std::vector<ShelfFile> out;
    for (const auto& p : paths) {
        ShelfFile f;
        f.path = p;
        f.name = PathFindFileNameW(p.c_str());
        DWORD a = GetFileAttributesW(p.c_str());
        f.dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        out.push_back(std::move(f));
    }
    return out;
}
}

std::wstring ShelfItem::Name() const {
    if (files.size() == 1) return files[0].name;
    if (Stack()) return std::to_wstring(files.size()) + (kind == ShelfKind::Image ? L" images" : L" files");
    size_t a = text.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = text.find_first_of(L"\r\n", a);
    return text.substr(a, (std::min)(b == std::wstring::npos ? text.size() - a : b - a, kNameMax));
}

Shelf::Shelf() : work(std::make_shared<Work>()) {}

Shelf::~Shelf() {
    if (listening) RemoveClipboardFormatListener(clipWnd);
    if (saveDue && shelfio::Kept()) Save(); // the last burst; not after a reset took the switch away
    if (clipWnd) DestroyWindow(clipWnd);
    std::lock_guard<std::mutex> g(work->lock);
    work->todo.clear(); // a running worker finishes its current job and leaves
    work->notify = nullptr;
}

void Shelf::Start(HWND hwnd, UINT message) {
    notify = hwnd;
    msg = message;
    {
        std::lock_guard<std::mutex> g(work->lock);
        work->notify = hwnd;
        work->msg = message;
    }
    keep = shelfio::Kept(); // off unless asked for: nothing about what you stash stays after you quit
    shelfio::Sweep(!keep);
    if (keep) Load();
    Window();
    started = true;
    Configure(opts);
    ScheduleAge();
}

void Shelf::Configure(const Opts& o) {
    opts = o;
    opts.limit = (std::clamp)(opts.limit, kHistMin, kHistMax);
    opts.expire = (std::clamp)(opts.expire, 0, kExpireNever);
    if (!started) return; // the model reads its settings before the window exists
    bool want = opts.capture && clipWnd;
    if (want != listening) {
        listening = want && AddClipboardFormatListener(clipWnd); // event driven: windows tells us, nothing polls
        if (!want) RemoveClipboardFormatListener(clipWnd);
    }
    size_t before = items.size();
    Trim();
    Age();
    if (items.size() != before) Changed();
    ScheduleAge();
}

const ShelfItem* Shelf::Find(uint64_t id) const {
    for (const auto& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

ShelfItem* Shelf::Get(uint64_t id) { return const_cast<ShelfItem*>(Find(id)); }

bool Shelf::Land(Haul& h) {
    int64_t now = shelfio::Now();
    if (!h.files.empty()) {
        std::vector<ShelfFile> files = shelfio::Files(h.files);
        size_t chunks = (files.size() + kStackMax - 1) / kStackMax;
        for (size_t c = chunks; c-- > 0;) { // backwards: the first dropped ends up first
            ShelfItem it;
            it.files.assign(files.begin() + c * kStackMax, files.begin() + (std::min)(files.size(), (c + 1) * kStackMax));
            it.kind = FileKind(it.files);
            it.at = now;
            Push(std::move(it));
        }
    } else if (!h.images.empty()) { // kept encrypted by the worker first; the card lands when they're written
        std::shared_ptr<Work> w = work;
        {
            std::lock_guard<std::mutex> g(w->lock);
            w->stores.push_back({w->tickets++, std::move(h.images)});
        }
        Previews();
        return true;
    } else {
        size_t a = h.text.find_first_not_of(L" \t\r\n"), b = h.text.find_last_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return false;
        ShelfItem it;
        it.kind = shelfkind::Classify(h.text, it.rgb);
        bool bare = it.kind == ShelfKind::Link || it.kind == ShelfKind::Color; // an address or a colour without the blanks around it
        it.text = bare ? h.text.substr(a, b - a + 1) : std::move(h.text);
        it.at = now;
        Push(std::move(it));
    }
    Trim();
    Changed();
    return true;
}

void Shelf::Push(ShelfItem item) {
    auto same = std::find_if(items.begin(), items.end(), [&](const ShelfItem& o) { return Same(o, item); });
    if (same != items.end()) { // again: it moves to the front with its count, keeping its pin, note and previews
        ShelfItem kept = std::move(*same);
        items.erase(same);
        kept.count = (std::min)(kept.count + item.count, 9999);
        kept.at = item.at;
        for (size_t i = 0; i < item.files.size() && i < kept.files.size(); ++i)
            if (item.files[i].owned && item.files[i].path != kept.files[i].path) DeleteFileW(item.files[i].path.c_str()); // the copy that just came in
        item = std::move(kept);
    } else {
        item.id = nextId++;
    }
    items.insert(items.begin(), std::move(item));
}

void Shelf::Trim() {
    for (size_t i = items.size(); items.size() > (size_t)opts.limit && i-- > 0;) // the oldest unkept go; pinned and noted stay whatever the count
        if (!items[i].Kept()) Erase(i);
}

void Shelf::Erase(size_t at) {
    uint64_t id = items[at].id;
    DropOwned(items[at].files);
    items.erase(items.begin() + (ptrdiff_t)at);
    ForgetId(*work, id);
    std::lock_guard<std::mutex> g(work->lock);
    auto& todo = work->todo; // a preview not made yet isn't needed any more
    todo.erase(std::remove_if(todo.begin(), todo.end(), [id](const Work::Job& j) { return (j.key >> 8) == id; }), todo.end());
}

void Shelf::Changed() {
    ++version;
    if (keep) {
        saveDue = true;
        if (!clipWnd || !SetTimer(clipWnd, shelfio::kSaveTimer, shelfio::kSaveDelayMs, nullptr)) Save();
    }
    ScheduleAge();
}

void Shelf::ScheduleAge() {
    if (!clipWnd) return;
    KillTimer(clipWnd, shelfio::kAgeTimer);
    if (opts.expire >= kExpireNever) return; // never: no timer at all
    int64_t oldest = INT64_MAX;
    for (const auto& it : items)
        if (!it.Kept()) oldest = (std::min)(oldest, it.at);
    if (oldest == INT64_MAX) return;
    int64_t ms = (oldest + kExpireSecs[opts.expire] - shelfio::Now()) * 1000 + kAgeSlackMs;
    SetTimer(clipWnd, shelfio::kAgeTimer, (UINT)(std::clamp)(ms, (int64_t)USER_TIMER_MINIMUM, (int64_t)USER_TIMER_MAXIMUM), nullptr);
}

void Shelf::Age() {
    if (opts.expire >= kExpireNever) return;
    int64_t cutoff = shelfio::Now() - kExpireSecs[opts.expire];
    bool gone = false;
    for (size_t i = items.size(); i-- > 0;)
        if (!items[i].Kept() && items[i].at < cutoff) {
            Erase(i);
            gone = true;
        }
    if (!gone) return;
    Changed();
    if (notify) PostMessageW(notify, msg, 0, 0);
}

void Shelf::Remove(uint64_t id) {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id) {
            Erase(i);
            Changed();
            return;
        }
}

void Shelf::Pin(uint64_t id, bool on) {
    if (ShelfItem* it = Get(id); it && it->pinned != on) {
        it->pinned = on;
        Changed();
    }
}

void Shelf::SetNote(uint64_t id, const std::wstring& note) {
    ShelfItem* it = Get(id);
    if (!it) return;
    std::wstring n;
    for (wchar_t c : note)
        if (n.size() < kNoteMax) n += c == L'\r' || c == L'\n' || c == L'\t' ? L' ' : c; // one line
    size_t a = n.find_first_not_of(L' '), b = n.find_last_not_of(L' ');
    n = a == std::wstring::npos ? L"" : n.substr(a, b - a + 1);
    if (n == it->note) return;
    it->note = n;
    Changed();
}

bool Shelf::CanMerge(uint64_t from, uint64_t into) const {
    const ShelfItem *a = Find(from), *b = Find(into);
    return a && b && from != into && a->HasFiles() && b->HasFiles() && a->files.size() + b->files.size() <= kStackMax;
}

bool Shelf::Merge(uint64_t from, uint64_t into) {
    if (!CanMerge(from, into)) return false;
    ShelfItem* b = Get(into);
    const ShelfItem* a = Find(from);
    b->files.insert(b->files.end(), a->files.begin(), a->files.end());
    b->kind = FileKind(b->files);
    b->pinned = b->pinned || a->pinned;
    if (b->note.empty()) b->note = a->note;
    auto at = std::find_if(items.begin(), items.end(), [from](const ShelfItem& o) { return o.id == from; });
    items.erase(at); // not Erase: its files live on in the stack
    ForgetId(*work, into); // its previews are of the smaller stack
    Changed();
    return true;
}

void Shelf::Split(uint64_t id, const std::vector<size_t>& picks) {
    auto at = std::find_if(items.begin(), items.end(), [id](const ShelfItem& o) { return o.id == id; });
    if (at == items.end() || !at->Stack()) return;
    std::vector<bool> out(at->files.size(), picks.empty());
    for (size_t p : picks)
        if (p < out.size()) out[p] = true;
    std::vector<ShelfItem> loose;
    std::vector<ShelfFile> rest;
    for (size_t i = 0; i < at->files.size(); ++i) {
        if (!out[i]) {
            rest.push_back(at->files[i]);
            continue;
        }
        ShelfItem one;
        one.files = {at->files[i]};
        one.kind = FileKind(one.files);
        one.at = at->at;
        one.id = nextId++;
        loose.push_back(std::move(one));
    }
    size_t pos = (size_t)(at - items.begin());
    if (rest.empty()) {
        items.erase(at);
    } else {
        at->files = std::move(rest);
        at->kind = FileKind(at->files);
        ++pos;
    }
    items.insert(items.begin() + (ptrdiff_t)pos, loose.begin(), loose.end()); // right after the stack: they stay where they were
    ForgetId(*work, id);
    Changed();
}

void Shelf::DropFiles(uint64_t id, const std::vector<size_t>& picks) {
    ShelfItem* it = Get(id);
    if (!it || picks.empty()) return;
    std::vector<ShelfFile> rest, gone;
    for (size_t i = 0; i < it->files.size(); ++i)
        (std::find(picks.begin(), picks.end(), i) != picks.end() ? gone : rest).push_back(it->files[i]);
    if (rest.empty()) {
        Remove(id);
        return;
    }
    DropOwned(gone);
    it->files = std::move(rest);
    it->kind = FileKind(it->files);
    ForgetId(*work, id);
    Changed();
}

size_t Shelf::ClearWhere(const std::function<bool(const ShelfItem&)>& match) {
    size_t n = 0;
    for (size_t i = items.size(); i-- > 0;)
        if (!items[i].Kept() && match(items[i])) {
            Erase(i);
            ++n;
        }
    if (n) Changed();
    return n;
}

void Shelf::WantThumb(uint64_t key) {
    if (work->asked.count(key) || work->ready.count(key)) return;
    const ShelfItem* it = Find(key >> 8);
    size_t slot = (size_t)(key >> 1 & 0x7F);
    if (!it || slot >= it->files.size()) return;
    work->asked.insert(key);
    {
        std::lock_guard<std::mutex> g(work->lock);
        work->todo.push_front({key, it->files[slot].path, it->files[slot].owned, (key & 1) ? kBigPx : kThumbPx}); // newest wanted first: what's on screen now
    }
    Previews();
}

bool Shelf::TakeThumb(uint64_t key, Pixels& out) {
    auto it = work->ready.find(key);
    if (it == work->ready.end()) return false;
    out = std::move(it->second);
    work->ready.erase(it);
    return true;
}

void Shelf::ForgetThumb(uint64_t key) {
    work->asked.erase(key);
    work->ready.erase(key);
}

void Shelf::ForgetThumbs() {
    work->asked.clear();
    work->ready.clear();
    std::lock_guard<std::mutex> g(work->lock);
    work->todo.clear();
}

void Shelf::Previews() {
    std::shared_ptr<Work> w = work;
    std::lock_guard<std::mutex> g(w->lock);
    if ((w->todo.empty() && w->stores.empty()) || w->running) return;
    w->running = true;
    try {
        std::thread([w] {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // thumbnail providers want an sta
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // never in the panel's way
            for (;;) {
                Work::Store store;
                Work::Job job;
                bool isStore = false;
                {
                    std::lock_guard<std::mutex> g2(w->lock);
                    if (!w->stores.empty()) {
                        store = std::move(w->stores.front());
                        w->stores.pop_front();
                        isStore = true;
                    } else if (!w->todo.empty()) {
                        job = std::move(w->todo.front());
                        w->todo.pop_front();
                    } else {
                        w->running = false; // nothing left: the thread is gone, no idle worker
                        break;
                    }
                }
                if (isStore) {
                    Work::Stored s{store.ticket, {}};
                    for (auto& img : store.imgs) {
                        ShelfFile f;
                        if (shelfio::StoreImage(img, f)) s.files.push_back(std::move(f));
                    }
                    std::lock_guard<std::mutex> g2(w->lock);
                    w->stored.push_back(std::move(s));
                    if (w->notify) PostMessageW(w->notify, w->msg, 0, 0);
                    continue;
                }
                Work::Done d{job.key, {}};
                shelfio::Preview(job.path, job.owned, job.px, d.pix); // a failed one still comes back: the card keeps its glyph, nothing asks again
                std::lock_guard<std::mutex> g2(w->lock);
                w->done.push_back(std::move(d));
                if (w->notify) PostMessageW(w->notify, w->msg, 0, 0);
            }
            if (SUCCEEDED(hr)) CoUninitialize();
        }).detach();
    } catch (...) {
        w->running = false;
        OutputDebugStringW(L"[shelf] worker thread failed\n");
    }
}

void Shelf::OnWork() {
    std::vector<Work::Done> done;
    std::vector<Work::Stored> stored;
    {
        std::lock_guard<std::mutex> g(work->lock);
        done.swap(work->done);
        stored.swap(work->stored);
    }
    for (auto& d : done)
        if (Find(d.key >> 8) && work->asked.count(d.key)) work->ready[d.key] = std::move(d.pix); // the card may have gone meanwhile
    if (stored.empty()) return;
    int64_t now = shelfio::Now();
    for (auto& s : stored) {
        for (size_t c = (s.files.size() + kStackMax - 1) / kStackMax; c-- > 0;) {
            ShelfItem it;
            it.files.assign(s.files.begin() + c * kStackMax, s.files.begin() + (std::min)(s.files.size(), (c + 1) * kStackMax));
            it.kind = ShelfKind::Image;
            it.at = now;
            Push(std::move(it));
        }
    }
    Trim();
    Changed();
}

}
