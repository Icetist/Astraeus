// The screen reader's view of a window. One provider object stands for the window and one for each control; they read a list
// the last frame left and turn a reader's press into a message, so nothing is touched off the window's own thread.
#include "access.h"

#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "glyphs.h"

#pragma comment(lib, "uiautomationcore.lib")

struct Access::Shared {
    std::mutex lock;
    HWND hwnd = nullptr;
    std::wstring title;
    std::vector<Item> items;
    uint32_t focus = 0;
    bool asked = false; // a reader has come
};

namespace {

constexpr int kValueScale = 10000; // a slider's place across a message
constexpr float kRowSlack = 8, kAboveMax = 44; // dips: how far off a label can sit from a control with no words of its own
constexpr float kBesideFirst = 2000; // a label over a control counts only when none is beside it
constexpr double kSmallStep = 5, kLargeStep = 10; // percent

const wchar_t* GlyphWord(int g) { // an icon button's name
    switch (g) {
    case glyph::Close: return L"Close";
    case glyph::Play: return L"Play";
    case glyph::Pause: return L"Pause";
    case glyph::Next: return L"Next";
    case glyph::Prev: return L"Previous";
    case glyph::Shuffle: return L"Shuffle";
    case glyph::Repeat:
    case glyph::RepeatOne: return L"Repeat";
    case glyph::Stop: return L"Stop";
    case glyph::Edit: return L"Edit";
    case glyph::Settings: return L"Settings";
    case glyph::OpenNew: return L"Open";
    case glyph::Add: return L"Add";
    case glyph::Delete: return L"Delete";
    case glyph::Copy: return L"Copy";
    case glyph::Check: return L"Done";
    case glyph::VolumeUp:
    case glyph::VolumeDown: return L"Mute";
    case glyph::VolumeOff: return L"Unmute";
    case glyph::Mic:
    case glyph::MicOff: return L"Microphone";
    case glyph::Lock: return L"Lock";
    case glyph::Sleep: return L"Sleep";
    case glyph::Restart: return L"Restart";
    case glyph::Power: return L"Shut down";
    case glyph::SignOut: return L"Sign out";
    case glyph::Wifi: return L"Wi-Fi";
    case glyph::Bluetooth:
    case glyph::BluetoothOff: return L"Bluetooth";
    case glyph::More: return L"More";
    case glyph::Less: return L"Less";
    case glyph::Up: return L"Up";
    case glyph::Down: return L"Down";
    case glyph::Timer: return L"Timer";
    default: return nullptr;
    }
}

BSTR Str(const std::wstring& s) { return SysAllocStringLen(s.c_str(), (UINT)s.size()); }

class Node : public IRawElementProviderSimple, public IRawElementProviderFragment, public IRawElementProviderFragmentRoot, public IInvokeProvider, public IToggleProvider, public IRangeValueProvider {
public:
    Node(std::shared_ptr<Access::Shared> s, uint32_t element) : shared(std::move(s)), id(element) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == __uuidof(IRawElementProviderFragmentRoot) && !id) *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (iid == __uuidof(IInvokeProvider)) *out = static_cast<IInvokeProvider*>(this);
        else if (iid == __uuidof(IToggleProvider)) *out = static_cast<IToggleProvider*>(this);
        else if (iid == __uuidof(IRangeValueProvider)) *out = static_cast<IRangeValueProvider*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    // simple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override {
        *o = (ProviderOptions)(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading); // called on the window's thread
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** out) override {
        *out = nullptr;
        Access::Item it;
        if (!id || !Find(it)) return S_OK;
        bool has = (pattern == UIA_InvokePatternId && it.role == Access::kButton) || (pattern == UIA_TogglePatternId && it.role == Access::kToggle) ||
                   (pattern == UIA_RangeValuePatternId && it.role == Access::kSlider);
        if (has) {
            *out = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID prop, VARIANT* v) override {
        v->vt = VT_EMPTY;
        auto text = [&](const std::wstring& s) { v->vt = VT_BSTR, v->bstrVal = Str(s); };
        auto flag = [&](bool b) { v->vt = VT_BOOL, v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; };
        std::lock_guard<std::mutex> guard(shared->lock);
        if (!id) {
            if (prop == UIA_NamePropertyId) text(shared->title);
            else if (prop == UIA_ControlTypePropertyId) v->vt = VT_I4, v->lVal = UIA_PaneControlTypeId;
            else if (prop == UIA_IsKeyboardFocusablePropertyId) flag(true);
            return S_OK;
        }
        const Access::Item* it = FindLocked();
        if (!it) return UIA_E_ELEMENTNOTAVAILABLE;
        switch (prop) {
        case UIA_NamePropertyId: text(it->name); break;
        case UIA_ControlTypePropertyId:
            v->vt = VT_I4;
            v->lVal = it->role == Access::kText ? UIA_TextControlTypeId : it->role == Access::kToggle ? UIA_CheckBoxControlTypeId : it->role == Access::kSlider ? UIA_SliderControlTypeId : UIA_ButtonControlTypeId;
            break;
        case UIA_IsKeyboardFocusablePropertyId: flag(it->role != Access::kText); break;
        case UIA_HasKeyboardFocusPropertyId: flag(id == shared->focus); break;
        case UIA_IsEnabledPropertyId:
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: flag(true); break;
        case UIA_IsOffscreenPropertyId: flag(it->away); break;
        case UIA_AutomationIdPropertyId: text(std::to_wstring(id)); break;
        case UIA_ValueValuePropertyId:
            if (!it->value.empty()) text(it->value);
            break;
        default: break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        *out = nullptr;
        return id ? S_OK : UiaHostProviderFromHwnd(shared->hwnd, out);
    }

    // fragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection d, IRawElementProviderFragment** out) override {
        *out = nullptr;
        std::lock_guard<std::mutex> guard(shared->lock);
        const auto& items = shared->items;
        uint32_t to = 0;
        bool found = false;
        if (!id) {
            if (d == NavigateDirection_FirstChild && !items.empty()) to = items.front().id, found = true;
            else if (d == NavigateDirection_LastChild && !items.empty()) to = items.back().id, found = true;
        } else {
            size_t at = items.size();
            for (size_t i = 0; i < items.size(); ++i)
                if (items[i].id == id) at = i;
            if (d == NavigateDirection_Parent) to = 0, found = true;
            else if (at < items.size() && d == NavigateDirection_NextSibling && at + 1 < items.size()) to = items[at + 1].id, found = true;
            else if (at < items.size() && d == NavigateDirection_PreviousSibling && at > 0) to = items[at - 1].id, found = true;
        }
        if (found) *out = new Node(shared, to);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
        *out = nullptr;
        if (!id) return S_OK; // the window's own comes from its host
        int parts[2] = {UiaAppendRuntimeId, (int)id};
        *out = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*out) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(*out, &i, &parts[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override {
        *r = {};
        if (!id) return S_OK; // its host knows
        std::lock_guard<std::mutex> guard(shared->lock);
        const Access::Item* it = FindLocked();
        if (!it) return UIA_E_ELEMENTNOTAVAILABLE;
        *r = {(double)it->px.left, (double)it->px.top, (double)(it->px.right - it->px.left), (double)(it->px.bottom - it->px.top)};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        if (id) Post(Access::kFocus, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        *out = new Node(shared, 0);
        return S_OK;
    }

    // root
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
        *out = nullptr;
        std::lock_guard<std::mutex> guard(shared->lock);
        for (auto it = shared->items.rbegin(); it != shared->items.rend(); ++it)
            if (x >= it->px.left && x < it->px.right && y >= it->px.top && y < it->px.bottom) {
                *out = new Node(shared, it->id);
                break;
            }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
        *out = nullptr;
        std::lock_guard<std::mutex> guard(shared->lock);
        if (shared->focus) *out = new Node(shared, shared->focus);
        return S_OK;
    }

    // patterns
    HRESULT STDMETHODCALLTYPE Invoke() override { return Post(Access::kFire, 0); }
    HRESULT STDMETHODCALLTYPE Toggle() override { return Post(Access::kFire, 0); }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* s) override {
        Access::Item it;
        if (!Find(it)) return UIA_E_ELEMENTNOTAVAILABLE;
        *s = it.on ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(double v) override { return Post(Access::kSet, (int)(std::clamp(v, 0.0, 100.0) / 100 * kValueScale)); }
    HRESULT STDMETHODCALLTYPE get_Value(double* v) override {
        Access::Item it;
        if (!Find(it)) return UIA_E_ELEMENTNOTAVAILABLE;
        *v = it.num * 100;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* b) override { return *b = FALSE, S_OK; }
    HRESULT STDMETHODCALLTYPE get_Maximum(double* v) override { return *v = 100, S_OK; }
    HRESULT STDMETHODCALLTYPE get_Minimum(double* v) override { return *v = 0, S_OK; }
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* v) override { return *v = kLargeStep, S_OK; }
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* v) override { return *v = kSmallStep, S_OK; }

private:
    const Access::Item* FindLocked() const {
        for (const auto& it : shared->items)
            if (it.id == id) return &it;
        return nullptr;
    }
    bool Find(Access::Item& out) const {
        std::lock_guard<std::mutex> guard(shared->lock);
        const Access::Item* it = FindLocked();
        if (it) out = *it;
        return it != nullptr;
    }
    HRESULT Post(Access::Act act, int value) const {
        return PostMessageW(shared->hwnd, Access::kMsg, id, MAKELPARAM(value, act)) ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    std::atomic<ULONG> refs{1};
    std::shared_ptr<Access::Shared> shared;
    uint32_t id; // 0: the window itself
};

}

Access::Access() : shared(std::make_shared<Shared>()) {}

Access::~Access() { Detach(); }

void Access::Attach(HWND hwnd, const wchar_t* title) {
    std::lock_guard<std::mutex> guard(shared->lock);
    shared->hwnd = hwnd;
    shared->title = title;
}

void Access::Detach() {
    HWND h = shared->hwnd;
    if (h && shared->asked) UiaReturnRawElementProvider(h, 0, 0, nullptr); // a reader holding on is told the window is gone
    std::lock_guard<std::mutex> guard(shared->lock);
    shared->items.clear();
}

void Access::Enable(bool on) { enabled = on; }

bool Access::Listening() const { return enabled && shared->asked && UiaClientsAreListening(); }

bool Access::OnGetObject(WPARAM wp, LPARAM lp, LRESULT& out) {
    if (!enabled || !shared->hwnd || (LONG)lp != UiaRootObjectId) return false;
    if (!shared->asked) {
        shared->asked = true;
        PostMessageW(shared->hwnd, kMsg, 0, MAKELPARAM(0, kNudge)); // a frame, so there's a list to read
    }
    Node* root = new Node(shared, 0);
    out = UiaReturnRawElementProvider(shared->hwnd, wp, lp, static_cast<IRawElementProviderSimple*>(root));
    root->Release();
    return true;
}

void Access::Publish(const Ui& ui, const Canvas& canvas, float pxScale, D2D1_POINT_2F shift, bool controls) {
    if (!Listening()) return;
    POINT at = {0, 0};
    ClientToScreen(shared->hwnd, &at);
    RECT client = {};
    GetClientRect(shared->hwnd, &client);
    auto toScreen = [&](const D2D1_RECT_F& r) {
        return RECT{at.x + (LONG)((r.left - shift.x) * pxScale), at.y + (LONG)((r.top - shift.y) * pxScale), at.x + (LONG)((r.right - shift.x) * pxScale), at.y + (LONG)((r.bottom - shift.y) * pxScale)};
    };
    auto outside = [&](const RECT& p) { return p.left - at.x >= client.right || p.top - at.y >= client.bottom || p.right <= at.x || p.bottom <= at.y; };
    const auto& said = canvas.Words();
    std::vector<char> used(said.size(), 0);
    D2D1::Matrix3x2F base = *D2D1::Matrix3x2F::ReinterpretBaseType(&ui.BaseXf());
    std::vector<Item> items;
    uint32_t focus = 0;
    if (controls) {
        for (const Ui::Item& hit : ui.Items()) {
            if (!hit.stop) continue;
            D2D1_POINT_2F a = base.TransformPoint({hit.r.left, hit.r.top}), b = base.TransformPoint({hit.r.right, hit.r.bottom});
            D2D1_RECT_F r = {a.x, a.y, b.x, b.y};
            Item it;
            it.id = hit.id;
            it.role = hit.role == Ui::Role::Slider ? kSlider : hit.role == Ui::Role::Toggle ? kToggle : kButton;
            it.on = hit.on;
            it.num = hit.value;
            it.value = hit.valueText;
            it.name = hit.name;
            const wchar_t* icon = nullptr;
            for (size_t i = 0; i < said.size(); ++i) { // the words drawn on it, and its icon
                const auto& s = said[i];
                float cx = (s.r.left + s.r.right) / 2, cy = (s.r.top + s.r.bottom) / 2;
                if (cx < r.left || cx >= r.right || cy < r.top || cy >= r.bottom) continue;
                used[i] = 1;
                if (s.glyph) {
                    if (!icon) icon = GlyphWord(s.glyph);
                } else if (hit.name.empty() && hit.role != Ui::Role::Slider) {
                    it.name += (it.name.empty() ? L"" : L", ") + s.text;
                }
            }
            if (it.name.empty() && icon) it.name = icon;
            if (it.name.empty()) { // no words of its own: the label beside it, or else over it
                float best = 1e9f, my = (r.top + r.bottom) / 2;
                int pick = -1;
                for (size_t i = 0; i < said.size(); ++i) {
                    const auto& s = said[i];
                    if (s.glyph) continue;
                    float sy = (s.r.top + s.r.bottom) / 2, gap = -1;
                    if (std::fabs(sy - my) < (r.bottom - r.top) / 2 + kRowSlack && s.r.left < r.left) gap = r.left - s.r.left;
                    else if (s.r.bottom <= r.top + kRowSlack && r.top - s.r.bottom < kAboveMax && s.r.left < r.right && r.left < s.r.right) gap = (r.top - s.r.bottom) + kBesideFirst;
                    if (gap >= 0 && gap < best) best = gap, pick = (int)i;
                }
                if (pick >= 0) it.name = said[pick].text;
            }
            if (it.name.empty()) it.name = L"Button";
            it.px = toScreen(r);
            it.away = outside(it.px);
            if (ui.Focused() == hit.id) focus = hit.id;
            items.push_back(std::move(it));
        }
    }
    constexpr uint32_t kWordMark = 0x80000000u; // a word's id, apart from the controls'
    for (size_t i = 0; i < said.size(); ++i) { // whatever else is written there, to read through
        if (used[i] || said[i].glyph || said[i].text.empty()) continue;
        Item it;
        it.id = kWordMark | (uint32_t)i;
        it.name = said[i].text;
        it.px = toScreen(said[i].r);
        it.away = outside(it.px);
        items.push_back(std::move(it));
    }
    Swap(std::move(items), focus);
}

void Access::Publish(std::vector<Item> items, uint32_t focus) {
    if (Listening()) Swap(std::move(items), focus);
}

void Access::Swap(std::vector<Item> items, uint32_t focus) {
    bool focusMoved, changed = false, count;
    Item was, now;
    {
        std::lock_guard<std::mutex> guard(shared->lock);
        focusMoved = focus != shared->focus;
        count = items.size() != shared->items.size();
        if (focus && !focusMoved) { // the same control, in a new state: said out loud
            auto find = [&](const std::vector<Item>& list) { return std::find_if(list.begin(), list.end(), [&](const Item& i) { return i.id == focus; }); };
            auto a = find(shared->items), b = find(items);
            if (a != shared->items.end() && b != items.end() && (a->on != b->on || a->num != b->num)) was = *a, now = *b, changed = true;
        }
        shared->items = std::move(items);
        shared->focus = focus;
    }
    if (count) {
        Node* root = new Node(shared, 0);
        UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(root), StructureChangeType_ChildrenInvalidated, nullptr, 0);
        root->Release();
    }
    if (!focus) return;
    Node* node = new Node(shared, focus);
    auto* simple = static_cast<IRawElementProviderSimple*>(node);
    if (focusMoved) UiaRaiseAutomationEvent(simple, UIA_AutomationFocusChangedEventId);
    if (changed && now.role == kToggle) {
        VARIANT a, b;
        a.vt = b.vt = VT_I4;
        a.lVal = was.on ? ToggleState_On : ToggleState_Off;
        b.lVal = now.on ? ToggleState_On : ToggleState_Off;
        UiaRaiseAutomationPropertyChangedEvent(simple, UIA_ToggleToggleStatePropertyId, a, b);
    } else if (changed && now.role == kSlider) {
        VARIANT a, b;
        a.vt = b.vt = VT_R8;
        a.dblVal = was.num * 100;
        b.dblVal = now.num * 100;
        UiaRaiseAutomationPropertyChangedEvent(simple, UIA_RangeValueValuePropertyId, a, b);
    }
    node->Release();
}

int Access::Request(WPARAM wp, LPARAM lp, uint32_t& id, float& value) const {
    id = (uint32_t)wp;
    value = LOWORD(lp) / (float)kValueScale;
    return HIWORD(lp);
}

bool Access::Apply(Ui& ui, WPARAM wp, LPARAM lp) {
    uint32_t id = 0;
    float value = 0;
    switch (Request(wp, lp, id, value)) {
    case kFire: ui.Fire(id); break;
    case kFocus: ui.FocusOn(id); break;
    case kSet: ui.SetValue(id, value); break;
    default: break;
    }
    return true;
}
