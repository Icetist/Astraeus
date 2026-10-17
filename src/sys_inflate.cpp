// A small inflate for zlib streams (the deflate format, rfc 1951), after Mark Adler's puff: Windows ships no zlib.
#include "inflate.h"

namespace sys::detail {
namespace {

struct Bits { // deflate packs bits least significant first
    const uint8_t* p;
    size_t n, i = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool bad = false;

    int Need(int k) {
        uint32_t v = buf;
        while (cnt < k) {
            if (i >= n) {
                bad = true; // ran off the end: malformed
                return 0;
            }
            v |= (uint32_t)p[i++] << cnt;
            cnt += 8;
        }
        buf = k == 32 ? 0 : v >> k;
        cnt -= k;
        return (int)(v & ((1u << k) - 1));
    }
};

struct Huff {
    short count[16]; // codes of each length
    short symbol[288]; // symbols ordered by code
};

bool Build(Huff& h, const short* len, int n) {
    for (short& c : h.count) c = 0;
    for (int s = 0; s < n; ++s) h.count[len[s]]++;
    if (h.count[0] == n) return true; // no codes: fine as long as none is used
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left = (left << 1) - h.count[l];
        if (left < 0) return false; // oversubscribed
    }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = (short)(offs[l] + h.count[l]);
    for (int s = 0; s < n; ++s)
        if (len[s]) h.symbol[offs[len[s]]++] = (short)s;
    return true;
}

int Decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        code |= b.Need(1);
        if (b.bad) return -1;
        int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Bits& b, std::string& out, const Huff& lit, const Huff& dist, size_t maxOut) {
    for (;;) {
        int sym = Decode(b, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= maxOut) return false;
            out.push_back((char)sym);
        } else if (sym == 256) {
            return true; // end of block
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym] + b.Need(kLenExtra[sym]);
            int ds = Decode(b, dist);
            if (ds < 0 || ds >= 30) return false;
            size_t back = (size_t)kDistBase[ds] + b.Need(kDistExtra[ds]);
            if (b.bad || back > out.size() || out.size() + len > maxOut) return false;
            size_t from = out.size() - back;
            for (int k = 0; k < len; ++k) out.push_back(out[from + k]); // may overlap itself, byte by byte on purpose
        }
    }
}

bool Stored(Bits& b, std::string& out, size_t maxOut) {
    b.buf = 0; // to the byte boundary
    b.cnt = 0;
    if (b.i + 4 > b.n) return false;
    unsigned len = b.p[b.i] | (b.p[b.i + 1] << 8), nlen = b.p[b.i + 2] | (b.p[b.i + 3] << 8);
    b.i += 4;
    if (len != (~nlen & 0xffff) || b.i + len > b.n || out.size() + len > maxOut) return false;
    out.append(reinterpret_cast<const char*>(b.p + b.i), len);
    b.i += len;
    return true;
}

struct FixedCodes {
    Huff lit, dist;
    FixedCodes() {
        short len[288];
        for (int s = 0; s < 288; ++s) len[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
        Build(lit, len, 288);
        for (int s = 0; s < 30; ++s) len[s] = 5;
        Build(dist, len, 30);
    }
};

bool Fixed(Bits& b, std::string& out, size_t maxOut) {
    static const FixedCodes codes; // built once, safely, whichever thread gets here first
    return Codes(b, out, codes.lit, codes.dist, maxOut);
}

bool Dynamic(Bits& b, std::string& out, size_t maxOut) {
    static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = b.Need(5) + 257, ndist = b.Need(5) + 1, ncode = b.Need(4) + 4;
    if (b.bad || nlen > 286 || ndist > 30) return false;
    short len[320] = {};
    for (int k = 0; k < ncode; ++k) len[kOrder[k]] = (short)b.Need(3);
    Huff lens, lit, dist;
    if (b.bad || !Build(lens, len, 19)) return false;
    for (int k = 0; k < nlen + ndist;) {
        int sym = Decode(b, lens);
        if (sym < 0) return false;
        if (sym < 16) {
            len[k++] = (short)sym;
            continue;
        }
        short v = 0;
        int rep;
        if (sym == 16) {
            if (k == 0) return false;
            v = len[k - 1];
            rep = 3 + b.Need(2);
        } else {
            rep = sym == 17 ? 3 + b.Need(3) : 11 + b.Need(7);
        }
        if (b.bad || k + rep > nlen + ndist) return false;
        while (rep--) len[k++] = v;
    }
    if (len[256] == 0) return false; // no end-of-block code
    if (!Build(lit, len, nlen) || !Build(dist, len + nlen, ndist)) return false;
    return Codes(b, out, lit, dist, maxOut);
}

}

bool Unbase64(const std::string& in, std::string& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\r' || ch == '\n') continue;
        int v = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
              : ch == '+' ? 62 : ch == '/' ? 63 : -1;
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((acc >> bits) & 0xff));
        }
    }
    return true;
}

bool Unzlib(const uint8_t* in, size_t n, std::string& out, size_t maxOut) {
    out.clear();
    if (n < 2 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return false; // deflate, the header's own check, no preset dictionary
    Bits b{in + 2, n - 2};
    for (int last = 0; !last;) {
        last = b.Need(1);
        int type = b.Need(2);
        if (b.bad) return false;
        bool ok = type == 0 ? Stored(b, out, maxOut) : type == 1 ? Fixed(b, out, maxOut) : type == 2 ? Dynamic(b, out, maxOut) : false;
        if (!ok) return false;
    }
    return true;
}

}
