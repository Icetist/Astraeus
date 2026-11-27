#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sys::clips { // the clips on disk: videos\astraeus\<game>\<game> <date>.mp4. listing, pictures and edits; anything slow runs off the ui thread

struct Clip {
    std::wstring path, game, name; // name: the file's, without .mp4
    int64_t when = 0; // last written, unix seconds
    double secs = 0; // 0 when windows doesn't know yet
    uint64_t bytes = 0;
};

struct Picture { // premultiplied bgra, top-down
    std::vector<uint32_t> px;
    UINT w = 0, h = 0;
};

std::wstring Root(); // videos\astraeus, empty if there's no videos folder
std::wstring SafeName(std::wstring s); // a title as a file or folder name: what windows won't take becomes a space
std::wstring FolderOf(const std::wstring& game); // root\<game>
std::vector<Clip> List(const std::wstring& game, size_t max); // newest first; game empty: every game's
bool Thumb(const std::wstring& path, UINT px, Picture& out); // windows' own thumbnail of the clip
bool Frame(const std::wstring& path, double at, UINT maxW, Picture& out); // the picture at that second, decoded
double Length(const std::wstring& path); // seconds, read from the file
bool Trim(const std::wstring& path, double from, double to, const std::wstring& out, std::wstring& why); // copies that stretch, nothing encoded again: it starts on the keyframe at or before from
bool Rename(const std::wstring& path, const std::wstring& name, std::wstring& newPath, std::wstring& why);
bool Recycle(const std::wstring& path); // to the recycle bin: a mistake can be undone there
bool Copy(HWND owner, const std::wstring& path); // onto the clipboard as a file: pasted into a chat, it's sent
void Show(const std::wstring& path); // its folder, with it selected
void Watch(HWND notify, UINT msg);
constexpr uint64_t kDiscordBytes = 10ull * 1000 * 1000; // discord's upload limit without nitro
bool Shrink(const std::wstring& path, uint64_t maxBytes, std::wstring& out, std::wstring& why); // encoded again, under maxBytes, into a temp folder: for chats with an upload limit. slow: off the ui thread // once: windows says when anything in the clips folder changes, and msg goes to notify with wparam 1. no polling

}
