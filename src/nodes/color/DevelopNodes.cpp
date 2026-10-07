// Photo "develop" nodes modelled on Lightroom's panels: Basic, Color Mixer (HSL) and Color
// Grading. Sliders use Lightroom's -100..100 scale so numbers carry over.
//
// Scene-linear projects use darktable-style maths on linear light: CAT16 white balance, a tone
// equalizer for Highlights/Shadows (gains per exposure band, driven by an edge-aware luminance
// mask), contrast and local contrast as ratios in log space, and colour work in Oklch. Values are
// unbounded until the view transform, so Highlights can recover what Exposure pushed past white.
// Legacy projects keep the original display-referred maths (and its 0..1 clamp) unchanged.
#include <atomic>
#include <array>
#include <cmath>

#include "core/ColorMath.h"
#include "core/ColorScience.h"
#include "gpu/Blur.h"
#include "gpu/PointOp.h"
#include "gpu/Reduce.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace colormath;

namespace {

float smooth(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float luma(const float* p) { return luminance(p[0], p[1], p[2]); }

// Edge-preserving smoothing (He et al.'s guided filter, guided by the image itself): flat areas
// are blurred, while edges with more contrast than about sqrt(eps) are kept. Clarity uses it as
// its base so strong edges (a tree against the sky) don't get halos.
std::vector<float> guidedSmooth(const std::vector<float>& I, int w, int h, float sigma, float eps) {
    const size_t n = I.size();
    std::vector<float> mean = I, sq(n);
    for (size_t i = 0; i < n; ++i) sq[i] = I[i] * I[i];
    imageops::blurChannel(mean, w, h, sigma, sigma);
    imageops::blurChannel(sq, w, h, sigma, sigma);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; ++i) {
        const float var = std::max(sq[i] - mean[i] * mean[i], 0.0f);
        a[i] = var / (var + eps);
        b[i] = mean[i] - a[i] * mean[i];
    }
    imageops::blurChannel(a, w, h, sigma, sigma);
    imageops::blurChannel(b, w, h, sigma, sigma);
    for (size_t i = 0; i < n; ++i) mean[i] = a[i] * I[i] + b[i];
    return mean;
}

// He & Sun's fast guided filter: the same result as guidedSmooth, but the linear coefficients a, b
// are solved at 1/s resolution and upsampled before applying them to the full-resolution guide, so
// edges stay sharp while the cost drops by about s^2. s is chosen so the filter radius stays at
// least ~4 low-res pixels (the coefficients are smooth at that scale).
// (ox, oy): where the buffer sits in the full image. The low-res grid is anchored to the full
// image, so a region (see RoiWindow) gets the same coefficients as the whole image.
int guidedScale(float sigma) { return std::clamp(int(sigma / 4.0f), 1, 8); }

std::vector<float> fastGuidedSmooth(const std::vector<float>& I, int w, int h, float sigma, float eps, int ox = 0,
                                    int oy = 0) {
    const int s = guidedScale(sigma);
    if (s == 1) return guidedSmooth(I, w, h, sigma, eps);
    // Low-res cells overlapping the buffer, from the one holding its first pixel.
    const int cx0 = ox / s, cy0 = oy / s;
    const int lw = (ox + w + s - 1) / s - cx0, lh = (oy + h + s - 1) / s - cy0;
    const size_t ln = size_t(lw) * lh;
    std::vector<float> mean(ln), sq(ln);
    parallelFor(lh, [&](int ly) {
        for (int lx = 0; lx < lw; ++lx) {
            float sum = 0, sum2 = 0;
            int cnt = 0;
            const int ys = std::max((ly + cy0) * s - oy, 0), xs = std::max((lx + cx0) * s - ox, 0);
            for (int y = ys; y < std::min((ly + cy0 + 1) * s - oy, h); ++y)
                for (int x = xs; x < std::min((lx + cx0 + 1) * s - ox, w); ++x) {
                    const float v = I[size_t(y) * w + x];
                    sum += v, sum2 += v * v, ++cnt;
                }
            mean[size_t(ly) * lw + lx] = sum / cnt;
            sq[size_t(ly) * lw + lx] = sum2 / cnt;
        }
    });
    const float ls = sigma / s;
    imageops::blurChannel(mean, lw, lh, ls, ls);
    imageops::blurChannel(sq, lw, lh, ls, ls);
    std::vector<float> a(ln), b(ln);
    for (size_t i = 0; i < ln; ++i) {
        const float var = std::max(sq[i] - mean[i] * mean[i], 0.0f);
        a[i] = var / (var + eps);
        b[i] = mean[i] - a[i] * mean[i];
    }
    imageops::blurChannel(a, lw, lh, ls, ls);
    imageops::blurChannel(b, lw, lh, ls, ls);
    std::vector<float> q(I.size());
    const float inv = 1.0f / s;
    parallelFor(h, [&](int y) {
        const float ly = (y + oy + 0.5f) * inv - cy0;
        for (int x = 0; x < w; ++x) {
            const float lx = (x + ox + 0.5f) * inv - cx0;
            const size_t i = size_t(y) * w + x;
            q[i] = imageops::sampleBilinear(a, lw, lh, lx, ly) * I[i] + imageops::sampleBilinear(b, lw, lh, lx, ly);
        }
    });
    return q;
}

// ---------------------------------------------------------------- scene-linear helpers

constexpr float kMidGreyEv = -2.4739312f;  // log2(0.18)

// Exposure of a luminance in stops relative to 1.0 (white in the Standard view).
float evOf(float y) { return std::log2(std::max(y, 1.0f / 65536.0f)); }

// How far (pixels) fastGuidedSmooth reads from each pixel: two blurs in a row, at 1/s resolution,
// plus the cells and their interpolation.
int guidedReach(float sigma) {
    const int s = guidedScale(sigma);
    if (s == 1) return 2 * imageops::blurReach(sigma) + 2;
    return s * (2 * imageops::blurReach(sigma / s) + 3) + 2;
}

std::vector<float> logLuminance(const Image& img) {
    std::vector<float> ev(size_t(img.w) * img.h);
    parallelFor(img.h, [&](int y) {
        for (int x = 0; x < img.w; ++x) ev[size_t(y) * img.w + x] = evOf(luma(img.pixel(size_t(y) * img.w + x)));
    });
    return ev;
}

// True when some colour value is negative or NaN (what finishLinear would change).
bool anyBelowZero(const Image& img) {
    std::atomic<bool> found{false};
    parallelFor(img.h, [&](int y) {
        if (found.load(std::memory_order_relaxed)) return;
        const float* p = img.pixel(size_t(y) * img.w);
        for (int x = 0; x < img.w; ++x, p += 4)
            if (!(p[0] >= 0.0f && p[1] >= 0.0f && p[2] >= 0.0f)) {
                found = true;
                return;
            }
    });
    return found;
}

// Colours come out of the Oklch edits slightly outside the RGB gamut; linear projects keep values
// above 1, so only negatives need fixing.
void finishLinear(Image& img) {
    parallelFor(img.h, [&](int y) {
        for (int x = 0; x < img.w; ++x) colorsci::compressToGamut(img.pixel(size_t(y) * img.w + x));
    });
}

// tanh through one exp2: std::tanh is about three times slower, and Contrast calls it per pixel.
// Within 1e-7 of it, which is far below what a gain in stops can show.
inline float fastTanh(float x) { return 1.0f - 2.0f / (std::exp2(x * 2.88539008f) + 1.0f); }

// Linear projects blend by Factor in log2(x + kFactorFloor): a mask at 0.5 gives half the
// adjustment's stops (an Exposure of -2 EV becomes -1 EV), as Lightroom scales the slider amounts
// by its mask. Mixing linear light instead gave -0.68 EV there, so gradients seemed to do little
// until near their full end. The floor keeps blacks (and lifts of them) blending smoothly.
constexpr float kFactorFloor = 1.0f / 256.0f;

inline float factorBlend(float s, float d, float f, bool logBlend) {
    if (!logBlend || !(s > -kFactorFloor && d > -kFactorFloor)) return s + (d - s) * f;
    // Fully in or out of the mask (most of a gradient's pixels): the adjusted or the source value
    // exactly, without a log and an exp per channel.
    if (f >= 1.0f) return d;
    if (f <= 0.0f) return s;
    const float a = s + kFactorFloor, b = d + kFactorFloor;
    return a * std::exp2(f * std::log2(b / a)) - kFactorFloor;
}

// Blends the adjusted image back over the source by a Factor channel (Lightroom's masks plug in
// here). Legacy projects mix sRGB-encoded values, already perceptual, so they keep the plain mix.
void applyFactor(const Node& node, const Image& src, Image& img, const Value& facIn, bool logBlend = false) {
    ChannelPtr fac = channelOr(facIn, 1.0f);
    if (fac->constant && fac->value >= 1.0f) return;
    ChannelSampler sf = paramSampler(node, 1, fac, src.w, src.h);
    parallelFor(src.h, [&](int y) {
        for (int x = 0; x < src.w; ++x) {
            size_t i = size_t(y) * src.w + x;
            float f = sf(x, y);
            float* d = img.pixel(i);
            if (logBlend && f >= 1.0f) continue;  // the adjusted value, as factorBlend gives
            const float* s = src.pixel(i);
            for (int k = 0; k < 3; ++k) d[k] = factorBlend(s[k], d[k], f, logBlend);
        }
    });
}

// The GPU versions of smooth() and applyFactor's blend.
const char* const kGlslDevelop = R"(
float smoothT(float e0, float e1, float x) {
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
const float kFactorFloor = 1.0 / 256.0;
vec4 applyFactor(vec4 s, vec3 d, float f, bool logBlend) {
    vec3 mixed = s.rgb + (d - s.rgb) * f;
    if (!logBlend) return vec4(mixed, s.a);
    if (f >= 1.0) return vec4(d, s.a);  // as the CPU's factorBlend
    if (f <= 0.0) return s;
    vec3 a = s.rgb + kFactorFloor, b = d + kFactorFloor;
    vec3 l = a * exp2(f * log2(max(b, 1e-30) / max(a, 1e-30))) - kFactorFloor;
    vec3 ok = vec3(greaterThan(a, vec3(0.0))) * vec3(greaterThan(b, vec3(0.0)));
    return vec4(mix(mixed, l, ok), s.a);
}
)";

// ---------------------------------------------------------------- GPU local filters

gpu::TexturePtr textureOf(const Value& v) {
    if (auto i = std::get_if<GpuImagePtr>(&v.v); i && *i) return (*i)->texture();
    if (auto c = std::get_if<GpuChannelPtr>(&v.v); c && *c) return (*c)->texture();
    throw gpu::Error("GPU: expected pixels on the device");
}
Value imageValue(gpu::TexturePtr t) {
    auto r = std::make_shared<GpuImage>();
    r->w = t->w(), r->h = t->h(), r->tex = std::move(t);
    return Value(GpuImagePtr(r));
}
Value channelValue(gpu::TexturePtr t) {
    auto r = std::make_shared<GpuChannel>();
    r->w = t->w(), r->h = t->h(), r->tex = std::move(t);
    return Value(GpuChannelPtr(r));
}

// guidedSmooth (fast = false) or fastGuidedSmooth of the w x h channel I on the device, with the
// same passes: means of I and I^2 (over s x s cells for the fast one), blurred together as one
// RGBA texture; the coefficients a, b, blurred; then a * I + b (a, b sampled bilinearly from the
// cells). (ox, oy): where the buffer sits in the full image; the cells are anchored to it, as in
// fastGuidedSmooth, so a region gets the whole image's coefficients.
Value guidedGpu(EvalContext& ctx, const Value& I, int w, int h, float sigma, float eps, bool fast, int ox, int oy) {
    const int s = fast ? guidedScale(sigma) : 1;
    const int cx0 = ox / s, cy0 = oy / s;
    gpu::PointOp m;
    m.w = (ox + w + s - 1) / s - cx0, m.h = (oy + h + s - 1) / s - cy0;
    m.full = true;
    if (s == 1) {
        m.body = "    float v = ch0(p);\n    out0 = vec4(v, v * v, 0.0, 1.0);\n";
    } else {
        m.gather = {0};
        m.params = {float(s), float(ox), float(oy), float(cx0), float(cy0)};
        m.body = R"(
    int S = int(P[0]), OX = int(P[1]), OY = int(P[2]), CX = int(P[3]), CY = int(P[4]);
    float sum = 0.0, sum2 = 0.0;
    int cnt = 0;
    for (int y = max((p.y + CY) * S - OY, 0); y < min((p.y + CY + 1) * S - OY, size0.y); ++y)
        for (int x = max((p.x + CX) * S - OX, 0); x < min((p.x + CX + 1) * S - OX, size0.x); ++x) {
            float v = fetchCh0(ivec2(x, y));
            sum += v, sum2 += v * v, ++cnt;
        }
    out0 = vec4(sum / float(cnt), sum2 / float(cnt), 0.0, 1.0);
)";
    }
    const float ls = sigma / s;
    const Value means = imageValue(gpu::boxBlur(textureOf(gpu::runPass(ctx, m, {I}, {true})[0]), ls, ls));
    gpu::PointOp c;
    c.w = m.w, c.h = m.h;
    c.full = true;
    c.params = {eps};
    c.body = R"(
    vec4 m = img0(p);
    float var = max(m.g - m.r * m.r, 0.0);
    float a = var / (var + P[0]);
    out0 = vec4(a, m.r - a * m.r, 0.0, 1.0);
)";
    const Value ab = imageValue(gpu::boxBlur(textureOf(gpu::runPass(ctx, c, {means}, {true})[0]), ls, ls));
    gpu::PointOp q;
    q.w = w, q.h = h;
    if (s == 1) {
        q.body = "    vec4 c = img1(p);\n    out0 = c.r * ch0(p) + c.g;\n";
    } else {
        q.gather = {1};
        q.params = {1.0f / s, float(ox), float(oy), float(cx0), float(cy0)};
        q.body = R"(
    precise vec2 l = (vec2(p) + vec2(P[1], P[2]) + 0.5) * P[0] - vec2(P[3], P[4]);
    vec4 c = bilinear1(l, false);
    out0 = c.r * ch0(p) + c.g;
)";
    }
    return gpu::runPass(ctx, q, {I, ab}, {false})[0];
}

// The sum of every pixel of a GPU image, read back: `first` reduces 16 x 16 blocks of its inputs
// (gathered) to one RGBA sum each, then plain block sums run down to one pixel.
std::array<float, 4> sumGpu(EvalContext& ctx, gpu::PointOp first, const std::vector<Value>& in, int w, int h) {
    first.w = (w + 15) / 16, first.h = (h + 15) / 16;
    first.full = true;
    first.inlinable = false;
    Value cur = gpu::runPass(ctx, first, in, {true})[0];
    int cw = first.w, ch = first.h;
    while (cw > 1 || ch > 1) {
        gpu::PointOp b;
        b.w = (cw + 15) / 16, b.h = (ch + 15) / 16;
        b.full = true;
        b.inlinable = false;
        b.gather = {0};
        b.body = R"(
    vec4 acc = vec4(0.0);
    for (int y = p.y * 16; y < min(p.y * 16 + 16, size0.y); ++y)
        for (int x = p.x * 16; x < min(p.x * 16 + 16, size0.x); ++x) acc += fetch0(ivec2(x, y));
    out0 = acc;
)";
        cur = gpu::runPass(ctx, b, {cur}, {true})[0];
        cw = b.w, ch = b.h;
    }
    const ImagePtr px = gpu::download(*std::get<GpuImagePtr>(cur.v));
    const float* v = px->pixel(0);
    return {v[0], v[1], v[2], v[3]};
}

// ---------------------------------------------------------------- Basic

// Contrast S-curve on 0..1 through (0.5, 0.5): p > 1 steepens the middle, p < 1 flattens it.
float sCurve(float x, float p) {
    float xc = std::clamp(x, 0.0f, 1.0f);
    float y = xc < 0.5f ? 0.5f * std::pow(2.0f * xc, p) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * xc, p);
    return y + (x - xc);  // values outside 0..1 (before Highlights recovers them) pass through
}

struct ToneParams {
    float contrast, highlights, shadows, whites, blacks;  // -1..1
};

// Highlights, Shadows, Whites and Blacks as one curve on luminance, in Lightroom's order.
float toneCurve(float v, const ToneParams& t) {
    if (t.highlights != 0.0f) {
        float w = smooth(0.35f, 1.0f, v);
        // Negative compresses proportionally, so values above 1 come back into range; positive
        // brightens but tapers off before it would clip everything.
        if (t.highlights < 0) v += t.highlights * 0.22f * w * std::max(v, 0.0f);
        else v += t.highlights * 0.2f * w * std::clamp(1.25f - v, 0.0f, 1.0f);
    }
    if (t.shadows != 0.0f) {
        float w = 1.0f - smooth(0.0f, 0.6f, v);
        // Positive lifts dark areas but leaves pure black alone.
        if (t.shadows > 0) v += t.shadows * 0.18f * w * smooth(0.0f, 0.2f, v);
        else v += t.shadows * 0.4f * w * std::max(v, 0.0f);
    }
    if (t.whites != 0.0f) {
        const float wp = 1.0f - 0.2f * t.whites;  // white point: < 1 clips earlier
        v += (v / wp - v) * smooth(0.4f, 1.0f, v);
    }
    if (t.blacks != 0.0f) {
        const float bp = -0.08f * t.blacks;  // black point: > 0 crushes, < 0 lifts
        v += ((v - bp) / (1.0f - bp) - v) * (1.0f - smooth(0.0f, 0.5f, v));
    }
    return v;
}

class BasicNode : public Node {
public:
    NODELAB_NODE({"color.basic", "Basic", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Temperature", 0.0f, -100.0f, 100.0f).withTrack(SliderTrack::Temperature),
                   ParamDesc::Float("Tint", 0.0f, -100.0f, 100.0f).withTrack(SliderTrack::Tint),
                   ParamDesc::Float("Exposure", 0.0f, -5.0f, 5.0f), ParamDesc::Float("Contrast", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Highlights", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Shadows", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Whites", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Blacks", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Texture", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Clarity", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Dehaze", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Vibrance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Saturation", 0.0f, -100.0f, 100.0f)},
                  false, true})
    // Its local filters read around each pixel; sizes follow the full image's long edge. Dehaze's
    // airlight is global: a region reuses the preview's.
    int roiPadding(const EvalContext& ctx) const override {
        const PixelFrame fr = frameOf(ctx, ctx.defaultW, ctx.defaultH);
        const float longEdge = float(std::max(fr.fullW, fr.fullH));
        const bool lin = ctx.linear();
        int pad = 1;
        if (paramF(11) != 0.0f) {
            if (!ctx.previewStats) return kRoiWhole;
            pad += imageops::blurReach(std::max(longEdge * 0.01f, 1.0f));
        }
        if (lin && (paramF(5) != 0.0f || paramF(6) != 0.0f)) pad += guidedReach(std::max(longEdge * 0.02f, 1.0f));
        if (paramF(10) != 0.0f) {
            const float sigma = std::max(longEdge * 0.012f, 1.0f);
            pad += lin ? guidedReach(sigma) : 2 * imageops::blurReach(sigma) + 2;
        }
        if (paramF(9) != 0.0f) pad += imageops::blurReach(std::max(longEdge * 0.0025f, 0.7f));
        return pad;
    }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        if (ctx.linear()) {
            out[0] = Value(evaluateLinear(ctx, src, in[1]));
            return;
        }
        const float temp = paramF(1) / 100, tint = paramF(2) / 100, stops = paramF(3);
        const ToneParams tone{paramF(4) / 100, paramF(5) / 100, paramF(6) / 100, paramF(7) / 100, paramF(8) / 100};
        const float texture = paramF(9) / 100, clarity = paramF(10) / 100, dehaze = paramF(11) / 100;
        const float vibrance = paramF(12) / 100, saturation = paramF(13) / 100;
        const int w = src->w, h = src->h;
        const PixelFrame fr = frameOf(ctx, w, h);
        const float longEdge = float(std::max(fr.fullW, fr.fullH));

        // White balance and exposure in linear light. Temperature warms (more red, less blue),
        // Tint goes toward magenta (less green); the gains are normalised so white keeps its
        // brightness and only Exposure changes it.
        float gain[3] = {std::exp2(0.7f * temp), std::exp2(-0.5f * tint), std::exp2(-0.7f * temp)};
        const float norm = std::exp2(stops) / luminance(gain[0], gain[1], gain[2]);
        for (float& g : gain) g *= norm;
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            for (int k = 0; k < 3; ++k) d[k] = linearToSrgb(srgbToLinear(std::max(s[k], 0.0f)) * gain[k]);
            d[3] = s[3];
        });

        if (dehaze != 0.0f) dehazeImage(ctx, *img, dehaze, longEdge, 0.3f);

        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                float* d = img->pixel(size_t(y) * w + x);
                // Contrast is a per-channel curve (it adds saturation, as in Lightroom); the other
                // tone sliders scale RGB by the luminance change, so colours keep their saturation.
                if (tone.contrast != 0.0f) {
                    const float p = 1.0f + tone.contrast * (tone.contrast > 0 ? 0.9f : 0.6f);
                    for (int k = 0; k < 3; ++k) d[k] = sCurve(d[k], p);
                }
                const float l = std::max(luma(d), 0.0f), nl = toneCurve(l, tone);
                if (l > 1e-3f) {
                    const float m = std::min(nl / l, 4.0f);
                    for (int k = 0; k < 3; ++k) d[k] *= m;
                } else {
                    for (int k = 0; k < 3; ++k) d[k] += nl - l;
                }
            }
        });

        // Local contrast: detail = luma minus a blurred luma. Texture uses a small radius (fine
        // detail), Clarity a large one weighted toward midtones. Radii are relative to the image
        // so the preview matches the export.
        std::vector<float> lum, coarse, fine;
        if (clarity != 0.0f || texture != 0.0f) {
            lum.resize(size_t(w) * h);
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) lum[size_t(y) * w + x] = std::clamp(luma(img->pixel(size_t(y) * w + x)), 0.0f, 1.5f);
            });
            if (clarity != 0.0f) coarse = guidedSmooth(lum, w, h, std::max(longEdge * 0.012f, 1.0f), 0.004f);
            if (texture != 0.0f) {
                fine = lum;
                const float sg = std::max(longEdge * 0.0025f, 0.7f);
                imageops::blurChannel(fine, w, h, sg, sg);
            }
        }

        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                float* d = img->pixel(i);
                if (!lum.empty()) {
                    const float l = lum[i];
                    float delta = 0.0f;
                    if (!coarse.empty()) {
                        float dt = l - coarse[i];
                        dt /= 1.0f + 3.0f * std::fabs(dt);  // soft limit: no harsh overshoot
                        float mid = std::max(0.0f, 1.0f - (2.0f * l - 1.0f) * (2.0f * l - 1.0f));
                        delta += clarity * 1.2f * dt * (0.3f + 0.7f * mid);
                    }
                    if (!fine.empty()) delta += texture * 1.5f * (l - fine[i]);
                    for (int k = 0; k < 3; ++k) d[k] += delta;
                }
                for (int k = 0; k < 3; ++k) d[k] = clamp01(d[k]);
                if (vibrance != 0.0f || saturation != 0.0f) {
                    float hh, sat, v;
                    rgbToHsv(d[0], d[1], d[2], hh, sat, v);
                    // Vibrance boosts muted colours more than saturated ones and goes easier on
                    // skin tones (orange hues), like Lightroom's.
                    float vib = vibrance;
                    if (vib > 0) {
                        const float skin = 1.0f - 0.5f * smooth(0.0f, 0.04f, hh) * (1.0f - smooth(0.1f, 0.16f, hh));
                        vib *= (1.0f - sat) * skin;
                    }
                    const float m = std::max(0.0f, (1.0f + saturation) * (1.0f + vib));
                    const float l = luma(d);
                    for (int k = 0; k < 3; ++k) d[k] = clamp01(l + (d[k] - l) * m);
                }
            }
        });
        applyFactor(*this, *src, *img, in[1]);
        out[0] = Value(ImagePtr(img));
    }

    // The local filters run as GPU passes around the per-pixel maths: Dehaze's dark channel (its
    // airlight from an exact selection and a sum on the device), the tone equalizer's guided mask,
    // and Clarity's and Texture's smoothed luminance. The per-pixel steps between them fuse.
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        const float temp = paramF(1) / 100, tint = paramF(2) / 100, stops = paramF(3);
        const float highlights = paramF(5) / 100, shadows = paramF(6) / 100;
        const float texture = paramF(9) / 100, clarity = paramF(10) / 100, dehaze = paramF(11) / 100;
        int w, h;
        if (!in[0].size(w, h)) throw gpu::Error("GPU: Basic needs an image");
        const PixelFrame fr = frameOf(ctx, w, h);
        const float longEdge = float(std::max(fr.fullW, fr.fullH));

        // P[0..9]: white balance (a matrix and the exposure gain in linear projects, gains in legacy
        // ones); contrast, highlights, shadows, whites, blacks, vibrance, saturation at P[10..16];
        // texture, clarity, dehaze at P[17..19]; the airlight and its maximum at P[20..23].
        std::vector<float> P;
        if (lin) {
            colorsci::Mat3 wb;
            colorsci::whiteBalanceMatrix(temp, tint, wb);
            const bool useWb = temp != 0.0f || tint != 0.0f;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) P.push_back(useWb ? wb[r][c] : float(r == c));
            P.push_back(std::exp2(stops));
        } else {
            float gain[3] = {std::exp2(0.7f * temp), std::exp2(-0.5f * tint), std::exp2(-0.7f * temp)};
            const float norm = std::exp2(stops) / luminance(gain[0], gain[1], gain[2]);
            for (float g : gain) P.push_back(g * norm);
            P.resize(10, 0.0f);
        }
        for (int i : {4, 5, 6, 7, 8, 12, 13, 9, 10, 11}) P.push_back(paramF(i) / 100);
        P.resize(24, 0.0f);
        const std::string functions = basicGlsl(lin);
        auto pass = [&](const std::string& body, const std::vector<Value>& pin, bool image) {
            gpu::PointOp op;
            op.w = w, op.h = h;
            op.full = true;
            op.params = P;
            op.functions = functions;
            op.body = body;
            return gpu::runPass(ctx, op, pin, {image})[0];
        };

        Value cur = pass("    vec4 s = img0(p);\n    out0 = vec4(basicWb(s.rgb), s.a);\n", {in[0]}, true);

        if (dehaze != 0.0f) {
            const Value darkRaw = pass("    vec3 c = img0(p).rgb;\n    out0 = clamp(min(c.r, min(c.g, c.b)), 0.0, 1.0);\n",
                                       {cur}, false);
            const float sg = std::max(longEdge * 0.01f, 1.0f);
            const Value dark = channelValue(gpu::boxBlur(textureOf(darkRaw), sg, sg));
            // Airlight: the average colour of the haziest 0.1% (pixels at or above the dark
            // channel's value at that rank), as dehazeImage. A region reuses the whole image's.
            float A[3];
            if (ctx.roi && ctx.previewStats && ctx.previewStats->size() == 3) {
                for (int k = 0; k < 3; ++k) A[k] = (*ctx.previewStats)[size_t(k)];
            } else {
            const size_t n = size_t(w) * h, top = std::max<size_t>(1, n / 1000);
            const float thresh = gpu::select(*textureOf(dark), {n - top})[0];
            gpu::PointOp hazy;
            hazy.gather = {0, 1};
            hazy.params = {thresh};
            hazy.body = R"(
    vec4 acc = vec4(0.0);
    for (int y = p.y * 16; y < min(p.y * 16 + 16, size0.y); ++y)
        for (int x = p.x * 16; x < min(p.x * 16 + 16, size0.x); ++x)
            if (fetchCh1(ivec2(x, y)) >= P[0]) acc += vec4(clamp(fetch0(ivec2(x, y)).rgb, 0.0, 1.0), 1.0);
    out0 = acc;
)";
            const std::array<float, 4> sum = sumGpu(ctx, hazy, {cur, dark}, w, h);
            const float minAir = lin ? 0.1f : 0.3f;
            for (int k = 0; k < 3; ++k) A[k] = std::max(sum[size_t(k)] / std::max(sum[3], 1.0f), minAir);
            }
            if (ctx.statsOut) *ctx.statsOut = {A[0], A[1], A[2]};
            P[20] = A[0], P[21] = A[1], P[22] = A[2], P[23] = std::max({A[0], A[1], A[2]});
            cur = pass("    vec4 c = img0(p);\n    out0 = vec4(basicDehaze(c.rgb, ch1(p)), c.a);\n", {cur, dark}, true);
        }

        // Tone. The equalizer's mask is the guided filter on log luminance (linear projects).
        Value mask;
        if (lin && (highlights != 0.0f || shadows != 0.0f)) {
            const Value ev = pass("    out0 = evOf(luminance(img0(p).rgb));\n", {cur}, false);
            mask = guidedGpu(ctx, ev, w, h, std::max(longEdge * 0.02f, 1.0f), 0.5f, true, fr.x0, fr.y0);
        }
        cur = pass("    vec4 c = img0(p);\n    out0 = vec4(basicTone(c.rgb, has1, ch1(p)), c.a);\n", {cur, mask}, true);

        // Local contrast: luminance (log luminance in linear projects) and its smoothed copies.
        Value lum, coarse, fine;
        if (clarity != 0.0f || texture != 0.0f) {
            lum = pass(lin ? "    out0 = evOf(luminance(img0(p).rgb));\n"
                           : "    out0 = clamp(luminance(img0(p).rgb), 0.0, 1.5);\n",
                       {cur}, false);
            if (clarity != 0.0f) {
                const float sigma = std::max(longEdge * 0.012f, 1.0f);
                coarse = guidedGpu(ctx, lum, w, h, sigma, lin ? 0.1f : 0.004f, lin, fr.x0, fr.y0);
            }
            if (texture != 0.0f) {
                const float sg = std::max(longEdge * 0.0025f, 0.7f);
                fine = channelValue(gpu::boxBlur(textureOf(lum), sg, sg));
            }
        }

        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        op.params = P;
        op.functions = functions;
        op.body = R"(
    vec4 s = img0(p);
    out0 = applyFactor(s, basicFinish(img2(p).rgb, has3, ch3(p), has4, ch4(p), has5, ch5(p)), par1(p), LIN);)";
        gpu::runOver(ctx, *this, op, {in[0], in[1], cur, lum, coarse, fine}, out);
    }

private:
    ImagePtr evaluateLinear(const EvalContext& ctx, const ImagePtr& src, const Value& facIn) const {
        // Every slider at zero (the library's default graph): only finishLinear would act, and it
        // changes nothing but negative (or NaN) values. Without any, the source passes through
        // untouched instead of being copied (a 24 MP image is 384 MB). A RAW can have negatives.
        bool untouched = true;
        for (int i = 1; i <= 13; ++i) untouched = untouched && paramF(i) == 0.0f;
        if (untouched) {
            ChannelPtr fac = channelOr(facIn, 1.0f);
            if (fac->constant && fac->value >= 1.0f && !anyBelowZero(*src)) return src;
        }
        const float temp = paramF(1) / 100, tint = paramF(2) / 100, stops = paramF(3);
        const float contrast = paramF(4) / 100, highlights = paramF(5) / 100, shadows = paramF(6) / 100;
        const float whites = paramF(7) / 100, blacks = paramF(8) / 100;
        const float texture = paramF(9) / 100, clarity = paramF(10) / 100, dehaze = paramF(11) / 100;
        const float vibrance = paramF(12) / 100, saturation = paramF(13) / 100;
        const int w = src->w, h = src->h;
        const PixelFrame fr = frameOf(ctx, w, h);
        const float longEdge = float(std::max(fr.fullW, fr.fullH));

        // White balance adapts the assumed light to D65 (CAT16); exposure is a plain multiply.
        colorsci::Mat3 wb;
        colorsci::whiteBalanceMatrix(temp, tint, wb);
        const bool useWb = temp != 0.0f || tint != 0.0f;
        const float gain = std::exp2(stops);
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            if (useWb) colorsci::mul(wb, s, d);
            else d[0] = s[0], d[1] = s[1], d[2] = s[2];
            for (int k = 0; k < 3; ++k) d[k] *= gain;
            d[3] = s[3];
        });

        // Haze is additive light, so removing it is most accurate on linear values.
        if (dehaze != 0.0f) dehazeImage(ctx, *img, dehaze, longEdge, 0.1f);

        // Tone: every slider becomes a gain in stops, applied as a ratio to RGB so hue and
        // saturation hold. Contrast bends the exposure scale around middle grey (softly limited,
        // so it can't push highlights without bound). Highlights and Shadows form a tone
        // equalizer: their bands are read from an edge-aware smoothed luminance (a guided filter
        // on log luminance, exposure-independent like darktable's EIGF), so whole regions move
        // together and local contrast survives. Whites and Blacks follow each pixel's own level,
        // like moving the end points of a curve.
        if (contrast != 0.0f || highlights != 0.0f || shadows != 0.0f || whites != 0.0f || blacks != 0.0f) {
            const std::vector<float> ev = logLuminance(*img);
            std::vector<float> mask;
            if (highlights != 0.0f || shadows != 0.0f) mask = fastGuidedSmooth(ev, w, h, std::max(longEdge * 0.02f, 1.0f), 0.5f, fr.x0, fr.y0);
            const float slope = 1.0f + contrast * (contrast > 0 ? 0.6f : 0.45f);
            const float hA = highlights * (highlights < 0 ? 1.5f : 1.0f), sA = shadows * (shadows > 0 ? 2.0f : 1.5f);
            const float wA = whites, bA = blacks * 1.5f;
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(y) * w + x;
                    const float e = ev[i];
                    float g = 0.0f;
                    if (contrast != 0.0f) g += (slope - 1.0f) * 3.0f * fastTanh((e - kMidGreyEv) / 3.0f);
                    if (!mask.empty()) {
                        const float em = mask[i];
                        g += hA * smooth(-3.0f, -0.5f, em) + sA * (1.0f - smooth(-6.0f, -2.5f, em));
                    }
                    const float ep = e + g;
                    g += wA * smooth(-1.5f, 1.0f, ep) + bA * (1.0f - smooth(-9.0f, -4.5f, ep));
                    const float m = std::exp2(g);
                    float* d = img->pixel(i);
                    for (int k = 0; k < 3; ++k) d[k] *= m;
                }
            });
        }

        // Local contrast in stops: detail = log luminance minus a smoothed copy, applied as a
        // ratio. Texture uses a small radius (fine detail), Clarity a large edge-aware one weighted
        // toward the midtones. Radii are relative to the image so the preview matches the export.
        if (clarity != 0.0f || texture != 0.0f) {
            const std::vector<float> L = logLuminance(*img);
            std::vector<float> coarse, fine;
            if (clarity != 0.0f) coarse = fastGuidedSmooth(L, w, h, std::max(longEdge * 0.012f, 1.0f), 0.1f, fr.x0, fr.y0);
            if (texture != 0.0f) {
                fine = L;
                const float sg = std::max(longEdge * 0.0025f, 0.7f);
                imageops::blurChannel(fine, w, h, sg, sg);
            }
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(y) * w + x;
                    float delta = 0.0f;
                    if (!coarse.empty()) {
                        float dt = L[i] - coarse[i];
                        dt /= 1.0f + 0.5f * std::fabs(dt);  // soft limit: no harsh overshoot
                        const float z = (L[i] - kMidGreyEv) / 2.5f;
                        delta += clarity * 0.8f * dt * (0.3f + 0.7f * std::exp(-0.5f * z * z));
                    }
                    if (!fine.empty()) {
                        float dt = L[i] - fine[i];
                        dt /= 1.0f + 0.5f * std::fabs(dt);
                        delta += texture * dt;
                    }
                    const float m = std::exp2(delta);
                    float* d = img->pixel(i);
                    for (int k = 0; k < 3; ++k) d[k] *= m;
                }
            });
        }

        // Vibrance and Saturation scale Oklch chroma, so lightness and hue stay put. Vibrance
        // favours muted colours and, when boosting, goes easy on skin (orange) hues.
        if (vibrance != 0.0f || saturation != 0.0f) {
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    float* d = img->pixel(size_t(y) * w + x);
                    float lab[3];
                    colorsci::rgbToOklab(d, lab);
                    const float C = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
                    float vib = vibrance;
                    if (vib > 0.0f) {
                        const float hue = std::atan2(lab[2], lab[1]) * 57.29578f;
                        const float skin = 1.0f - 0.5f * smooth(20.0f, 40.0f, hue) * (1.0f - smooth(75.0f, 95.0f, hue));
                        vib *= (1.0f - smooth(0.0f, 0.2f, C)) * skin;
                    }
                    const float m = std::max(0.0f, (1.0f + saturation) * (1.0f + vib));
                    lab[1] *= m, lab[2] *= m;
                    colorsci::oklabToRgb(lab, d);
                }
            });
        }
        finishLinear(*img);
        applyFactor(*this, *src, *img, facIn, true);
        return img;
    }

    // Basic's per-pixel steps in GLSL, reading the params laid out in evaluateGpu.
    static std::string basicGlsl(bool lin) {
        return kGlslDevelop + std::string(lin ? "const bool LIN = true;\n" : "const bool LIN = false;\n") + R"(
const float kMidGreyEv = -2.4739312;
float evOf(float y) { return log2(max(y, 1.0 / 65536.0)); }
float sCurve(float x, float p) {
    float xc = clamp(x, 0.0, 1.0);
    float y = xc < 0.5 ? 0.5 * powPos(2.0 * xc, p) : 1.0 - 0.5 * powPos(2.0 - 2.0 * xc, p);
    return y + (x - xc);
}
float toneCurve(float v, float highlights, float shadows, float whites, float blacks) {
    if (highlights != 0.0) {
        float w = smoothT(0.35, 1.0, v);
        if (highlights < 0.0) v += highlights * 0.22 * w * max(v, 0.0);
        else v += highlights * 0.2 * w * clamp(1.25 - v, 0.0, 1.0);
    }
    if (shadows != 0.0) {
        float w = 1.0 - smoothT(0.0, 0.6, v);
        if (shadows > 0.0) v += shadows * 0.18 * w * smoothT(0.0, 0.2, v);
        else v += shadows * 0.4 * w * max(v, 0.0);
    }
    if (whites != 0.0) {
        float wp = 1.0 - 0.2 * whites;
        v += (v / wp - v) * smoothT(0.4, 1.0, v);
    }
    if (blacks != 0.0) {
        float bp = -0.08 * blacks;
        v += ((v - bp) / (1.0 - bp) - v) * (1.0 - smoothT(0.0, 0.5, v));
    }
    return v;
}
vec3 basicWb(vec3 s) {
    if (LIN)
        return vec3(P[0] * s.r + P[1] * s.g + P[2] * s.b, P[3] * s.r + P[4] * s.g + P[5] * s.b,
                    P[6] * s.r + P[7] * s.g + P[8] * s.b) * P[9];
    return linearToSrgb(srgbToLinear(max(s, 0.0)) * vec3(P[0], P[1], P[2]));
}
vec3 basicDehaze(vec3 c, float dark) {
    float amount = P[19];
    vec3 A = vec3(P[20], P[21], P[22]);
    if (amount > 0.0) {
        float t = max(1.0 - 0.95 * amount * dark / P[23], 0.25);
        return (c - A) / t + A;
    }
    float hz = -amount * (0.35 + 0.4 * dark);
    return c + (A - c) * hz;
}
vec3 basicTone(vec3 d, bool hasMask, float em) {
    float contrast = P[10], highlights = P[11], shadows = P[12], whites = P[13], blacks = P[14];
    if (LIN) {
        if (contrast != 0.0 || hasMask || whites != 0.0 || blacks != 0.0) {
            float e = evOf(luminance(d)), g = 0.0;
            float slope = 1.0 + contrast * (contrast > 0.0 ? 0.6 : 0.45);
            if (contrast != 0.0) g += (slope - 1.0) * 3.0 * tanh((e - kMidGreyEv) / 3.0);
            if (hasMask) {
                float hA = highlights * (highlights < 0.0 ? 1.5 : 1.0), sA = shadows * (shadows > 0.0 ? 2.0 : 1.5);
                g += hA * smoothT(-3.0, -0.5, em) + sA * (1.0 - smoothT(-6.0, -2.5, em));
            }
            float ep = e + g;
            g += whites * smoothT(-1.5, 1.0, ep) + blacks * 1.5 * (1.0 - smoothT(-9.0, -4.5, ep));
            d *= exp2(g);
        }
        return d;
    }
    if (contrast != 0.0) {
        float pw = 1.0 + contrast * (contrast > 0.0 ? 0.9 : 0.6);
        d = vec3(sCurve(d.r, pw), sCurve(d.g, pw), sCurve(d.b, pw));
    }
    float l = max(luminance(d), 0.0), nl = toneCurve(l, highlights, shadows, whites, blacks);
    if (l > 1e-3) d *= min(nl / l, 4.0);
    else d += nl - l;
    return d;
}
// Local contrast from the luminance l and its smoothed copies, then vibrance and saturation.
vec3 basicFinish(vec3 d, bool local, float l, bool hasCoarse, float coarse, bool hasFine, float fine) {
    float vibrance = P[15], saturation = P[16], texture = P[17], clarity = P[18];
    if (LIN) {
        if (local) {
            float delta = 0.0;
            if (hasCoarse) {
                float dt = l - coarse;
                dt /= 1.0 + 0.5 * abs(dt);
                float z = (l - kMidGreyEv) / 2.5;
                delta += clarity * 0.8 * dt * (0.3 + 0.7 * exp(-0.5 * z * z));
            }
            if (hasFine) {
                float dt = l - fine;
                dt /= 1.0 + 0.5 * abs(dt);
                delta += texture * dt;
            }
            d *= exp2(delta);
        }
        if (vibrance != 0.0 || saturation != 0.0) {
            vec3 lab = rgbToOklab(d);
            float C = sqrt(lab.y * lab.y + lab.z * lab.z), vib = vibrance;
            if (vib > 0.0) {
                float hue = atan2C(lab.z, lab.y) * 57.29578;
                float skin = 1.0 - 0.5 * smoothT(20.0, 40.0, hue) * (1.0 - smoothT(75.0, 95.0, hue));
                vib *= (1.0 - smoothT(0.0, 0.2, C)) * skin;
            }
            float m = max(0.0, (1.0 + saturation) * (1.0 + vib));
            d = oklabToRgb(vec3(lab.x, lab.y * m, lab.z * m));
        }
        return compressToGamut(d);
    }
    if (local) {
        float delta = 0.0;
        if (hasCoarse) {
            float dt = l - coarse;
            dt /= 1.0 + 3.0 * abs(dt);
            float mid = max(0.0, 1.0 - (2.0 * l - 1.0) * (2.0 * l - 1.0));
            delta += clarity * 1.2 * dt * (0.3 + 0.7 * mid);
        }
        if (hasFine) delta += texture * 1.5 * (l - fine);
        d += delta;
    }
    d = clamp01(d);
    if (vibrance != 0.0 || saturation != 0.0) {
        vec3 hsv = rgbToHsv(d);
        float vib = vibrance;
        if (vib > 0.0) {
            float skin = 1.0 - 0.5 * smoothT(0.0, 0.04, hsv.x) * (1.0 - smoothT(0.1, 0.16, hsv.x));
            vib *= (1.0 - hsv.y) * skin;
        }
        float m = max(0.0, (1.0 + saturation) * (1.0 + vib));
        float ll = luminance(d);
        d = clamp01(ll + (d - ll) * m);
    }
    return d;
}
)";
    }

    // Dark channel prior (He et al.): haze lifts the darkest channel of every patch toward the
    // airlight colour. Positive amounts remove that veil, negative ones add haze.
    // minAir: the airlight's floor (0.3 is mid grey in encoded values; linear ones need less).
    static void dehazeImage(const EvalContext& ctx, Image& img, float amount, float longEdge, float minAir) {
        const int w = img.w, h = img.h;
        const size_t n = size_t(w) * h;
        std::vector<float> dark(n);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const float* p = img.pixel(size_t(y) * w + x);
                dark[size_t(y) * w + x] = std::clamp(std::min({p[0], p[1], p[2]}), 0.0f, 1.0f);
            }
        });
        const float sg = std::max(longEdge * 0.01f, 1.0f);
        imageops::blurChannel(dark, w, h, sg, sg);
        // Airlight: average colour of the haziest 0.1% of pixels; a region reuses the preview's.
        float A[3];
        if (ctx.roi && ctx.previewStats && ctx.previewStats->size() == 3) {
            for (int k = 0; k < 3; ++k) A[k] = (*ctx.previewStats)[size_t(k)];
        } else {
            std::vector<float> sorted = dark;
            const size_t top = std::max<size_t>(1, n / 1000);
            std::nth_element(sorted.begin(), sorted.begin() + (n - top), sorted.end());
            const float thresh = sorted[n - top];
            double acc[3] = {0, 0, 0};
            size_t cnt = 0;
            for (size_t i = 0; i < n; ++i)
                if (dark[i] >= thresh) {
                    const float* p = img.pixel(i);
                    for (int k = 0; k < 3; ++k) acc[k] += std::clamp(p[k], 0.0f, 1.0f);
                    ++cnt;
                }
            for (int k = 0; k < 3; ++k) A[k] = std::max(float(acc[k] / double(std::max<size_t>(cnt, 1))), minAir);
            if (ctx.statsOut) *ctx.statsOut = {A[0], A[1], A[2]};
        }
        const float amax = std::max({A[0], A[1], A[2]});
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                float* p = img.pixel(i);
                if (amount > 0) {
                    const float t = std::max(1.0f - 0.95f * amount * dark[i] / amax, 0.25f);
                    for (int k = 0; k < 3; ++k) p[k] = (p[k] - A[k]) / t + A[k];
                } else {
                    // Thicker haze where the scene is already hazy (farther away).
                    const float hz = -amount * (0.35f + 0.4f * dark[i]);
                    for (int k = 0; k < 3; ++k) p[k] += (A[k] - p[k]) * hz;
                }
            }
        });
    }
};

// ---------------------------------------------------------------- Color Mixer

// Lightroom's eight colour bands, by centre hue in degrees.
const char* const kBandNames[8] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
const float kBandHue[8] = {0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 225.0f, 270.0f, 315.0f};

// Weights of the bands for a hue (0..360): the two neighbouring bands share it with a smooth
// crossfade, so the weights always sum to 1 and adjustments blend without seams.
void bandWeights(float hueDeg, float wgt[8]) {
    for (int i = 0; i < 8; ++i) wgt[i] = 0.0f;
    hueDeg = std::fmod(std::fmod(hueDeg, 360.0f) + 360.0f, 360.0f);
    for (int i = 0; i < 8; ++i) {
        const int j = (i + 1) % 8;
        const float a = kBandHue[i], b = j == 0 ? 360.0f : kBandHue[j];
        if (hueDeg >= a && hueDeg < b) {
            const float t = smooth(0.0f, 1.0f, (hueDeg - a) / (b - a));
            wgt[i] = 1.0f - t;
            wgt[j] = t;
            return;
        }
    }
    wgt[0] = 1.0f;
}

// The bands' centres as Oklch hues: the Oklch hue of each band's pure sRGB colour, so a band
// still selects the colours its name says.
const float* bandHuesOklch() {
    static const auto hues = [] {
        std::array<float, 8> a{};
        for (int i = 0; i < 8; ++i) {
            float r, g, b;
            hsvToRgb(kBandHue[i] / 360.0f, 1.0f, 1.0f, r, g, b);
            a[size_t(i)] = colorsci::oklabHueOfSrgb(r, g, b);
        }
        return a;
    }();
    return hues.data();
}

// bandWeights() for band centres that don't start at 0 degrees. Also returns, per band, the
// distance to the neighbouring centres, which scales the Hue sliders.
void bandWeightsAt(float hueDeg, const float* centre, float wgt[8]) {
    for (int i = 0; i < 8; ++i) wgt[i] = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const int j = (i + 1) % 8;
        float a = centre[i], b = centre[j];
        if (b <= a) b += 360.0f;
        float h = hueDeg;
        if (h < a) h += 360.0f;
        if (h >= a && h < b) {
            const float t = smooth(0.0f, 1.0f, (h - a) / (b - a));
            wgt[i] = 1.0f - t;
            wgt[j] = t;
            return;
        }
    }
    wgt[0] = 1.0f;
}

// sin and cos of an angle in radians, by Taylor series where |x| < 1 (within 1e-8 there, and
// several times faster than MinGW's sinf/cosf).
void sinCosSmall(float x, float& s, float& c) {
    if (std::fabs(x) >= 1.0f) {
        s = std::sin(x);
        c = std::cos(x);
        return;
    }
    const float x2 = x * x;
    s = x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
    c = 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f * (1.0f - x2 / 56.0f * (1.0f - x2 / 90.0f))));
}

float hueGap(const float* centre, int from, int to) {
    float d = std::fabs(centre[to] - centre[from]);
    return d > 180.0f ? 360.0f - d : d;
}

std::vector<ParamDesc> mixerParams() {
    std::vector<ParamDesc> p{ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f)};
    // Each slider's track shows its band's colour changing: Hue moves it by up to 30 degrees
    // either way (as evaluate does), Saturation and Luminance fade it.
    const SliderTrack tracks[3] = {SliderTrack::HueShift, SliderTrack::Saturation, SliderTrack::Luminance};
    const char* const what[3] = {"Hue", "Saturation", "Luminance"};
    for (int k = 0; k < 3; ++k)
        for (int b = 0; b < 8; ++b)
            p.push_back(ParamDesc::Float(std::string(kBandNames[b]) + " " + what[k], 0.0f, -100.0f, 100.0f)
                            .withTrack(tracks[k], kBandHue[b], 30.0f));
    return p;
}

class ColorMixerNode : public Node {
public:
    NODELAB_NODE({"color.color_mixer", "Color Mixer", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  mixerParams(), false, true})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        float hue[8], sat[8], lum[8];
        for (int i = 0; i < 8; ++i) hue[i] = paramF(1 + i) / 100, sat[i] = paramF(9 + i) / 100, lum[i] = paramF(17 + i) / 100;
        if (ctx.linear()) {
            out[0] = Value(evaluateLinear(src, in[1], hue, sat, lum));
            return;
        }
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            float h, sv, v;
            rgbToHsv(clamp01(s[0]), clamp01(s[1]), clamp01(s[2]), h, sv, v);
            float wgt[8];
            bandWeights(h * 360.0f, wgt);
            float dh = 0, ds = 0, dl = 0;
            for (int i = 0; i < 8; ++i) dh += wgt[i] * hue[i], ds += wgt[i] * sat[i], dl += wgt[i] * lum[i];
            // Greys have no hue, so the bands fade out as colour does.
            const float colourful = smooth(0.0f, 0.15f, sv * v);
            float r, g, b;
            // Hue +-100 moves about halfway to the neighbouring band.
            hsvToRgb(h + dh * colourful * 30.0f / 360.0f, sv, v, r, g, b);
            float c[3] = {r, g, b};
            const float l = luminance(r, g, b), m = std::max(0.0f, 1.0f + ds * colourful);
            // Luminance scales in linear light (-100 is about one stop down) so hue and
            // saturation hold.
            const float lm = std::exp2(dl * colourful);
            for (int k = 0; k < 3; ++k) {
                float cc = l + (c[k] - l) * m;
                d[k] = clamp01(linearToSrgb(srgbToLinear(std::max(cc, 0.0f)) * lm));
            }
            d[3] = s[3];
        });
        applyFactor(*this, *src, *img, in[1]);
        out[0] = Value(ImagePtr(img));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        float hue[8], sat[8], lum[8];
        bool any = false;
        for (int i = 0; i < 8; ++i) {
            hue[i] = paramF(1 + i) / 100, sat[i] = paramF(9 + i) / 100, lum[i] = paramF(17 + i) / 100;
            any |= hue[i] != 0.0f || sat[i] != 0.0f || lum[i] != 0.0f;
        }
        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        // P: the band centres, their hue moves, saturation and luminance (8 each).
        const float* centre = lin ? bandHuesOklch() : kBandHue;
        op.params.assign(centre, centre + 8);
        for (int i = 0; i < 8; ++i)
            op.params.push_back(!lin ? hue[i]
                                     : hue[i] * 0.5f * (hue[i] > 0 ? hueGap(centre, i, (i + 1) % 8) : hueGap(centre, i, (i + 7) % 8)));
        op.params.insert(op.params.end(), sat, sat + 8);
        op.params.insert(op.params.end(), lum, lum + 8);
        op.functions = kGlslDevelop + std::string(lin ? "const bool LIN = true;\n" : "const bool LIN = false;\n") +
                       (any ? "const bool ANY = true;\n" : "const bool ANY = false;\n") + R"(
// bandWeightsAt: the band adjustments for a hue, crossfading between the two neighbouring
// bands. (With the legacy centres from 0 degrees it is bandWeights.)
vec3 bandAdjust(float hueDeg) {
    for (int i = 0; i < 8; ++i) {
        int j = (i + 1) % 8;
        float a = P[i], b = P[j];
        if (b <= a) b += 360.0;
        float h = hueDeg;
        if (h < a) h += 360.0;
        if (h >= a && h < b) {
            float t = smoothT(0.0, 1.0, (h - a) / (b - a));
            return vec3(P[8 + i], P[16 + i], P[24 + i]) * (1.0 - t) + vec3(P[8 + j], P[16 + j], P[24 + j]) * t;
        }
    }
    return vec3(P[8], P[16], P[24]);
}
)";
        op.body = R"(
    vec4 s = img0(p);
    vec3 d;
    if (LIN) {
        if (!ANY) { out0 = s; return; }
        vec3 lab = rgbToOklab(s.rgb);
        float C = sqrt(lab.y * lab.y + lab.z * lab.z);
        float hDeg = atan2C(lab.z, lab.y) * 57.29578;
        if (hDeg < 0.0) hDeg += 360.0;
        vec3 adj = bandAdjust(hDeg);
        float colourful = smoothT(0.0, 0.04, C);
        float h = (hDeg + adj.x * colourful) * 0.017453293, c = C * max(0.0, 1.0 + adj.y * colourful);
        d = compressToGamut(oklabToRgb(vec3(lab.x, c * cos(h), c * sin(h))) * exp2(adj.z * colourful));
    } else {
        vec3 hsv = rgbToHsv(clamp01(s.rgb));
        vec3 adj = bandAdjust(mod(mod(hsv.x * 360.0, 360.0) + 360.0, 360.0));
        float colourful = smoothT(0.0, 0.15, hsv.y * hsv.z);
        vec3 c = hsvToRgb(vec3(hsv.x + adj.x * colourful * 30.0 / 360.0, hsv.y, hsv.z));
        float l = luminance(c), m = max(0.0, 1.0 + adj.y * colourful);
        d = clamp01(linearToSrgb(srgbToLinear(max(l + (c - l) * m, 0.0)) * exp2(adj.z * colourful)));
    }
    out0 = applyFactor(s, d, par1(p), LIN);)";
        gpu::runOver(ctx, *this, op, in, out);
    }

private:
    // The same sliders in Oklch: hue moves along the perceptual hue circle, saturation scales
    // chroma, and luminance is an exposure change of the band (-100 is about one stop down).
    ImagePtr evaluateLinear(const ImagePtr& src, const Value& facIn, const float* hue, const float* sat, const float* lum) const {
        bool any = false;
        for (int i = 0; i < 8; ++i) any |= hue[i] != 0.0f || sat[i] != 0.0f || lum[i] != 0.0f;
        if (!any) return src;
        const float* centre = bandHuesOklch();
        // Hue +-100 moves about halfway to the neighbouring band on that side.
        float shift[8];
        for (int i = 0; i < 8; ++i)
            shift[i] = hue[i] * 0.5f * (hue[i] > 0 ? hueGap(centre, i, (i + 1) % 8) : hueGap(centre, i, (i + 7) % 8));
        // The blended band adjustments (hue move in radians, saturation, luminance) depend only
        // on the hue, so they come from a table at 0.1 degree steps, interpolated.
        constexpr int kSteps = 3600;
        std::vector<float> table(size_t(kSteps + 1) * 3);
        for (int i = 0; i <= kSteps; ++i) {
            float wgt[8];
            bandWeightsAt(float(i % kSteps) * (360.0f / kSteps), centre, wgt);
            float* t = &table[size_t(i) * 3];
            t[0] = t[1] = t[2] = 0.0f;
            for (int b = 0; b < 8; ++b) t[0] += wgt[b] * shift[b], t[1] += wgt[b] * sat[b], t[2] += wgt[b] * lum[b];
            t[0] *= 0.017453293f;
        }
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            float lab[3];
            colorsci::rgbToOklab(s, lab);
            const float C = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
            float hDeg = std::atan2(lab[2], lab[1]) * 57.29578f;
            if (hDeg < 0) hDeg += 360.0f;
            const float f = hDeg > 0.0f ? std::min(hDeg * (kSteps / 360.0f), float(kSteps)) : 0.0f;  // NaN to 0
            const int i = std::min(int(f), kSteps - 1);
            const float u = f - float(i);
            const float* t0 = &table[size_t(i) * 3];
            const float* t1 = t0 + 3;
            // Greys have no hue, so the bands fade out as colour does.
            const float colourful = smooth(0.0f, 0.04f, C);
            const float dh = (t0[0] + (t1[0] - t0[0]) * u) * colourful;
            const float ds = (t0[1] + (t1[1] - t0[1]) * u) * colourful;
            const float dl = (t0[2] + (t1[2] - t0[2]) * u) * colourful;
            // Turning the hue by dh and scaling chroma is a rotation and scale of (a, b).
            const float m = std::max(0.0f, 1.0f + ds);
            float c, sn;
            sinCosSmall(dh, sn, c);
            const float o[3] = {lab[0], m * (lab[1] * c - lab[2] * sn), m * (lab[1] * sn + lab[2] * c)};
            colorsci::oklabToRgb(o, d);
            const float lm = std::exp2(dl);
            for (int k = 0; k < 3; ++k) d[k] *= lm;
            d[3] = s[3];
        });
        finishLinear(*img);
        applyFactor(*this, *src, *img, facIn, true);
        return img;
    }
};

// ---------------------------------------------------------------- Color Grading

class ColorGradingNode : public Node {
public:
    NODELAB_NODE({"color.color_grading", "Color Grading", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Shadows Hue", 220.0f, 0.0f, 360.0f).withTrack(SliderTrack::Hue), ParamDesc::Float("Shadows Saturation", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Shadows Luminance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Midtones Hue", 40.0f, 0.0f, 360.0f).withTrack(SliderTrack::Hue), ParamDesc::Float("Midtones Saturation", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Midtones Luminance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Highlights Hue", 45.0f, 0.0f, 360.0f).withTrack(SliderTrack::Hue), ParamDesc::Float("Highlights Saturation", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Highlights Luminance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Global Hue", 0.0f, 0.0f, 360.0f).withTrack(SliderTrack::Hue), ParamDesc::Float("Global Saturation", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Global Luminance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Blending", 50.0f, 0.0f, 100.0f), ParamDesc::Float("Balance", 0.0f, -100.0f, 100.0f)},
                  false, true})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        if (ctx.linear()) {
            out[0] = Value(evaluateLinear(src, in[1]));
            return;
        }
        // Per zone (shadows, midtones, highlights, global): a luma-neutral tint and a lift.
        float tint[4][3], lift[4];
        for (int z = 0; z < 4; ++z) {
            float r, g, b;
            hsvToRgb(paramF(1 + z * 3) / 360.0f, 1.0f, 1.0f, r, g, b);
            const float l = luminance(r, g, b), amt = paramF(2 + z * 3) / 100 * 0.25f;
            tint[z][0] = (r - l) * amt, tint[z][1] = (g - l) * amt, tint[z][2] = (b - l) * amt;
            lift[z] = paramF(3 + z * 3) / 100 * 0.25f;
        }
        // Balance moves the split between shadows and highlights; Blending widens the overlap.
        const float pivot = 0.5f - paramF(14) / 100 * 0.3f;
        const float k = 1.0f + 3.0f * (1.0f - paramF(13) / 100);
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            const float l = clamp01(luminance(s[0], s[1], s[2]));
            float wz[4];
            wz[0] = std::pow(1.0f - smooth(0.0f, 2.0f * pivot, l), k);
            wz[2] = std::pow(smooth(2.0f * pivot - 1.0f, 1.0f, l), k);
            wz[1] = std::max(0.0f, 1.0f - wz[0] - wz[2]);
            wz[3] = 1.0f;
            for (int c = 0; c < 3; ++c) {
                float v = s[c];
                for (int z = 0; z < 4; ++z) v += (tint[z][c] + lift[z]) * wz[z];
                d[c] = clamp01(v);
            }
            d[3] = s[3];
        });
        applyFactor(*this, *src, *img, in[1]);
        out[0] = Value(ImagePtr(img));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        // P: per zone (shadows, midtones, highlights, global) a tint (legacy: RGB; linear: Oklab
        // a/b direction and amount) and a lift, then the pivot and the zone exponent at P[16..17].
        bool any = false;
        op.params.assign(18, 0.0f);
        for (int z = 0; z < 4; ++z) {
            float r, g, b;
            hsvToRgb(paramF(1 + z * 3) / 360.0f, 1.0f, 1.0f, r, g, b);
            if (lin) {
                const float h = colorsci::oklabHueOfSrgb(r, g, b) * 0.017453293f;
                op.params[size_t(z * 3)] = std::cos(h);
                op.params[size_t(z * 3 + 1)] = std::sin(h);
                op.params[size_t(z * 3 + 2)] = paramF(2 + z * 3) / 100 * 0.08f;
                op.params[size_t(12 + z)] = paramF(3 + z * 3) / 100 * 0.2f;
                any |= op.params[size_t(z * 3 + 2)] != 0.0f || op.params[size_t(12 + z)] != 0.0f;
            } else {
                const float l = luminance(r, g, b), amt = paramF(2 + z * 3) / 100 * 0.25f;
                op.params[size_t(z * 3)] = (r - l) * amt;
                op.params[size_t(z * 3 + 1)] = (g - l) * amt;
                op.params[size_t(z * 3 + 2)] = (b - l) * amt;
                op.params[size_t(12 + z)] = paramF(3 + z * 3) / 100 * 0.25f;
            }
        }
        op.params[16] = 0.5f - paramF(14) / 100 * 0.3f;
        op.params[17] = 1.0f + 3.0f * (1.0f - paramF(13) / 100);
        op.functions = kGlslDevelop + std::string(lin ? "const bool LIN = true;\n" : "const bool LIN = false;\n") +
                       (any ? "const bool ANY = true;\n" : "const bool ANY = false;\n");
        op.body = R"(
    vec4 s = img0(p);
    if (LIN && !ANY) { out0 = s; return; }
    vec3 lab = LIN ? rgbToOklab(s.rgb) : vec3(0.0);
    float l = clamp01(LIN ? lab.x : luminance(s.rgb));
    float wz[4];
    wz[0] = powPos(1.0 - smoothT(0.0, 2.0 * P[16], l), P[17]);
    wz[2] = powPos(smoothT(2.0 * P[16] - 1.0, 1.0, l), P[17]);
    wz[1] = max(0.0, 1.0 - wz[0] - wz[2]);
    wz[3] = 1.0;
    vec3 d;
    if (LIN) {
        float tintFade = smoothT(0.0, 0.25, l);
        for (int z = 0; z < 4; ++z) {
            lab.y += P[z * 3] * P[z * 3 + 2] * wz[z] * tintFade;
            lab.z += P[z * 3 + 1] * P[z * 3 + 2] * wz[z] * tintFade;
            lab.x += P[12 + z] * wz[z];
        }
        d = compressToGamut(oklabToRgb(vec3(max(lab.x, 0.0), lab.yz)));
    } else {
        d = s.rgb;
        for (int z = 0; z < 4; ++z) d += (vec3(P[z * 3], P[z * 3 + 1], P[z * 3 + 2]) + P[12 + z]) * wz[z];
        d = clamp01(d);
    }
    out0 = applyFactor(s, d, par1(p), LIN);)";
        gpu::runOver(ctx, *this, op, in, out);
    }

private:
    // Zones by Oklab lightness (perceptual, so the split sits where it looks right on scene
    // values); each zone shifts Oklab a/b toward its hue and moves lightness.
    ImagePtr evaluateLinear(const ImagePtr& src, const Value& facIn) const {
        bool any = false;
        float dir[4][2], amt[4], lift[4];
        for (int z = 0; z < 4; ++z) {
            float r, g, b;
            hsvToRgb(paramF(1 + z * 3) / 360.0f, 1.0f, 1.0f, r, g, b);
            const float h = colorsci::oklabHueOfSrgb(r, g, b) * 0.017453293f;
            dir[z][0] = std::cos(h), dir[z][1] = std::sin(h);
            amt[z] = paramF(2 + z * 3) / 100 * 0.08f;
            lift[z] = paramF(3 + z * 3) / 100 * 0.2f;
            any |= amt[z] != 0.0f || lift[z] != 0.0f;
        }
        if (!any) return src;
        const float pivot = 0.5f - paramF(14) / 100 * 0.3f;
        const float k = 1.0f + 3.0f * (1.0f - paramF(13) / 100);
        // The shifts of L, a and b depend only on the pixel's lightness (clamped to 0..1), so
        // they come from a table, interpolated: the two powers per pixel cost more than the rest.
        constexpr int kSteps = 4096;
        std::vector<float> table(size_t(kSteps + 1) * 3);
        for (int i = 0; i <= kSteps; ++i) {
            const float l = float(i) / kSteps;
            float wz[4];
            wz[0] = std::pow(1.0f - smooth(0.0f, 2.0f * pivot, l), k);
            wz[2] = std::pow(smooth(2.0f * pivot - 1.0f, 1.0f, l), k);
            wz[1] = std::max(0.0f, 1.0f - wz[0] - wz[2]);
            wz[3] = 1.0f;
            // Tints fade out toward black, so black stays neutral.
            const float tintFade = smooth(0.0f, 0.25f, l);
            float* t = &table[size_t(i) * 3];
            t[0] = t[1] = t[2] = 0.0f;
            for (int z = 0; z < 4; ++z) {
                t[0] += lift[z] * wz[z];
                t[1] += dir[z][0] * amt[z] * wz[z] * tintFade;
                t[2] += dir[z][1] * amt[z] * wz[z] * tintFade;
            }
        }
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            float lab[3];
            colorsci::rgbToOklab(s, lab);
            const float f = lab[0] > 0.0f ? std::min(lab[0], 1.0f) * kSteps : 0.0f;  // NaN to 0
            const int i = std::min(int(f), kSteps - 1);
            const float u = f - float(i);
            const float* t0 = &table[size_t(i) * 3];
            const float* t1 = t0 + 3;
            const float L = lab[0] + t0[0] + (t1[0] - t0[0]) * u;
            const float a = lab[1] + t0[1] + (t1[1] - t0[1]) * u;
            const float b = lab[2] + t0[2] + (t1[2] - t0[2]) * u;
            const float o[3] = {std::max(L, 0.0f), a, b};
            colorsci::oklabToRgb(o, d);
            d[3] = s[3];
        });
        finishLinear(*img);
        applyFactor(*this, *src, *img, facIn, true);
        return img;
    }
};

}  // namespace

void registerDevelopNodes(NodeRegistry& r) {
    r.add<BasicNode>();
    r.add<ColorMixerNode>();
    r.add<ColorGradingNode>();
}
