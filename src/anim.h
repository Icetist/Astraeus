#pragma once
#include <cmath>

namespace anim {

inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

inline bool Approach(float& value, float target, float rate, float dt, float eps = 0.0005f) {
    value += (target - value) * (1 - std::exp(-rate * dt)); // frame-rate independent exp ease
    if (std::fabs(target - value) < eps) {
        value = target; // snap so callers can stop ticking
        return false;
    }
    return true;
}

struct Spring {
    float value = 0, vel = 0, target = 0;
    float stiff = 320;
    float damp = 0.62f; // damping ratio, <1 gives the drip bounce

    bool Step(float dt) {
        const float sub = 1.0f / 240; // fixed substeps keep it stable on long frames
        const float c = 2 * damp * std::sqrt(stiff); // ratio to real damping coeff
        while (dt > 0) {
            float s = dt < sub ? dt : sub;
            vel += (stiff * (target - value) - c * vel) * s; // hooke pull minus drag
            value += vel * s; // semi-implicit euler
            dt -= s;
        }
        if (std::fabs(target - value) < 0.2f && std::fabs(vel) < 3) { // settled, sub-pixel and slow
            value = target;
            vel = 0;
            return false;
        }
        return true;
    }
};

inline float Bezier(float x1, float y1, float x2, float y2, float x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    auto sample = [](float a, float b, float t) { float u = 1 - t; return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t; };
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 24; ++i) { // bisect curve param t for given x
        if (sample(x1, x2, t) < x) lo = t; else hi = t;
        t = (lo + hi) / 2;
    }
    return sample(y1, y2, t);
}

inline float EaseOut(float t) { return Bezier(0.05f, 0.7f, 0.1f, 1.0f, t); } // fast start, long soft tail

}
