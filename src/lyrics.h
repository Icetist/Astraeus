#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace sys {

struct LyricLine {
    double start = 0, end = 0; // seconds; end is where the singing is guessed to stop, not the next line
    std::wstring text;
    std::vector<std::pair<double, int>> words; // word-synced songs only: when each word starts, its offset in text
};

struct Lyrics {
    std::wstring key; // the song these belong to
    bool done = false; // looked up; lines empty = no synced lyrics for it
    bool interim = false; // shown early: a word-timed version may still replace it
    int offsetMs = 0; // this song's own nudge, [ and ] in fullscreen; kept with it on disk
    bool chosen = false; // picked by hand: kept for good, an empty one means no lyrics for this song
    int version = -1, versions = 0; // just cycled to: which of how many, 0 hidden; -1 none were found; as loaded, unknown
    std::vector<LyricLine> lines;
};

struct LyricChoice { // one version of a song's lyrics, as a menu shows it
    std::wstring source;
    bool worded = false; // timed word by word
    size_t lines = 0;
};

// Time-synced lyrics from lrclib.net, looked up on a worker thread one song at a time, the last few kept in memory.
class LyricSrc {
public:
    LyricSrc();
    ~LyricSrc();
    LyricSrc(const LyricSrc&) = delete;
    LyricSrc& operator=(const LyricSrc&) = delete;

    static std::wstring Key(const std::wstring& title, const std::wstring& artist) { return title + L'\x1f' + artist; }
    void Want(const std::wstring& title, const std::wstring& artist, const std::wstring& album, double seconds); // no-op if known or underway
    std::shared_ptr<const Lyrics> Get(const std::wstring& key) const; // null until looked up
    void SetOffset(const std::wstring& key, int ms); // remembered for that song, on disk too
    void NextVersion(const std::wstring& title, const std::wstring& artist, const std::wstring& album, double seconds); // these lyrics are wrong: the next copy any source has, then none, then round again; remembered
    void AskVersions(const std::wstring& title, const std::wstring& artist, const std::wstring& album, double seconds); // every source's copies, gathered for a menu
    bool Versions(const std::wstring& key, std::vector<LyricChoice>& out, int& current) const; // false until gathered; current: which is showing, -1 none, -2 something else
    void PickVersion(const std::wstring& title, const std::wstring& artist, const std::wstring& album, double seconds, int index); // that one, -1 no lyrics; remembered
private:
    void Send(const std::wstring& title, const std::wstring& artist, const std::wstring& album, double seconds, int mode, int pick);
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
