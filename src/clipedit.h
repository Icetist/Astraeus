#pragma once
#include <memory>
#include <string>
#include <vector>

// What the clip studio does to a clip, kept apart from how it's shown or made. The clip's own time is "source" time; the
// finished clip's is "out" time, which starts at the trim's start, leaves out removed parts and runs faster or slower
// through parts with a speed. Plain data and arithmetic, cheap to copy, so undo keeps whole copies.
namespace clipedit {

struct Piece { // a stretch of the clip, between two splits
    double from = 0, to = 0;
    float speed = 1;
    bool cut = false; // left out of the finished clip
};

enum Shape : int { kWide, kTall, kSquare }; // as recorded, 9:16 for shorts and reels, 1:1

struct Voice { // a voiceover, recorded against the finished clip
    double at = 0; // out seconds
    std::shared_ptr<const std::vector<float>> pcm; // 48 khz stereo, interleaved
};

struct Spot { // where a moving frame is at a moment of the clip
    double at = 0;
    float x = 0.5f, y = 0.5f;
};

struct Edit {
    double from = 0, to = 0; // the trim, source seconds
    std::vector<Piece> pieces; // the whole clip end to end, in order
    int shape = kWide;
    float frameX = 0.5f, frameY = 0.5f; // a tall or square frame's middle across the picture, 0..1
    bool fit = false; // the whole picture inside the frame, black above and below, instead of a crop
    float zoom = 1, angle = 0; // the picture made bigger or smaller, and turned by this many degrees, inside the frame
    bool moving = false; // the frame follows spots set along the clip
    std::vector<Spot> spots; // by time
    bool clean = false, even = false; // less background noise; one steady loudness
    float cleanAmt = 0.5f; // how hard the noise is taken down, 0..1
    float game = 1, voice = 1, music = 0.6f; // volumes
    std::wstring song, songName; // music under it: the file, and what it's shown as
    double songAt = 0; // out seconds where the music starts
    std::vector<Voice> voices;
    bool operator==(const Edit& o) const;
};

constexpr double kMinPiece = 0.2; // a split never makes a part shorter than this

void Init(Edit& e, double len); // the whole clip, nothing done to it
int PieceAt(const Edit& e, double src);
bool Split(Edit& e, double src); // false: too near a split or an end
double OutLength(const Edit& e);
double ToOut(const Edit& e, double src); // in a removed part: where that part would have been
double ToSrc(const Edit& e, double out, float* speed = nullptr);
bool Kept(const Edit& e, double src); // inside the trim and not removed
double NextKept(const Edit& e, double src); // the first kept moment at or after src; to when there's none
float Aspect(int shape, float srcAspect); // width over height of the finished picture
void OutSize(int shape, int w, int h, int& ow, int& oh); // even sizes for the encoder
constexpr float kMinZoom = 0.25f, kMaxZoom = 4;
void Place(const Edit& e, double src, int w, int h, int ow, int oh, float m[6]); // where each pixel of the finished frame comes from in the picture: sx = m0 u + m1 v + m2, sy = m3 u + m4 v + m5
bool Transformed(const Edit& e); // the picture is moved, scaled, turned or framed differently from how it was recorded
void FrameAt(const Edit& e, double src, float& x, float& y); // the frame's middle then: eased between spots
void SetSpot(Edit& e, double src, float x, float y); // a spot there, or the one already near it moved
int SpotNear(const Edit& e, double src); // -1: none close
bool Cropping(const Edit& e); // tall or square, filled: part of the picture is left out
bool Boxed(const Edit& e); // tall or square, fitted: black bars
bool Reworked(const Edit& e); // more than a trim: the clip has to be made again, not copied
bool SoundWork(const Edit& e); // the sound differs from the clip's own: it's mixed, and heard from the mix
bool Trimmed(const Edit& e, double len);

}
