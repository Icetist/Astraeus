#pragma once
#include <windows.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mixer.h"

// the mixer's groups, chat mix, ducking and profiles: one setup shared by astraeus and halobar, applied by whichever runs first
namespace sys {

struct DeskGroup {
    std::wstring name;
    uint32_t color = 0x7C9CFF; // rgb
    int icon = 0; // one of Desk::kIcons
    float volume = 1;
    bool muted = false;
    bool ducks = false; // lowered while a lowering group plays
    bool lowers = false; // its sound lowers the ducking groups
    std::wstring device; // its output, empty: the default
    std::vector<std::wstring> apps; // exe keys, as the mixer spells them
};

struct DeskProfile {
    std::wstring name;
    float chatMix = 0;
    std::vector<float> volumes; // per group, by position
    std::vector<uint8_t> mutes;
    std::vector<std::wstring> devices;
};

struct Desk {
    static constexpr int kMaxGroups = 8, kMaxProfiles = 8, kIcons = 8, kColors = 8;
    static constexpr int kDuckMin = 10, kDuckMax = 90, kDuckDef = 70; // percent the ducking groups drop by
    std::vector<DeskGroup> groups;
    int game = -1, chat = -1; // the chat mix's two groups, -1 none
    float chatMix = 0; // -1 chat only .. 0 both full .. 1 game only
    int duckBy = kDuckDef;
    bool keys = false, osd = true; // global shortcuts, and the pill that answers them
    std::vector<DeskProfile> profiles;
    int profile = -1; // the one last recalled
    std::map<std::wstring, float> base; // a grouped app's own level, under its group's

    int GroupOf(const std::wstring& key) const; // -1 ungrouped
    float ChatGain(int g) const; // what the chat mix leaves of that group
    bool Chatting() const { return game >= 0 && chat >= 0 && game != chat && game < (int)groups.size() && chat < (int)groups.size(); }
    void Assign(const std::wstring& key, int g); // -1 takes it out of every group
    int AddGroup(); // -1 when full
    void RemoveGroup(int g);
    void SaveProfile(int slot, const std::wstring& name); // slot == profiles.size(): a new one
    void Recall(int slot);

    std::wstring Write() const;
    void Read(const std::wstring& text);
    static uint32_t Swatch(int k); // the eight group colors
};

Desk LoadDesk(); // from the shared registry value
void SaveDesk(const Desk& d);
bool DeskChanged(); // the shared value was written since the last call, by either app

// applies the desk to the mixer: levels, mutes, outputs, ducking. Only one app runs it at a time
class DeskEngine {
public:
    ~DeskEngine();
    bool Claim(); // true when this app holds the engine; the other only edits
    bool Owner() const { return lock != nullptr; }
    void Tick(Mixer& mix, Desk& d, float dt); // after a Refresh, or a Meter while ducking
    bool NeedsMeter(const Desk& d) const; // ducking listens: meter several times a second
    float Duck() const { return duck; } // 1 none .. lower while ducking
    void Forget() { written.clear(); }
private:
    HANDLE lock = nullptr;
    float duck = 1, quiet = 0;
    std::map<std::wstring, float> written; // what we last set, to tell our writes from someone else's
    std::set<std::wstring> mutedByUs, routedByUs;
};

}
