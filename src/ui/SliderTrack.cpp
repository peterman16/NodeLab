#include "ui/SliderTrack.h"

#include <algorithm>
#include <cmath>

#include "core/ColorMath.h"

namespace slidertrack {

namespace {

void lerp3(const float* a, const float* b, float t, float* out) {
    for (int k = 0; k < 3; ++k) out[k] = a[k] + (b[k] - a[k]) * t;
}

// Three stops: s = -1 at the left end, 0 (no change, grey) in the middle, +1 at the right.
void bipolar(const float* lo, const float* hi, float s, float* out) {
    static const float kNeutral[3] = {0.62f, 0.62f, 0.62f};
    if (s < 0) lerp3(kNeutral, lo, -s, out);
    else lerp3(kNeutral, hi, s, out);
}

void hsv(float hueDeg, float s, float v, float* out) {
    const float h = std::fmod(std::fmod(hueDeg, 360.0f) + 360.0f, 360.0f) / 360.0f;
    colormath::hsvToRgb(h, s, v, out[0], out[1], out[2]);
}

ImU32 toU32(const float* c, ImU32 base) {
    // Halfway toward the field colour: full-strength yellows and greens would drown white text.
    const ImVec4 b = ImGui::ColorConvertU32ToFloat4(base);
    const float bb[3] = {b.x, b.y, b.z};
    float m[3];
    lerp3(bb, c, 0.5f, m);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(m[0], m[1], m[2], 1.0f));
}

}  // namespace

void colorAt(const ParamDesc& d, float t, float rgb[3]) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float v = d.min + (d.max - d.min) * t;
    // Signed position for ranges centred on "no change" (-100..100, -180..180).
    const float reach = std::max(std::fabs(d.min), std::fabs(d.max));
    const float s = reach > 0 ? std::clamp(v / reach, -1.0f, 1.0f) : 0.0f;
    switch (d.track) {
        case SliderTrack::Temperature: {
            static const float kCool[3] = {0.30f, 0.52f, 1.00f}, kWarm[3] = {1.00f, 0.80f, 0.22f};
            bipolar(kCool, kWarm, s, rgb);
            return;
        }
        case SliderTrack::Tint: {
            static const float kGreen[3] = {0.30f, 0.82f, 0.30f}, kMagenta[3] = {0.88f, 0.30f, 0.88f};
            bipolar(kGreen, kMagenta, s, rgb);
            return;
        }
        case SliderTrack::Kelvin: colormath::blackbodyToRgb(std::max(v, 100.0f), rgb[0], rgb[1], rgb[2]); return;
        case SliderTrack::Hue: hsv(v, 0.85f, 0.95f, rgb); return;
        case SliderTrack::HueShift: hsv(d.trackHue + s * d.trackSpan, 0.85f, 0.95f, rgb); return;
        case SliderTrack::Saturation: hsv(d.trackHue, 0.45f + 0.45f * s, 0.9f, rgb); return;
        case SliderTrack::Luminance: hsv(d.trackHue, 0.75f, 0.55f + 0.4f * s, rgb); return;
        case SliderTrack::None: break;
    }
    rgb[0] = rgb[1] = rgb[2] = 0.5f;
}

void draw(ImDrawList* dl, ImVec2 a, ImVec2 b, const ParamDesc& d, ImU32 base, float rounding) {
    const int kSeg = 32;
    const float w = b.x - a.x;
    if (w <= 0 || b.y <= a.y) return;
    ImU32 col[kSeg + 1];
    for (int i = 0; i <= kSeg; ++i) {
        float c[3];
        colorAt(d, float(i) / kSeg, c);
        col[i] = toU32(c, base);
    }
    // The end segments are plain rounded rects (multi-colour rects can't round their corners);
    // they're narrow enough that their single colour doesn't show.
    const float r = std::min(rounding, w / kSeg);
    for (int i = 0; i < kSeg; ++i) {
        const ImVec2 p0(a.x + w * i / kSeg, a.y), p1(a.x + w * (i + 1) / kSeg, b.y);
        if (r > 0 && i == 0) dl->AddRectFilled(p0, p1, col[0], r, ImDrawFlags_RoundCornersLeft);
        else if (r > 0 && i == kSeg - 1) dl->AddRectFilled(p0, p1, col[kSeg], r, ImDrawFlags_RoundCornersRight);
        else dl->AddRectFilledMultiColor(p0, p1, col[i], col[i + 1], col[i + 1], col[i]);
    }
}

}  // namespace slidertrack
