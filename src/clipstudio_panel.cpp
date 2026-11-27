// The clip studio's effects panel, and what it sets going: speed for a part, a tall or square frame, the sound cleaned and
// evened, quiet stretches cut, a voiceover recorded over the clip, music laid under it and cuts moved onto its beats.
// The sound is heard as it'll be saved: a mix is made off the ui thread whenever it changes, and plays in step with the
// picture while the clip's own sound is muted.
#include "clipstudio.h"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"

namespace audiofx = sys::audiofx;
using clipedit::Piece;

namespace {

constexpr UINT kWorkMsg = WM_APP + 2; // as clipstudio.cpp has it
constexpr float kPad = 18, kBarR = 24, kSegH = 30, kToolH = 34, kGap = 10, kLine = 18, kSmall = 12;
constexpr float kChipH = 30, kChipGap = 8, kRowSwitch = 46, kMeterH = 6;
constexpr float kWheelStep = 48;
constexpr double kMixWait = 0.35; // edits settle this long before the sound is mixed again
constexpr double kMixDrift = 0.2; // the mix put back in step when it's this far out
constexpr double kTakeMin = 0.3; // a take shorter than this heard nothing
constexpr double kTakeSettle = 0.6; // a take this old, with the clip stopped: the clip ended, so does the take
constexpr float kVolMax = 1.5f; // volumes go to 150%
constexpr float kSpeeds[] = {0.25f, 0.5f, 0.75f, 1, 1.5f, 2, 4};
const wchar_t* const kSpeedNames[] = {L"0.25×", L"0.5×", L"0.75×", L"1×", L"1.5×", L"2×", L"4×"};

double Now() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / freq.QuadPart;
}

std::wstring Pct(float v) { return std::to_wstring((int)std::lround(v * 100)) + L"%"; }

}

void ClipStudio::Change() { Commit(); }

float ClipStudio::PanelButton(const Theme& t, const char* id, float x, float y, float w, const wchar_t* label, const wchar_t* tip, bool primary, bool live, bool& hit) {
    hit = Tool(t, id, Rect(x, y, w, kToolH), label, tip, primary, live);
    return y + kToolH + kGap;
}

void ClipStudio::DrawPanel(const Theme& t, const D2D1_RECT_F& r, float a) {
    Canvas& c = *canvas;
    float rise = (1 - a) * 24;
    D2D1_RECT_F p = Rect(r.left + rise, r.top, Width(r), Height(r));
    c.Fill(Rect(p.left, p.top + 4, Width(p), Height(p)), kBarR, Color{0, 0, 0, 0.22f * a});
    c.Fill(p, kBarR, t.cardHi.Alpha(a));
    c.StrokeV(p, kBarR, t.hairTop.Alpha(a), t.hairBot.Alpha(a), 1);
    D2D1_RECT_F in = Inset(p, kPad, kPad);
    static const wchar_t* const tabs[] = {L"Speed", L"Frame", L"Sound", L"Music"};
    int pick = sheet::Segmented(ui, t, "cs.tabs", Rect(in.left, in.top, Width(in), kSegH), tabs, 4, tab, true);
    if (pick >= 0 && pick != tab) tab = pick, panelScroll = 0;
    D2D1_RECT_F body = Rect(in.left, in.top + kSegH + 16, Width(in), in.bottom - in.top - kSegH - 16);
    if (float n = ui.Wheel(body)) panelScroll -= n * kWheelStep, input.wheel = 0; // the panel takes the wheel before its sliders see it: scrolling past one never moves it
    panelScroll = (std::clamp)(panelScroll, 0.0f, (std::max)(0.0f, panelContent - Height(body)));
    float top = body.top - ui.Smooth(Ui::Id("cs.pscroll"), panelScroll, 18);
    c.PushClip(Inset(body, -4, 0));
    D2D1_RECT_F area = Rect(body.left, top, Width(body), Height(body));
    switch (tab) {
    case kTabSpeed: PanelSpeed(t, area); break;
    case kTabFrame: PanelFrame(t, area); break;
    case kTabSound: PanelSound(t, area); break;
    default: PanelMusic(t, area); break;
    }
    c.PopClip();
    if (take && take->Live() && !playing && Now() - takeStarted > kTakeSettle) VoiceToggle(); // the clip ran out: the take ends with it
}

void ClipStudio::PanelSpeed(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = *canvas;
    float x = r.left, w = Width(r), y = r.top;
    int i = clipedit::PieceAt(cut, at);
    if (i < 0) return;
    const Piece& pc = cut.pieces[(size_t)i];
    std::wstring head = cut.pieces.size() == 1 ? L"The whole clip" : L"Part " + std::to_wstring(i + 1) + L" of " + std::to_wstring(cut.pieces.size());
    c.Text(head, Rect(x, y, w, 22), Weight::Semi, 15, t.text);
    y += 22;
    c.Text(StudioClock(pc.from, true) + L" to " + StudioClock(pc.to, true) + (pc.cut ? L"  ·  removed" : L""), Rect(x, y, w, kLine), Weight::Regular, kSmall, pc.cut ? sheet::kDanger : t.textDim);
    y += kLine + 14;
    c.TextSp(L"SPEED", Rect(x, y, w, 14), Weight::Medium, 10, t.textLow, 1.4f);
    y += 20;
    constexpr int kPerRow = 4;
    float cw = (w - (kPerRow - 1) * kChipGap) / kPerRow;
    for (int k = 0; k < (int)std::size(kSpeeds); ++k) {
        D2D1_RECT_F chip = Rect(x + (k % kPerRow) * (cw + kChipGap), y + (k / kPerRow) * (kChipH + kChipGap), cw, kChipH);
        if (w::Chip(ui, t, Ui::Id("cs.speed", k), chip, kSpeedNames[k], pc.speed == kSpeeds[k]) && !busy) {
            cut.pieces[(size_t)i].speed = kSpeeds[k];
            Commit();
        }
        ui.Name(std::wstring(L"Speed ") + kSpeedNames[k]);
        Tip(chip, kSpeeds[k] < 1 ? L"Slow motion: this part plays slower, at the same pitch" : kSpeeds[k] > 1 ? L"This part plays faster, at the same pitch" : L"As it was recorded");
    }
    y += 2 * kChipH + kChipGap + 18;
    bool hit = false;
    float half = (w - kGap) / 2;
    PanelButton(t, "cs.p.split", x, y, half, L"Split here", L"Cuts the timeline in two at the playhead  ·  S", false, len > 0, hit);
    if (hit) SplitHere();
    y = PanelButton(t, "cs.p.remove", x + half + kGap, y, half, pc.cut ? L"Bring it back" : L"Remove part", pc.cut ? L"Puts this part back in the clip  ·  Delete" : L"Leaves this part out of the clip  ·  Delete",
                    false, cut.pieces.size() > 1, hit);
    if (hit) RemoveHere();
    y += 6;
    c.Text(L"Split the timeline to give a part its own speed,", Rect(x, y, w, kLine), Weight::Regular, kSmall, t.textDim);
    c.Text(L"or to leave it out. Click a part to pick it.", Rect(x, y + kLine, w, kLine), Weight::Regular, kSmall, t.textDim);
    y += 2 * kLine;
    panelContent = y - r.top;
}

void ClipStudio::PanelFrame(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = *canvas;
    float x = r.left, w = Width(r), y = r.top;
    bool hit = false;
    auto head = [&](const wchar_t* text) {
        c.TextSp(text, Rect(x, y, w, 14), Weight::Medium, 10, t.textLow, 1.4f);
        y += 20;
    };
    auto line = [&](const wchar_t* text) {
        c.Text(text, Rect(x, y, w, kLine), Weight::Regular, kSmall, t.textDim);
        y += kLine;
    };
    head(L"SHAPE");
    static const wchar_t* const shapes[] = {L"As recorded", L"9:16", L"1:1"};
    int pick = sheet::Segmented(ui, t, "cs.shape", Rect(x, y, w, kSegH), shapes, 3, cut.shape, !busy);
    if (pick >= 0 && pick != cut.shape) cut.shape = pick, Commit();
    y += kSegH + 10;
    const wchar_t* about[3] = {L"The clip keeps the shape it was recorded in.", L"Tall, for YouTube Shorts, Reels and TikTok.", L"Square, for posts."};
    line(about[cut.shape]);
    if (vw && vh) {
        int ow = 0, oh = 0;
        clipedit::OutSize(cut.shape, (int)vw, (int)vh, ow, oh);
        c.Text(L"Saved at " + std::to_wstring(ow) + L" × " + std::to_wstring(oh), Rect(x, y, w, kLine), Weight::Medium, kSmall, t.text);
        y += kLine;
    }
    y += 14;
    if (cut.shape != clipedit::kWide) {
        static const wchar_t* const fits[] = {L"Fill the frame", L"Fit with bars"};
        int fitPick = sheet::Segmented(ui, t, "cs.fit", Rect(x, y, w, kSegH), fits, 2, cut.fit ? 1 : 0, !busy);
        if (fitPick >= 0 && (fitPick == 1) != cut.fit) cut.fit = fitPick == 1, Commit();
        Tip(Rect(x, y, w, kSegH), cut.fit ? L"All of the picture, with black above and below" : L"The picture covers the frame; what's outside is left out");
        y += kSegH + 18;
    }
    head(L"SIZE AND TURN");
    {
        float v = std::log(cut.zoom / clipedit::kMinZoom) / std::log(clipedit::kMaxZoom / clipedit::kMinZoom); // even steps up and down from 100%
        bool released = false;
        if (sheet::Knob(ui, t, Ui::Id("cs.zoom"), Rect(x, y, w, sheet::kSlideH), L"Size", Pct(cut.zoom), v, !busy, released, 0.02f))
            cut.zoom = clipedit::kMinZoom * std::pow(clipedit::kMaxZoom / clipedit::kMinZoom, v), knobBusy = true;
        if (released && knobBusy) knobBusy = false, Commit();
        y += sheet::kSlideH + 6;
    }
    {
        float v = (cut.angle + 180) / 360;
        bool released = false;
        wchar_t deg[16];
        swprintf_s(deg, L"%d°", (int)std::lround(cut.angle));
        if (sheet::Knob(ui, t, Ui::Id("cs.turn"), Rect(x, y, w, sheet::kSlideH), L"Turn", deg, v, !busy, released, 1.0f / 72)) cut.angle = v * 360 - 180, knobBusy = true;
        if (released && knobBusy) knobBusy = false, Commit();
        y += sheet::kSlideH + 10;
    }
    float half = (w - kGap) / 2;
    PanelButton(t, "cs.p.turn90", x, y, half, L"Turn 90°", L"A quarter turn clockwise", false, !busy, hit);
    if (hit) cut.angle = cut.angle + 90 > 180 ? cut.angle - 270 : cut.angle + 90, Commit();
    y = PanelButton(t, "cs.p.place0", x + half + kGap, y, half, L"Reset", L"Back to its size, straight, in the middle", false,
                    !busy && (cut.zoom != 1 || cut.angle != 0 || cut.frameX != 0.5f || cut.frameY != 0.5f), hit);
    if (hit) cut.zoom = 1, cut.angle = 0, cut.frameX = cut.frameY = 0.5f, Commit();
    line(L"Drag the picture to move it. Scroll to resize it,");
    line(L"Shift+scroll to turn it, or use its handles.");
    y += 16;
    head(L"MOVING");
    c.Text(L"Move it over time", Rect(x, y + 2, w - 56, 20), Weight::Medium, 13.5f, t.text);
    c.Text(L"It glides between where you put it", Rect(x, y + 22, w - 56, kLine), Weight::Regular, kSmall, t.textDim);
    if (w::Switch(ui, t, Ui::Id("cs.moving"), {x + w - 20, y + 18}, cut.moving, !busy)) {
        cut.moving = !cut.moving;
        if (cut.moving && cut.spots.empty()) clipedit::SetSpot(cut, at, cut.frameX, cut.frameY); // where it is now becomes the first spot
        Commit();
    }
    ui.Name(L"Move it over time");
    y += kRowSwitch + 4;
    if (cut.moving) {
        int spot = clipedit::SpotNear(cut, at);
        std::wstring count = std::to_wstring(cut.spots.size()) + (cut.spots.size() == 1 ? L" spot" : L" spots");
        c.Text(count + L"  ·  move the playhead, then the picture", Rect(x, y, w, kLine), Weight::Regular, kSmall, t.textDim);
        y += kLine + 10;
        PanelButton(t, "cs.p.spotdel", x, y, half, L"Remove this spot", L"Takes away the spot at the playhead", false, spot >= 0, hit);
        if (hit && spot >= 0) cut.spots.erase(cut.spots.begin() + spot), Commit();
        y = PanelButton(t, "cs.p.spotclr", x + half + kGap, y, half, L"Clear spots", L"The picture stays still again", false, !cut.spots.empty(), hit);
        if (hit) cut.spots.clear(), cut.moving = false, Commit();
    }
    y += 6;
    panelContent = y - r.top;
}

void ClipStudio::PanelSound(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = *canvas;
    float x = r.left, w = Width(r), y = r.top;
    auto toggle = [&](const char* id, const wchar_t* label, const wchar_t* about, bool& field) {
        c.Text(label, Rect(x, y + 2, w - 56, 20), Weight::Medium, 13.5f, t.text);
        c.Text(about, Rect(x, y + 22, w - 56, kLine), Weight::Regular, kSmall, t.textDim);
        if (w::Switch(ui, t, Ui::Id(id), {x + w - 20, y + 18}, field, !busy)) field = !field, Commit();
        ui.Name(label);
        y += kRowSwitch;
    };
    auto knob = [&](const char* id, const wchar_t* label, float& field, float max) {
        float v = field / max;
        bool released = false;
        if (sheet::Knob(ui, t, Ui::Id(id), Rect(x, y, w, sheet::kSlideH), label, Pct(field), v, true, released, 0.05f)) field = v * max, knobBusy = true;
        if (released && knobBusy) knobBusy = false, Commit();
        y += sheet::kSlideH + 8;
    };
    toggle("cs.clean", L"Reduce background noise", L"Takes out fans, hiss and hum", cut.clean);
    if (cut.clean) knob("cs.camt", L"Strength", cut.cleanAmt, 1); // stronger takes more noise, and in the end some of the sound with it
    toggle("cs.even", L"Even out loudness", L"Steady volume, as YouTube and TikTok play it", cut.even);
    y += 4;
    knob("cs.vgame", L"Game sound", cut.game, kVolMax);
    {
        float v = quietAmt;
        bool released = false;
        if (sheet::Knob(ui, t, Ui::Id("cs.qamt"), Rect(x, y, w, sheet::kSlideH), L"Silence detection", quietAmt < 0.34f ? L"Only real silence" : quietAmt < 0.67f ? L"Balanced" : L"Quiet moments too", v, true, released, 0.05f))
            quietAmt = v;
        Tip(Rect(x, y, w, sheet::kSlideH), L"Higher finds fainter, shorter quiet stretches when you cut silent parts");
        y += sheet::kSlideH + 8;
    }
    bool hit = false;
    y = PanelButton(t, "cs.p.quiet", x, y, w, L"Cut silent parts", L"Finds stretches where nothing can be heard, and leaves them out", false, !noSound, hit);
    if (hit) CutQuiet();
    c.Fill(Rect(x, y + 2, w, 1), 0, t.hairTop);
    y += 16;
    c.TextSp(L"VOICEOVER", Rect(x, y, w, 14), Weight::Medium, 10, t.textLow, 1.4f);
    y += 20;
    bool live = take && take->Live();
    y = PanelButton(t, "cs.p.voice", x, y, w, live ? L"Stop recording" : L"Record a voiceover", live ? L"Ends the take and adds it" : L"The clip plays from the playhead while your microphone records", !live, !busy, hit);
    if (hit) VoiceToggle();
    if (live) { // how loud it hears them: so a muted microphone shows straight away
        float lv = ui.Smooth(Ui::Id("cs.meter"), (std::min)(1.0f, take->Level() * 2.5f), 20);
        c.Fill(Rect(x, y - 4, w, kMeterH), kMeterH / 2, t.track);
        c.Fill(Rect(x, y - 4, w * lv, kMeterH), kMeterH / 2, sheet::kDanger);
        y += kMeterH + 8;
        ui.Wake();
    } else {
        c.Text(L"Talk over the clip as it plays from the playhead.", Rect(x, y - 2, w, kLine), Weight::Regular, kSmall, t.textDim);
        y += kLine + 6;
    }
    if (!cut.voices.empty()) {
        std::wstring n = std::to_wstring(cut.voices.size()) + (cut.voices.size() == 1 ? L" voiceover" : L" voiceovers");
        c.Text(n, Rect(x, y + 8, w - 110, kLine), Weight::Medium, 13, t.text);
        PanelButton(t, "cs.p.voff", x + w - 100, y, 100, L"Remove all", L"Takes the voiceovers out", false, !live, hit);
        if (hit) cut.voices.clear(), Commit();
        y += kToolH + kGap;
        knob("cs.vvoice", L"Voice", cut.voice, kVolMax);
    }
    panelContent = y - r.top;
}

void ClipStudio::PanelMusic(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = *canvas;
    float x = r.left, w = Width(r), y = r.top;
    bool hit = false;
    if (cut.song.empty()) {
        y = PanelButton(t, "cs.p.song", x, y, w, songBusy ? L"Reading the song…" : L"Add music", L"Pick a song from your PC to play under the clip", true, !songBusy && !busy, hit);
        if (hit) PickSong();
        c.Text(L"MP3, M4A, WAV, FLAC or anything Windows plays.", Rect(x, y, w, kLine), Weight::Regular, kSmall, t.textDim);
        c.Text(L"It fades out as the clip ends.", Rect(x, y + kLine, w, kLine), Weight::Regular, kSmall, t.textDim);
        panelContent = y + 2 * kLine - r.top;
        return;
    }
    c.Icon(glyph::Music, {x + 12, y + 14}, 20, t.accentHi);
    c.Text(cut.songName, Rect(x + 32, y + 4, w - 32, 20), Weight::Semi, 14, t.text);
    y += 34;
    float half = (w - kGap) / 2;
    PanelButton(t, "cs.p.change", x, y, half, songBusy ? L"Reading…" : L"Change", L"Pick another song", false, !songBusy && !busy, hit);
    if (hit) PickSong();
    y = PanelButton(t, "cs.p.drop", x + half + kGap, y, half, L"Remove", L"Takes the music out", false, !busy, hit);
    if (hit) cut.song.clear(), cut.songName.clear(), Commit();
    float v = cut.music;
    bool released = false;
    if (sheet::Knob(ui, t, Ui::Id("cs.vmusic"), Rect(x, y, w, sheet::kSlideH), L"Music volume", Pct(cut.music), v, true, released, 0.05f)) cut.music = v, knobBusy = true;
    if (released && knobBusy) knobBusy = false, Commit();
    y += sheet::kSlideH + 12;
    c.Text(L"Starts at " + StudioClock(cut.songAt, true) + L" in the finished clip", Rect(x, y, w, kLine), Weight::Regular, kSmall, t.textDim);
    y += kLine + 8;
    y = PanelButton(t, "cs.p.songat", x, y, w, L"Start it at the playhead", L"The music begins where the playhead is", false, !busy, hit);
    if (hit) cut.songAt = clipedit::ToOut(cut, at), Commit();
    y = PanelButton(t, "cs.p.beat", x, y, w, L"Snap cuts to the beat", L"Moves your splits and the clip's end onto the nearest beats of the song", false, !beats.empty() && !busy, hit);
    if (hit) OnBeat();
    c.Text(beats.empty() ? L"No steady beat was found in this song." : L"The beats show as ticks along the timeline.", Rect(x, y, w, kLine), Weight::Regular, kSmall, t.textDim);
    y += kLine;
    panelContent = y - r.top;
}

void ClipStudio::SplitHere() {
    if (busy || len <= 0) return;
    if (clipedit::Split(cut, at)) Commit();
    else Say(L"Too close to a split or an end to split here");
}

void ClipStudio::RemoveHere() {
    int i = clipedit::PieceAt(cut, at);
    if (busy || i < 0) return;
    if (cut.pieces.size() == 1) {
        Say(L"Split the clip first, then remove the part you don't want");
        return;
    }
    Piece& pc = cut.pieces[(size_t)i];
    if (!pc.cut) {
        clipedit::Edit e = cut;
        e.pieces[(size_t)i].cut = true;
        if (clipedit::OutLength(e) < 0.5) {
            Say(L"At least one part has to stay");
            return;
        }
    }
    pc.cut = !pc.cut;
    Commit();
}

void ClipStudio::MixLater() {
    ++mixWant;
    if (!clipedit::SoundWork(cut)) { // the clip's own sound is right as it is
        mixDue = 0;
        mixing = false;
        return;
    }
    mixDue = Now() + kMixWait;
}

void ClipStudio::MixNow() {
    if (!clipedit::SoundWork(cut)) {
        mixDue = 0;
        return;
    }
    if (!gamePcm && !noSound) { // waits for the clip's sound
        LoadSound();
        return;
    }
    mixDue = 0;
    mixBusy = true;
    auto w = work;
    clipedit::Edit e = cut;
    auto own = gamePcm;
    auto song = cut.song.empty() ? nullptr : songPcm;
    int ver = mixWant;
    HWND to = hwnd;
    std::thread([w, e, own, song, ver, to] {
        audiofx::Pcm m = audiofx::Mix(e, own.get(), song.get());
        wchar_t tmp[MAX_PATH];
        std::wstring file;
        if (GetTempPathW(MAX_PATH, tmp)) {
            std::wstring dir = std::wstring(tmp) + L"Astraeus clips";
            CreateDirectoryW(dir.c_str(), nullptr);
            file = dir + L"\\mix " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(ver) + L".wav";
            if (!audiofx::WriteWav(file, m)) file.clear();
        }
        {
            std::lock_guard<std::mutex> lock(w->mu);
            w->mixPath = file, w->mixVer = ver, w->mixDone = true;
        }
        PostMessageW(to, kWorkMsg, 0, 0);
    }).detach();
}

void ClipStudio::SyncMix() {
    bool use = mixing && mixEngine && clipedit::SoundWork(cut);
    if (engine && use != muted) engine->SetMuted(use ? TRUE : FALSE), muted = use; // the mix speaks for the clip
    if (!mixEngine) return;
    if (!use) {
        if (!mixEngine->IsPaused()) mixEngine->Pause();
        return;
    }
    double outAt = clipedit::ToOut(cut, at);
    if (playing) {
        if (mixEngine->IsPaused()) mixEngine->SetCurrentTime(outAt), mixEngine->Play();
        else if (std::fabs(mixEngine->GetCurrentTime() - outAt) > kMixDrift) mixEngine->SetCurrentTime(outAt);
    } else if (!mixEngine->IsPaused()) {
        mixEngine->Pause();
    }
}

void ClipStudio::LoadSound() {
    if (soundAsked || path.empty()) return;
    soundAsked = true;
    auto w = work;
    std::wstring file = path;
    HWND to = hwnd;
    std::thread([w, file, to] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        audiofx::Pcm p;
        std::wstring why;
        bool ok = audiofx::Decode(file, p, why);
        CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(w->mu);
            w->sound = ok ? std::make_shared<const audiofx::Pcm>(std::move(p)) : nullptr;
            w->soundDone = true;
        }
        PostMessageW(to, kWorkMsg, 0, 0);
    }).detach();
}

void ClipStudio::PickSong() {
    if (songBusy || busy) return;
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Music", L"*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac;*.ogg;*.opus"}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, spec);
    d->SetTitle(L"Music for the clip");
    ComPtr<IShellItem> item;
    LPWSTR chosen = nullptr;
    if (FAILED(d->Show(hwnd)) || FAILED(d->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) || !chosen) return;
    std::wstring file = chosen;
    CoTaskMemFree(chosen);
    songBusy = true;
    Poke();
    auto w = work;
    HWND to = hwnd;
    std::thread([w, file, to] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        audiofx::Pcm p;
        std::wstring why;
        bool ok = audiofx::Decode(file, p, why);
        std::vector<double> b = ok ? audiofx::Beats(p) : std::vector<double>{};
        CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(w->mu);
            w->song = ok ? std::make_shared<const audiofx::Pcm>(std::move(p)) : nullptr;
            w->beats = std::move(b), w->songPath = file, w->songWhy = why, w->songDone = true;
        }
        PostMessageW(to, kWorkMsg, 0, 0);
    }).detach();
}

void ClipStudio::CutQuiet() {
    if (busy) return;
    if (!gamePcm) {
        Say(noSound ? L"This clip has no sound to look at" : L"Still reading the clip's sound: try again in a moment");
        LoadSound();
        return;
    }
    clipedit::Edit e = cut;
    int n = 0;
    double secs = 0;
    for (auto [a, b] : audiofx::Quiet(*gamePcm, e.from, e.to, quietAmt)) {
        clipedit::Split(e, a);
        clipedit::Split(e, b);
        int i = clipedit::PieceAt(e, (a + b) / 2);
        if (i < 0) continue;
        Piece& pc = e.pieces[(size_t)i];
        if (!pc.cut && pc.from >= a - 0.25 && pc.to <= b + 0.25) pc.cut = true, ++n, secs += pc.to - pc.from; // only the quiet stretch: a part with sound in it stays
    }
    if (!n) {
        Say(L"No silent parts found");
        return;
    }
    if (clipedit::OutLength(e) < 0.5) {
        Say(L"The whole clip is quiet, so nothing was cut");
        return;
    }
    cut = e;
    Commit();
    Say(L"Left out " + std::to_wstring(n) + (n == 1 ? L" silent part, " : L" silent parts, ") + StudioClock(secs, true) + L". Undo brings them back");
}

void ClipStudio::OnBeat() {
    if (beats.empty() || cut.song.empty() || busy) return;
    auto nearest = [&](double out, bool notAfter) { // the beat closest to that moment of the finished clip
        double best = out, gap = 1e9;
        for (double b : beats) {
            double o = cut.songAt + b;
            if (notAfter && o > out + 1e-6) break;
            if (std::fabs(o - out) < gap) gap = std::fabs(o - out), best = o;
        }
        return best;
    };
    clipedit::Edit e = cut;
    for (size_t i = 0; i + 1 < e.pieces.size(); ++i) { // each split, in order: one moved shifts those after it, so they're placed after it
        Piece &l = e.pieces[i], &rgt = e.pieces[i + 1];
        double s = l.to;
        if (s <= e.from || s >= e.to || l.cut) continue; // a removed part's end doesn't change when anything is heard
        double o = clipedit::ToOut(e, s), ds = (nearest(o, false) - o) * l.speed;
        double ns = (std::clamp)(s + ds, l.from + clipedit::kMinPiece, rgt.to - clipedit::kMinPiece);
        l.to = rgt.from = ns;
    }
    double end = clipedit::OutLength(e), want = nearest(end, true); // the clip ends on a beat, never past what's there
    int last = clipedit::PieceAt(e, e.to - 1e-6);
    float sp = last >= 0 ? e.pieces[(size_t)last].speed : 1.0f;
    if (want > 0.5) e.to = (std::max)(e.from + 0.5, e.to + (want - end) * sp);
    if (e == cut) {
        Say(L"The cuts already land on the beat");
        return;
    }
    cut = e;
    Commit();
    Say(L"Your cuts now land on the beat");
}

void ClipStudio::VoiceToggle() {
    if (!engine) return;
    if (take && take->Live()) {
        auto pcm = take->Stop();
        engine->Pause();
        if (pcm && pcm->size() > (size_t)(kTakeMin * audiofx::kRate * audiofx::kCh)) {
            cut.voices.push_back({takeAt, pcm});
            Commit();
            Say(L"Voiceover added. Play it back to hear it");
        } else {
            Say(L"Nothing was heard. Is the microphone on?");
        }
        return;
    }
    if (busy) return;
    if (!take) take = std::make_unique<sys::voice::Take>();
    if (!take->Start()) {
        Say(L"The microphone couldn't be opened. Check it in Windows' sound settings");
        return;
    }
    if (!clipedit::Kept(cut, at) || at >= cut.to - 0.05) Seek(clipedit::NextKept(cut, at >= cut.to - 0.05 ? cut.from : at));
    takeAt = clipedit::ToOut(cut, at);
    takeStarted = Now();
    engine->Play();
    Say(L"Recording. Press Stop recording when you're done");
}
