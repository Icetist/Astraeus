// The tools page's alarm tab: two views so both list and time editor fit cleanly in narrow sidebars too.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "alarms.h"
#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "pages.h"

namespace {

constexpr float kFieldH = 30, kRowH = 32, kGap = 8, kXSize = 22;
constexpr Color kWarn = Color{1.0f, 0.72f, 0.30f, 1.0f};

int WrapMod(int v, int mod) { return ((v % mod) + mod) % mod; }

Color Solid(const Theme& t) {
    Color b = t.bg;
    b.a = 1;
    return b.Mix(Color{t.cardHi.r, t.cardHi.g, t.cardHi.b, 1}, 0.55f);
}

struct TimePickerState {
    int mode = 0; // 0 closed, 1 hour (0..23), 2 minute (0..59)
    int spinTarget = 0;
    float spinShown = 0;
    double openedAt = -1;
    int visit = -1;
    bool dragging = false;
    bool dragMoved = false;
    float dragStartY = 0;
    int dragBaseSpin = 0;
    D2D1_RECT_F hhRect{}, mmRect{};

    void Close() {
        mode = 0;
        dragging = false;
        dragMoved = false;
    }

    void Open(int nextMode, int curVal, double now, int pageVisit) {
        mode = nextMode;
        spinTarget = curVal;
        spinShown = (float)curVal;
        openedAt = now;
        visit = pageVisit;
        dragging = false;
        dragMoved = false;
    }
};
TimePickerState gPicker;

std::wstring TwoDigits(int v) {
    wchar_t buf[8]{};
    swprintf_s(buf, L"%02d", v);
    return buf;
}

void DrawDigitSlot(PageCtx& p, uint32_t id, const D2D1_RECT_F& r, int value, const wchar_t* a11y, int& target, int mod, int slotMode) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    bool active = gPicker.mode == slotMode;
    Ui::Hit h = ui.Button(id, r);
    ui.Name(a11y, TwoDigits(value));
    float hov = ui.Smooth(id + 50, h.hover || active ? 1.0f : 0.0f, 16);
    if (hov > 0.01f) c.Fill(r, 12, (active ? t.accent.Alpha(0.14f) : t.text.Alpha(0.06f)).Alpha(hov));
    if (h.hover || active) c.Stroke(r, 12, t.accent.Alpha((active ? 0.65f : 0.35f) * hov), 1);
    c.Text(TwoDigits(value), r, Weight::Thin, 42, t.text, Align::Center);
    if (float notch = ui.Wheel(r); notch != 0) {
        int dir = notch > 0 ? 1 : -1;
        target = WrapMod(target + dir, mod);
    } else if (h.clicked) {
        if (active) gPicker.Close();
        else gPicker.Open(slotMode, target, ui.Now(), m.pageVisit);
    }
}

void LabelField(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    bool typing = m.field == Model::kFieldAlarmLabel;
    Ui::Hit h = ui.Button(Ui::Id("al.label"), r);
    ui.Name(L"Alarm label", typing ? m.fieldEdit : m.alarmLabel);
    float hov = ui.Smooth(Ui::Id("al.label.h"), h.hover || typing ? 1.0f : 0.0f, 16);
    c.Fill(r, Height(r) / 2, t.track.Mix(t.cardHi, 0.3f + 0.3f * hov));
    c.Stroke(r, Height(r) / 2, typing ? t.accent.Alpha(0.8f) : t.hairTop, 1);
    c.Icon(glyph::Edit, {r.left + 16, (r.top + r.bottom) / 2}, 14, typing ? t.accentHi : t.textDim);
    D2D1_RECT_F text = {r.left + 30, r.top, r.right - 12, r.bottom};
    if (typing) {
        bool caret = (int)(ui.Now() * 2) % 2 == 0;
        c.Text(m.fieldEdit + (caret ? L"|" : L""), text, Weight::Regular, 12, t.text);
        ui.KeepAlive();
    } else if (!m.alarmLabel.empty()) {
        c.Text(m.alarmLabel, text, Weight::Regular, 12, t.text);
    } else {
        c.Text(L"Label (optional)", text, Weight::Light, 11.5f, t.textLow);
    }
    if (h.clicked && !typing) m.field = Model::kFieldAlarmLabel, m.fieldEdit = m.alarmLabel;
}

void DrawEditor(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    bool hasAny = !m.alarms.items.empty();
    float w = Width(r);
    float leftW = (std::min)(154.0f, (w - 10) * 0.48f);
    float rightX = r.left + leftW + 10, rightW = r.right - rightX;

    // Left column: clickable HH:MM wheel picker and quick-set chips
    float slotW = (leftW - 14) / 2, slotH = 54, clockY = r.top + 4;
    gPicker.hhRect = Rect(r.left, clockY, slotW, slotH);
    gPicker.mmRect = Rect(r.left + slotW + 14, clockY, slotW, slotH);
    DrawDigitSlot(p, Ui::Id("al.hr"), gPicker.hhRect, m.alarmHour, L"Alarm hour", m.alarmHour, 24, 1);
    c.Text(L":", Rect(r.left + slotW, clockY - 2, 14, slotH), Weight::Thin, 36, t.textDim, Align::Center);
    DrawDigitSlot(p, Ui::Id("al.min"), gPicker.mmRect, m.alarmMin, L"Alarm minute", m.alarmMin, 60, 2);

    struct Preset { const wchar_t* name; int addMin; bool fixed; int fh, fm; };
    const Preset presets[] = {{L"+15m", 15, false, 0, 0}, {L"+1h", 60, false, 0, 0}, {L"09:00", 0, true, 9, 0}};
    float chipY = clockY + slotH + 8, chipGap = 5, cw = (leftW - 2 * chipGap) / 3;
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F cr = Rect(r.left + i * (cw + chipGap), chipY, cw, 28);
        Ui::Hit ch = ui.Button(Ui::Id("al.pre", i), cr);
        ui.Name(presets[i].name);
        float hot = ui.Smooth(Ui::Id("al.pre.h", i), ch.hover ? 1.0f : 0.0f, 16);
        c.Fill(cr, 14, t.track.Mix(t.cardHi, 0.35f + 0.35f * hot));
        c.Stroke(cr, 14, hot > 0.02f ? t.accent.Alpha(0.35f * hot) : t.hairTop, 1);
        c.Text(presets[i].name, Inset(cr, 4, 0), Weight::Medium, 10.5f, t.textDim.Mix(t.text, hot), Align::Center);
        if (ch.clicked) {
            if (presets[i].fixed) {
                m.alarmHour = presets[i].fh;
                m.alarmMin = presets[i].fm;
            } else {
                SYSTEMTIME st{};
                GetLocalTime(&st);
                int mins = (st.wHour * 60 + st.wMinute + presets[i].addMin) % (24 * 60);
                m.alarmHour = mins / 60;
                m.alarmMin = mins % 60;
            }
        }
    }
    if (r.bottom - (chipY + 32) >= 16)
        c.Text(L"Click HH or MM to pick", Rect(r.left, chipY + 32, leftW, 16), Weight::Light, 10.5f, t.textLow, Align::Center);

    // Right column: optional label, one-off/daily toggle, Save/Cancel
    LabelField(p, Rect(rightX, r.top + 4, rightW, kFieldH));
    float repY = r.top + kFieldH + 12, halfW = (rightW - 6) / 2;
    if (w::Chip(ui, t, Ui::Id("al.once"), Rect(rightX, repY, halfW, 28), L"One-off", !m.alarmDaily)) m.alarmDaily = false;
    if (w::Chip(ui, t, Ui::Id("al.daily"), Rect(rightX + halfW + 6, repY, halfW, 28), L"Daily", m.alarmDaily)) m.alarmDaily = true;

    float actY = repY + 36;
    if (hasAny) {
        float cancelW = (std::min)(68.0f, rightW * 0.42f), saveW = rightW - cancelW - 6;
        if (sheet::Act(ui, t, Ui::Id("al.cancel"), Rect(rightX, actY, cancelW, 30), L"Cancel", false, false, true)) m.CancelAlarmEditor();
        if (sheet::Act(ui, t, Ui::Id("al.save"), Rect(rightX + cancelW + 6, actY, saveW, 30), m.alarmEditId > 0 ? L"Save" : L"Set", true, false, true)) m.SaveAlarmEditor();
    } else {
        if (sheet::Act(ui, t, Ui::Id("al.save"), Rect(rightX, actY, rightW, 30), L"Set alarm", true, false, true)) m.SaveAlarmEditor();
    }
}

void DrawList(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const auto& items = m.alarms.items;

    bool hasMissed = m.alarms.HasMissed();
    float barH = 28;
    D2D1_RECT_F bar = Rect(r.left, r.top, Width(r), barH);
    bool canAdd = (int)items.size() < AlarmStore::kMaxAlarms;
    float addW = 72;
    if (w::Chip(ui, t, Ui::Id("al.new"), Rect(bar.right - addW, bar.top, addW, 26), L"+ New", false) && canAdd) {
        m.BeginAlarmEdit(0);
        return;
    }

    if (hasMissed) {
        c.Icon(glyph::Bell, {bar.left + 8, bar.top + 13}, 14, kWarn, true);
        D2D1_RECT_F clearR = Rect(bar.right - addW - 52, bar.top, 46, 26);
        c.Text(L"Missed while asleep", Rect(bar.left + 20, bar.top + 3, clearR.left - (bar.left + 24), 20), Weight::Medium, 11.5f, kWarn);
        Ui::Hit ch = ui.Button(Ui::Id("al.clrmiss"), clearR);
        c.Text(L"Clear", clearR, Weight::Medium, 11, ch.hover ? t.accentHi : t.textDim, Align::Center);
        if (ch.clicked) m.alarms.ClearMissed(), m.SaveAlarms();
    } else {
        int activeCount = (int)std::count_if(items.begin(), items.end(), [](const Alarm& a) { return a.enabled; });
        std::wstring info = activeCount == 0 ? L"All off"
                          : activeCount == 1 ? L"1 active alarm"
                                             : std::to_wstring(activeCount) + L" active alarms";
        c.Text(info, Rect(bar.left + 2, bar.top + 3, bar.right - addW - bar.left - 8, 20), Weight::Light, 11.5f, t.textDim);
    }

    D2D1_RECT_F view = {r.left, bar.bottom + 6, r.right, r.bottom};
    static float target = 0;
    float content = items.size() * kRowH, off = sheet::Scroll(ui, "ls.alarm", target, view, content);
    c.PushClip(view);
    int removeId = 0, toggleId = 0, editId = 0;
    for (int i = 0; i < (int)items.size(); ++i) {
        const Alarm& a = items[(size_t)i];
        float y = view.top + i * kRowH - off;
        if (y + kRowH < view.top || y > view.bottom) continue;
        D2D1_RECT_F row = Rect(view.left, y, Width(view), kRowH);
        float swX = row.right - kXSize - 30, cy = (row.top + row.bottom) / 2;
        Ui::Hit rh = ui.Button(Ui::Id("al.row", a.id), {row.left, row.top, swX - 24, row.bottom});
        ui.Name(AlarmStore::FormatTime(a.hour, a.minute) + (a.label.empty() ? L"" : L" " + a.label), a.enabled ? L"On" : L"Off");
        float hov = ui.Smooth(Ui::Id("al.row.h", a.id), rh.hover ? 1.0f : 0.0f, 16);
        if (hov > 0.01f) c.Fill(row, 10, t.text.Alpha(0.05f * hov));

        std::wstring timeStr = AlarmStore::FormatTime(a.hour, a.minute);
        Color timeInk = a.enabled ? t.text : t.textLow;
        c.Text(timeStr, Rect(row.left + 6, row.top, 50, kRowH), Weight::Medium, 13.5f, timeInk);

        std::wstring sub = a.label;
        std::wstring tag = a.missed ? L"Missed" : a.snoozeUntil > 0 ? L"Snoozed" : a.daily ? L"Daily" : L"";
        if (!tag.empty()) {
            if (!sub.empty()) sub += L"  \u00B7  ";
            sub += tag;
        }
        Color subInk = a.missed ? kWarn : a.snoozeUntil > 0 ? t.accentHi : a.enabled ? t.textDim : t.textLow;
        c.Text(sub.empty() ? L"Alarm" : sub, Rect(row.left + 56, row.top, swX - 28 - (row.left + 56), kRowH), Weight::Regular, 12, sub.empty() ? t.textLow : subInk);

        if (rh.clicked) editId = a.id;
        if (w::Switch(ui, t, Ui::Id("al.sw", a.id), {swX, cy}, a.enabled)) toggleId = a.id;
        if (w::IconBtn(ui, t, Ui::Id("al.x", a.id), {row.right - kXSize / 2 - 2, cy}, kXSize, glyph::Close, 13)) removeId = a.id;
    }
    c.PopClip();
    sheet::Fades(c, t, view, off, content);

    if (editId > 0) m.BeginAlarmEdit(editId);
    if (toggleId > 0 && m.alarms.Toggle(toggleId)) m.SaveAlarms(), m.SaveAlarmSeen();
    if (removeId > 0 && m.alarms.Remove(removeId)) m.SaveAlarms(), m.SaveAlarmSeen();
}

} // namespace

bool AlarmPickerOpen(const Model& m, const D2D1_RECT_F&) {
    bool editing = m.alarmEditId >= 0 || m.alarms.items.empty();
    if (gPicker.mode != 0 && (!editing || m.toolTab != Model::kTabAlarm || gPicker.visit != m.pageVisit || m.field == Model::kFieldAlarmLabel))
        gPicker.Close();
    return gPicker.mode != 0;
}

void DrawAlarmPickerOverlay(PageCtx& p) {
    if (gPicker.mode == 0) return;
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const Input& in = ui.In();

    int& target = gPicker.mode == 1 ? m.alarmHour : m.alarmMin;
    int mod = gPicker.mode == 1 ? 24 : 60;
    D2D1_RECT_F anchor = gPicker.mode == 1 ? gPicker.hhRect : gPicker.mmRect;

    constexpr float kItemH = 28, kStepH = 22, kPadY = 4;
    int rowsVis = Height(p.area) >= 200 ? 5 : 3;
    float drumH = rowsVis * kItemH;
    float boxW = (std::max)(Width(anchor) + 16.0f, 76.0f);
    float boxH = 2 * kStepH + drumH + 2 * kPadY;

    float cx = (anchor.left + anchor.right) / 2, cy = (anchor.top + anchor.bottom) / 2;
    float x = (std::clamp)(cx - boxW / 2, p.area.left + 2.0f, (std::max)(p.area.left + 2.0f, p.area.right - boxW - 2.0f));
    float y = (std::clamp)(cy - boxH / 2, p.area.top + 2.0f, (std::max)(p.area.top + 2.0f, p.area.bottom - boxH - 2.0f));

    float openK = ui.SmoothFrom(Ui::Id("al.pk.in", gPicker.mode), 1.0f, 0.0f, 24);
    D2D1_RECT_F box = Rect(x, y + (1 - openK) * 6, boxW, boxH);
    c.Fill(Rect(box.left, box.top + 4, boxW, boxH), 14, Color{0, 0, 0, 0.24f * openK});
    c.Fill(box, 14, Solid(t).Alpha(openK));
    c.StrokeV(box, 14, t.hairTop.Mix(t.accent.Alpha(0.55f), 0.4f).Alpha(openK), t.hairBot.Alpha(openK), 1);

    D2D1_RECT_F upR = Rect(box.left + 4, box.top + kPadY, boxW - 8, kStepH);
    D2D1_RECT_F drum = Rect(box.left, upR.bottom, boxW, drumH);
    D2D1_RECT_F dnR = Rect(box.left + 4, drum.bottom, boxW - 8, kStepH);

    auto drawStep = [&](uint32_t bid, const D2D1_RECT_F& sr, int glyph, const wchar_t* label, int delta) {
        Ui::Hit sh = ui.Button(bid, sr);
        ui.Name(label);
        float hv = ui.Smooth(bid + 10, sh.hover ? 1.0f : 0.0f, 20);
        if (hv > 0.01f) c.Fill(sr, 8, t.track.Alpha(0.65f * hv * openK));
        c.Icon(glyph, {(sr.left + sr.right) / 2, (sr.top + sr.bottom) / 2}, 15, t.textDim.Mix(t.accentHi, hv).Alpha(openK));
        if (sh.clicked) {
            gPicker.spinTarget += delta;
            target = WrapMod(gPicker.spinTarget, mod);
        }
    };
    drawStep(Ui::Id("al.pk.up"), upR, glyph::Up, gPicker.mode == 1 ? L"Previous hour" : L"Previous minute", -1);
    drawStep(Ui::Id("al.pk.dn"), dnR, glyph::Down, gPicker.mode == 1 ? L"Next hour" : L"Next minute", 1);

    if (float notch = ui.Wheel(drum); notch != 0) {
        gPicker.spinTarget += notch > 0 ? -1 : 1;
        target = WrapMod(gPicker.spinTarget, mod);
    }

    Ui::Hit dh = ui.Button(Ui::Id("al.pk.drum"), drum);
    ui.Name(gPicker.mode == 1 ? L"Hour wheel" : L"Minute wheel", TwoDigits(target));
    float drumCy = (drum.top + drum.bottom) / 2;
    if (dh.down) {
        if (!gPicker.dragging) {
            gPicker.dragging = true;
            gPicker.dragMoved = false;
            gPicker.dragStartY = in.mouse.y;
            gPicker.dragBaseSpin = gPicker.spinTarget;
        }
        if (std::fabs(in.mouse.y - in.pressPos.y) >= 4.0f) gPicker.dragMoved = true;
        if (gPicker.dragMoved) {
            float deltaRows = (gPicker.dragStartY - in.mouse.y) / kItemH;
            gPicker.spinShown = (float)gPicker.dragBaseSpin + deltaRows;
            gPicker.spinTarget = gPicker.dragBaseSpin + (int)std::lround(deltaRows);
            target = WrapMod(gPicker.spinTarget, mod);
        }
    } else if (gPicker.dragging) {
        bool wasMoved = gPicker.dragMoved;
        gPicker.dragging = false;
        gPicker.dragMoved = false;
        if (!wasMoved && dh.clicked) {
            int rowOff = (int)std::lround((in.mouse.y - drumCy) / kItemH);
            if (rowOff == 0) {
                gPicker.Close();
                return;
            }
            gPicker.spinTarget += rowOff;
            target = WrapMod(gPicker.spinTarget, mod);
        }
    } else if (std::fabs(gPicker.spinShown - (float)gPicker.spinTarget) > 0.004f) {
        anim::Approach(gPicker.spinShown, (float)gPicker.spinTarget, 22.0f, ui.Dt(), 0.002f);
        ui.Wake();
    } else {
        gPicker.spinShown = (float)gPicker.spinTarget;
    }

    // Center Apple-style selection pill
    D2D1_RECT_F band = Rect(box.left + 6, drumCy - kItemH / 2, boxW - 12, kItemH);
    c.Fill(band, 8, t.accent.Alpha(0.16f * openK));
    c.Stroke(band, 8, t.accent.Alpha(0.35f * openK), 1);

    int hoverRow = dh.hover && !gPicker.dragging ? (int)std::lround((in.mouse.y - drumCy) / kItemH) : 99;
    c.PushClip(drum);
    int baseIdx = (int)std::floor(gPicker.spinShown + 0.5f);
    float frac = gPicker.spinShown - baseIdx;
    int halfSpan = rowsVis / 2 + 1;
    for (int k = -halfSpan; k <= halfSpan; ++k) {
        float offset = (float)k - frac;
        float dist = std::fabs(offset);
        if (dist > rowsVis * 0.5f + 0.55f) continue;
        int val = WrapMod(baseIdx + k, mod);
        float nearCenter = anim::Clamp01(1.0f - dist);
        float fade = anim::Clamp01(1.0f - (std::max)(0.0f, dist - 0.65f) / (rowsVis * 0.5f));
        float rowY = drumCy + offset * kItemH - kItemH / 2;
        if (k == hoverRow && k != 0) c.Fill(Rect(box.left + 8, rowY + 2, boxW - 16, kItemH - 4), 7, t.text.Alpha(0.06f * openK));
        float size = 12.5f + 6.0f * nearCenter;
        Color col = t.textLow.Mix(t.textDim, anim::Clamp01(2.0f - dist)).Mix(t.text, nearCenter).Alpha(fade * openK);
        Weight wt = dist < 0.45f ? Weight::Medium : dist < 1.35f ? Weight::Regular : Weight::Light;
        c.Text(TwoDigits(val), Rect(box.left + 4, rowY, boxW - 8, kItemH), wt, size, col, Align::Center);
    }
    c.PopClip();

    if ((in.pressed || ui.RClick(p.area)) && !Inside(box, in.mouse) && ui.Now() != gPicker.openedAt) {
        if (in.pressed && Inside(gPicker.hhRect, in.mouse)) {
            if (gPicker.mode == 1) gPicker.Close();
            else gPicker.Open(1, m.alarmHour, ui.Now(), m.pageVisit);
        } else if (in.pressed && Inside(gPicker.mmRect, in.mouse)) {
            if (gPicker.mode == 2) gPicker.Close();
            else gPicker.Open(2, m.alarmMin, ui.Now(), m.pageVisit);
        } else {
            gPicker.Close();
        }
    }
}

void AlarmTab(PageCtx& p, const D2D1_RECT_F& r) {
    if (p.m.alarms.items.empty() && p.m.alarmEditId < 0) p.m.BeginAlarmEdit(0);
    if (p.m.alarmEditId >= 0) DrawEditor(p, r);
    else DrawList(p, r);
}
