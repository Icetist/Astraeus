// A clip made again from an edit. The source is decoded to nv12 by windows; each frame of the finished clip asks the edit
// which moment of the source it shows (so a slowed part repeats frames, a sped one skips them, a removed one is never
// asked for), takes the frame's crop of that picture, scales it, and hands it to the hardware encoder. The finished sound,
// already mixed on the finished clip's own timeline, goes in alongside, a frame's worth at a time.
#include "sys_render.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <execution>
#include <mutex>
#include <numeric>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace sys::render {
namespace {

constexpr LONGLONG kSec = 10000000;
constexpr double kBitsPerPixel = 0.1; // per pixel per frame: sharp for game footage at any size
constexpr double kMinBits = 3e6, kMaxBits = 25e6;
constexpr UINT kAudioBytes = 24000; // aac at 192 kbit/s
constexpr size_t kAudioChunk = 1024; // frames per sound sample written
constexpr double kSeekGap = 2.5; // further ahead than this, the reader seeks instead of decoding its way there
constexpr int kBands = 8; // the picture scaled in this many strips at once
constexpr BYTE kBlackY = 16, kBlackUV = 128; // video black, as nv12 has it

void StartMf() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

struct Plane {
    const BYTE* px;
    int stride, w, h;
};

void Affine(const Plane& s, const float m[6], int sub, BYTE* dst, int dstride, int ow, int oh, int comps, BYTE black) { // each output pixel from where m says, bilinear; sub 2: the half-size colour plane
    std::vector<int> bands(kBands);
    std::iota(bands.begin(), bands.end(), 0);
    std::for_each(std::execution::par, bands.begin(), bands.end(), [&](int b) {
        int y0 = oh * b / kBands, y1 = oh * (b + 1) / kBands;
        for (int y = y0; y < y1; ++y) {
            BYTE* out = dst + (size_t)y * dstride;
            float v = sub * (y + 0.5f);
            for (int x = 0; x < ow; ++x) {
                float u = sub * (x + 0.5f);
                float fx = (m[0] * u + m[1] * v + m[2]) / sub - 0.5f, fy = (m[3] * u + m[4] * v + m[5]) / sub - 0.5f;
                if (fx < -1 || fy < -1 || fx > s.w || fy > s.h) { // outside the picture: black
                    for (int c = 0; c < comps; ++c) out[x * comps + c] = black;
                    continue;
                }
                fx = (std::clamp)(fx, 0.0f, (float)(s.w - 1)), fy = (std::clamp)(fy, 0.0f, (float)(s.h - 1));
                int ix = (int)fx, iy = (int)fy, ix2 = (std::min)(ix + 1, s.w - 1), iy2 = (std::min)(iy + 1, s.h - 1);
                float wx = fx - ix, wy = fy - iy;
                const BYTE *r0 = s.px + (size_t)iy * s.stride, *r1 = s.px + (size_t)iy2 * s.stride;
                for (int c = 0; c < comps; ++c) {
                    float top = r0[ix * comps + c] * (1 - wx) + r0[ix2 * comps + c] * wx, bot = r1[ix * comps + c] * (1 - wx) + r1[ix2 * comps + c] * wx;
                    out[x * comps + c] = (BYTE)(top * (1 - wy) + bot * wy + 0.5f);
                }
            }
        }
    });
}

bool Picture(IMFSample* sample, int w, int h, int stride, const float m[6], int ow, int oh, std::vector<BYTE>& out) {
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(sample->ConvertToContiguousBuffer(&b))) return false;
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(b->Lock(&data, nullptr, &len))) return false;
    if ((size_t)len < (size_t)stride * h * 3 / 2) {
        b->Unlock();
        return false;
    }
    int rows = (std::max)(h, (int)(len / (DWORD)stride * 2 / 3)); // a decoder may pad the height: the colour plane starts after the padding
    out.resize((size_t)ow * oh * 3 / 2);
    constexpr float kEps = 1e-4f;
    bool same = ow == w && oh == h && std::fabs(m[0] - 1) < kEps && std::fabs(m[4] - 1) < kEps && std::fabs(m[1]) < kEps && std::fabs(m[3]) < kEps && std::fabs(m[2]) < kEps && std::fabs(m[5]) < kEps;
    if (same) { // as recorded: rows copied as they are
        for (int y = 0; y < h; ++y) std::memcpy(&out[(size_t)y * ow], data + (size_t)y * stride, (size_t)w);
        for (int y = 0; y < h / 2; ++y) std::memcpy(&out[(size_t)ow * oh + (size_t)y * ow], data + (size_t)stride * rows + (size_t)y * stride, (size_t)w);
    } else { // moved, scaled, turned or framed: every pixel from where it now comes from
        Affine({data, stride, w, h}, m, 1, out.data(), ow, ow, oh, 1, kBlackY);
        Affine({data + (size_t)stride * rows, stride, w / 2, h / 2}, m, 2, out.data() + (size_t)ow * oh, ow, ow / 2, oh / 2, 2, kBlackUV);
    }
    b->Unlock();
    return true;
}

bool WriteBytes(IMFSinkWriter* sw, DWORD stream, const void* bytes, size_t n, LONGLONG at, LONGLONG dur) {
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFSample> s;
    BYTE* dst = nullptr;
    if (FAILED(MFCreateMemoryBuffer((DWORD)n, &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
    std::memcpy(dst, bytes, n);
    buf->Unlock();
    buf->SetCurrentLength((DWORD)n);
    return SUCCEEDED(MFCreateSample(&s)) && SUCCEEDED(s->AddBuffer(buf.Get())) && SUCCEEDED(s->SetSampleTime(at)) && SUCCEEDED(s->SetSampleDuration(dur)) &&
           SUCCEEDED(sw->WriteSample(stream, s.Get()));
}

}

bool Render(const std::wstring& src, const clipedit::Edit& e, const audiofx::Pcm& sound, const std::wstring& out, std::atomic<int>* percent, std::wstring& why) {
    StartMf();
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 1);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE); // decoded and handed over as nv12
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(src.c_str(), ra.Get(), &r))) return why = L"The clip couldn't be opened", false;
    ComPtr<IMFMediaType> native, nv12, got;
    UINT32 w = 0, h = 0, fn = 60, fd = 1;
    if (FAILED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native)) || FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h)
        return why = L"The clip has no picture", false;
    MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fn, &fd);
    if (!fn || !fd) fn = 60, fd = 1;
    w &= ~1u, h &= ~1u;
    MFCreateMediaType(&nv12);
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, w, h);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get())) || FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got)))
        return why = L"The clip couldn't be decoded", false;
    UINT32 strideU = MFGetAttributeUINT32(got.Get(), MF_MT_DEFAULT_STRIDE, w);
    int stride = (int)strideU > 0 ? (int)strideU : (int)w;
    int ow = 0, oh = 0;
    clipedit::OutSize(e.shape, (int)w, (int)h, ow, oh);
    float place[6];
    double fps = (double)fn / fd;
    LONGLONG frameDur = (LONGLONG)(kSec * (double)fd / fn);
    ComPtr<IMFAttributes> wa;
    MFCreateAttributes(&wa, 2);
    wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); // the graphics card encodes
    wa->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> sw;
    if (FAILED(MFCreateSinkWriterFromURL(out.c_str(), nullptr, wa.Get(), &sw))) return why = L"The new clip couldn't be written", false;
    ComPtr<IMFMediaType> h264, in, aac, pcm;
    MFCreateMediaType(&h264);
    h264->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    h264->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    h264->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)(std::clamp)((double)ow * oh * fps * kBitsPerPixel, kMinBits, kMaxBits));
    h264->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(h264.Get(), MF_MT_FRAME_SIZE, (UINT32)ow, (UINT32)oh);
    MFSetAttributeRatio(h264.Get(), MF_MT_FRAME_RATE, fn, fd);
    MFSetAttributeRatio(h264.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)ow);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, (UINT32)ow, (UINT32)oh);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fn, fd);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD vs = 0, as = 0;
    if (FAILED(sw->AddStream(h264.Get(), &vs)) || FAILED(sw->SetInputMediaType(vs, in.Get(), nullptr))) return why = L"No encoder took the clip", false;
    bool hasSound = !sound.empty();
    if (hasSound) {
        MFCreateMediaType(&aac);
        aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audiofx::kRate);
        aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audiofx::kCh);
        aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioBytes);
        MFCreateMediaType(&pcm);
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audiofx::kRate);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audiofx::kCh);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audiofx::kCh * 2);
        pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audiofx::kRate * audiofx::kCh * 2);
        hasSound = SUCCEEDED(sw->AddStream(aac.Get(), &as)) && SUCCEEDED(sw->SetInputMediaType(as, pcm.Get(), nullptr));
    }
    if (FAILED(sw->BeginWriting())) return why = L"The new clip couldn't be started", false;
    auto seek = [&](double s) {
        PROPVARIANT pos;
        InitPropVariantFromInt64((LONGLONG)((std::max)(s, 0.0) * kSec), &pos);
        r->SetCurrentPosition(GUID_NULL, pos); // lands on the keyframe before: decoded forward from there
        PropVariantClear(&pos);
    };
    seek(e.from);
    ComPtr<IMFSample> cur, next;
    LONGLONG curAt = -1, nextAt = -1;
    bool ended = false;
    auto readNext = [&] {
        while (!next && !ended) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) ended = true;
            else if (s) next = s, nextAt = ts;
        }
    };
    double outLen = clipedit::OutLength(e);
    size_t total = (size_t)std::floor(outLen * fps), soundFrames = sound.size() / audiofx::kCh, soundAt = 0;
    std::vector<BYTE> frame;
    std::vector<int16_t> chunk;
    for (size_t k = 0; k < total; ++k) {
        double s = clipedit::ToSrc(e, k / fps);
        LONGLONG want = (LONGLONG)(s * kSec);
        if (cur && (double)(want - curAt) / kSec > kSeekGap) { // a long removed part: jump over it, not through it
            seek(s);
            cur.Reset(), next.Reset(), ended = false;
        }
        for (;;) { // the latest picture at or before that moment
            readNext();
            if (next && (nextAt <= want || !cur)) {
                cur = next, curAt = nextAt;
                next.Reset();
                continue;
            }
            break;
        }
        if (!cur) break;
        clipedit::Place(e, s, (int)w, (int)h, ow, oh, place); // a moving frame is where it is at this moment
        if (!Picture(cur.Get(), (int)w, (int)h, stride, place, ow, oh, frame)) return why = L"A picture in the clip couldn't be read", false;
        LONGLONG at = (LONGLONG)k * frameDur;
        if (!WriteBytes(sw.Get(), vs, frame.data(), frame.size(), at, frameDur)) return why = L"The disk is full or the clip couldn't be written", false;
        size_t soundTo = (std::min)(soundFrames, (size_t)((double)(k + 1) / fps * audiofx::kRate));
        while (hasSound && soundAt < soundTo) { // the sound up to the end of this frame, so the two stay interleaved
            size_t n = (std::min)(kAudioChunk, soundFrames - soundAt);
            chunk.resize(n * audiofx::kCh);
            for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = (int16_t)std::lround((std::clamp)(sound[soundAt * audiofx::kCh + i], -1.0f, 1.0f) * 32767);
            LONGLONG sat = (LONGLONG)soundAt * kSec / audiofx::kRate, sdur = (LONGLONG)n * kSec / audiofx::kRate;
            if (!WriteBytes(sw.Get(), as, chunk.data(), chunk.size() * sizeof(int16_t), sat, sdur)) return why = L"The clip's sound couldn't be written", false;
            soundAt += n;
        }
        if (percent) *percent = (int)(100 * (k + 1) / (std::max)(total, (size_t)1));
    }
    if (FAILED(sw->Finalize())) return why = L"The new clip couldn't be finished", false;
    return true;
}

}
