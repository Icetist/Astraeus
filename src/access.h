#pragma once
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"

// What a screen reader sees of a custom-drawn window: its controls and words, as windows' UI Automation asks for them.
// A frame hands over what it drew; nothing is gathered until a reader has asked, so it costs nothing without one.
class Access {
public:
    static constexpr UINT kMsg = WM_APP + 77; // a reader's request, carried over to the window's own thread
    enum Role { kText, kButton, kToggle, kSlider };
    struct Item {
        uint32_t id = 0;
        Role role = kText;
        std::wstring name, value;
        bool on = false;
        float num = 0; // a slider's place, 0..1
        RECT px{}; // on the screen
        bool away = false; // scrolled out of sight
    };

    Access();
    ~Access();
    void Attach(HWND hwnd, const wchar_t* title);
    void Detach();
    void Enable(bool on);
    bool Listening() const; // a reader is here: frames say what they drew
    bool OnGetObject(WPARAM wp, LPARAM lp, LRESULT& out);
    void Publish(const Ui& ui, const Canvas& canvas, float pxScale, D2D1_POINT_2F shift = {0, 0}, bool controls = true); // after a frame; shift: dips between what was drawn and the window
    void Publish(std::vector<Item> items, uint32_t focus); // a window that keeps its own list
    int Request(WPARAM wp, LPARAM lp, uint32_t& id, float& value) const; // kMsg, for such a window: what was asked of which item
    bool Apply(Ui& ui, WPARAM wp, LPARAM lp); // kMsg, on the window's thread; true when a frame is wanted

    enum Act { kFire, kFocus, kSet, kNudge };
    struct Shared;

private:
    void Swap(std::vector<Item> items, uint32_t focus);
    std::shared_ptr<Shared> shared;
    bool enabled = true;
};
