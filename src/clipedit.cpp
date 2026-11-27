// The studio's edit as arithmetic: where a moment of the clip lands in the finished one and back, what's kept, and the
// frame a tall or square picture takes out of the recording.
#include "clipedit.h"

#include <algorithm>
#include <cmath>

namespace clipedit {
namespace {

constexpr double kSame = 1e-6;

bool Near(double a, double b) { return std::fabs(a - b) < kSame; }

template <class F> void EachKept(const Edit& e, F f) { // every kept stretch inside the trim, in order: f(from, to, speed) returns false to stop
    for (const Piece& p : e.pieces) {
        if (p.cut) continue;
        double a = (std::max)(p.from, e.from), b = (std::min)(p.to, e.to);
        if (b - a <= kSame) continue;
        if (!f(a, b, p.speed)) return;
    }
}

}

bool Edit::operator==(const Edit& o) const {
    if (!Near(from, o.from) || !Near(to, o.to) || shape != o.shape || frameX != o.frameX || frameY != o.frameY || clean != o.clean || even != o.even || game != o.game ||
        voice != o.voice || music != o.music || song != o.song || !Near(songAt, o.songAt) || pieces.size() != o.pieces.size() || voices.size() != o.voices.size() ||
        fit != o.fit || moving != o.moving || spots.size() != o.spots.size() || cleanAmt != o.cleanAmt)
        return false;
    for (size_t i = 0; i < spots.size(); ++i)
        if (!Near(spots[i].at, o.spots[i].at) || spots[i].x != o.spots[i].x || spots[i].y != o.spots[i].y) return false;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece &a = pieces[i], &b = o.pieces[i];
        if (!Near(a.from, b.from) || !Near(a.to, b.to) || a.speed != b.speed || a.cut != b.cut) return false;
    }
    for (size_t i = 0; i < voices.size(); ++i)
        if (!Near(voices[i].at, o.voices[i].at) || voices[i].pcm != o.voices[i].pcm) return false;
    return true;
}

void Init(Edit& e, double len) {
    e = Edit{};
    e.to = len;
    e.pieces.push_back({0, len, 1, false});
}

int PieceAt(const Edit& e, double src) {
    for (size_t i = 0; i < e.pieces.size(); ++i)
        if (src < e.pieces[i].to || i + 1 == e.pieces.size()) return (int)i;
    return -1;
}

bool Split(Edit& e, double src) {
    int i = PieceAt(e, src);
    if (i < 0) return false;
    Piece p = e.pieces[(size_t)i];
    if (src - p.from < kMinPiece || p.to - src < kMinPiece) return false;
    Piece right = p;
    p.to = right.from = src;
    e.pieces[(size_t)i] = p;
    e.pieces.insert(e.pieces.begin() + i + 1, right);
    return true;
}

double OutLength(const Edit& e) {
    double out = 0;
    EachKept(e, [&](double a, double b, float s) { return out += (b - a) / s, true; });
    return out;
}

double ToOut(const Edit& e, double src) {
    double out = 0;
    EachKept(e, [&](double a, double b, float s) {
        if (src <= a) return false;
        out += ((std::min)(src, b) - a) / s;
        return src > b;
    });
    return out;
}

double ToSrc(const Edit& e, double out, float* speed) {
    double acc = 0, src = e.to;
    float sp = 1;
    bool found = false;
    EachKept(e, [&](double a, double b, float s) {
        double d = (b - a) / s;
        sp = s;
        if (out <= acc + d) {
            src = a + (std::max)(out - acc, 0.0) * s, found = true;
            return false;
        }
        acc += d;
        return true;
    });
    if (speed) *speed = found ? sp : 1;
    return src;
}

bool Kept(const Edit& e, double src) {
    if (src < e.from - kSame || src >= e.to) return false;
    int i = PieceAt(e, src);
    return i >= 0 && !e.pieces[(size_t)i].cut;
}

double NextKept(const Edit& e, double src) {
    src = (std::max)(src, e.from);
    for (int guard = 0; guard <= (int)e.pieces.size() && src < e.to; ++guard) {
        int i = PieceAt(e, src);
        if (i < 0 || !e.pieces[(size_t)i].cut) return src;
        src = e.pieces[(size_t)i].to;
    }
    return e.to;
}

float Aspect(int shape, float srcAspect) { return shape == kTall ? 9.0f / 16 : shape == kSquare ? 1.0f : srcAspect; }

void OutSize(int shape, int w, int h, int& ow, int& oh) {
    float src = (float)w / (std::max)(h, 1), a = Aspect(shape, src);
    if (a < src) oh = h, ow = (int)std::lround(h * a);
    else ow = w, oh = (int)std::lround(w / a);
    ow &= ~1, oh &= ~1; // the encoder takes even sizes
}

void FrameAt(const Edit& e, double src, float& x, float& y) {
    x = e.frameX, y = e.frameY;
    if (!e.moving || e.spots.empty()) return;
    const std::vector<Spot>& s = e.spots;
    if (src <= s.front().at) {
        x = s.front().x, y = s.front().y;
        return;
    }
    if (src >= s.back().at) {
        x = s.back().x, y = s.back().y;
        return;
    }
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (src > s[i + 1].at) continue;
        double span = s[i + 1].at - s[i].at;
        float k = span > kSame ? (float)((src - s[i].at) / span) : 1.0f;
        k = k * k * (3 - 2 * k); // eased: it sets off and arrives gently, as a camera operator would
        x = s[i].x + (s[i + 1].x - s[i].x) * k, y = s[i].y + (s[i + 1].y - s[i].y) * k;
        return;
    }
}

int SpotNear(const Edit& e, double src) {
    constexpr double kNear = 0.15; // seconds: a drag this close to a spot moves it, not a new one
    for (size_t i = 0; i < e.spots.size(); ++i)
        if (std::fabs(e.spots[i].at - src) < kNear) return (int)i;
    return -1;
}

void SetSpot(Edit& e, double src, float x, float y) {
    int i = SpotNear(e, src);
    if (i >= 0) {
        e.spots[(size_t)i].x = x, e.spots[(size_t)i].y = y;
        return;
    }
    auto at = std::lower_bound(e.spots.begin(), e.spots.end(), src, [](const Spot& s, double t) { return s.at < t; });
    e.spots.insert(at, Spot{src, x, y});
}

bool Cropping(const Edit& e) { return e.shape != kWide && !e.fit; }
bool Boxed(const Edit& e) { return e.shape != kWide && e.fit; }

void Crop(const Edit& e, double src, int w, int h, float& x, float& y, float& cw, float& ch) {
    if (!Cropping(e)) { // the whole picture
        x = y = 0, cw = (float)w, ch = (float)h;
        return;
    }
    int ow = 0, oh = 0;
    OutSize(e.shape, w, h, ow, oh);
    cw = (float)ow, ch = (float)oh;
    float fx = 0.5f, fy = 0.5f;
    FrameAt(e, src, fx, fy);
    x = (std::clamp)(fx * w - cw / 2, 0.0f, w - cw);
    y = (std::clamp)(fy * h - ch / 2, 0.0f, h - ch);
}

bool SoundWork(const Edit& e) { return e.clean || e.even || e.game != 1 || !e.song.empty() || !e.voices.empty(); }

bool Reworked(const Edit& e) {
    if (e.shape != kWide || SoundWork(e)) return true;
    bool more = false;
    EachKept(e, [&](double, double, float s) { return !(more = s != 1); });
    for (const Piece& p : e.pieces)
        if (p.cut && p.to > e.from && p.from < e.to) more = true;
    return more;
}

bool Trimmed(const Edit& e, double len) { return e.from > 0.05 || e.to < len - 0.05; }

}
