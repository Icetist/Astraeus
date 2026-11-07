// An editor's undo and redo: whole snapshots of what it edits, a few dozen numbers each.
#pragma once
#include <cstddef>
#include <vector>

class Ui;
struct Theme;

class History {
public:
    using Snap = std::vector<float>; // whole numbers keep exactly up to 2^24, plenty for ids and sizes
    // every frame the editor shows: a new run starts afresh; a change counts once the mouse lets go, so a drag is one step
    void Sync(int run, const Snap& s, bool settled) {
        if (run != at) {
            at = run, now = s;
            back.clear(), ahead.clear();
            return;
        }
        if (!settled || s == now) return;
        back.push_back(now);
        if (back.size() > kDepth) back.erase(back.begin());
        ahead.clear();
        now = s;
    }
    bool CanUndo() const { return !back.empty(); }
    bool CanRedo() const { return !ahead.empty(); }
    const Snap& Undo() { // the editor loads what comes back
        ahead.push_back(now);
        now = back.back();
        back.pop_back();
        return now;
    }
    const Snap& Redo() {
        back.push_back(now);
        now = ahead.back();
        ahead.pop_back();
        return now;
    }

private:
    static constexpr size_t kDepth = 64;
    std::vector<Snap> back, ahead;
    Snap now;
    int at = -1;
};

namespace history {
constexpr float kBtn = 30, kGap = 6, kW = 2 * kBtn + kGap; // the undo and redo pair, round chips the height of the pills beside them
constexpr float kResetW = 72;
int Buttons(Ui& ui, const Theme& t, const char* id, float left, float midY, const History& h, bool live); // -1 undo, 1 redo, 0 neither
}
