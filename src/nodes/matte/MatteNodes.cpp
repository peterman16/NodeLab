// Matte nodes: shape masks and keyers. Keyers output a Matte channel (1 = keep) and the keyed
// image (color * matte, alpha = matte).
#include "nodes/matte/MatteNodes.h"
#include "nodes/matte/AutoMask.h"

#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/ColorMath.h"
#include "core/ColorScience.h"
#include "gpu/PointOp.h"
#include "nodes/ImageOps.h"

using namespace nodeutil;
using namespace colormath;

namespace {

constexpr float kPi = 3.14159265f;

float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Emits Matte + keyed Image from a per-pixel matte function.
template <typename Fn>
void keyOutputs(const Image& src, bool invert, std::vector<Value>& out, Fn&& matteOf) {
    auto matte = std::make_shared<Channel>(Channel::makeSized(src.w, src.h));
    auto keyed = mapImage(src, [&](int x, int y, const float* s, float* d) {
        float m = clamp01(matteOf(s, x, y));
        if (invert) m = 1.0f - m;
        matte->data[size_t(y) * src.w + x] = m;
        for (int k = 0; k < 3; ++k) d[k] = s[k] * m;
        d[3] = s[3] * m;
    });
    out[0] = Value(ChannelPtr(matte));
    out[1] = Value(ImagePtr(keyed));
}

// The GPU version of keyOutputs. `matte` sets `float m` from `s` (the pixel) and, for keyers with
// a Key input (pin 1), `k`: the Key input's pixel (clamped to its edges, as KeySource::at) or
// the Key Color param in P[0..2].
void gpuKey(EvalContext& ctx, const Node& node, const std::vector<Value>& in, std::vector<Value>& out,
            const std::string& matte, std::vector<float> params, bool invert, bool key, const std::string& functions = "") {
    gpu::PointOp g;
    g.functions = functions;
    g.body = "    vec4 s = img0(p);\n";
    if (key) {
        g.body += "    vec3 k = has1 ? fetch1(p).rgb : vec3(P[0], P[1], P[2]);\n";
        g.gather = {1};
    }
    g.body += "    float m;\n" + matte + "    m = clamp01(m);\n" + (invert ? "    m = 1.0 - m;\n" : "") +
              "    out0 = m;\n    out1 = s * m;\n";
    g.params = std::move(params);
    gpu::runOver(ctx, node, g, in, out);
}

// Key color: the Key input's pixel if connected, otherwise the Color param.
struct KeySource {
    ImagePtr img;
    float color[3];
    const float* at(int x, int y) const {
        if (img && img->w > 0) return img->pixel(size_t(std::min(y, img->h - 1)) * img->w + std::min(x, img->w - 1));
        return color;
    }
};

// ---------------------------------------------------------------- shape masks

// Combines a mask value into the base mask (the shape masks' Operation).
float combineMask(int op, float b, float v) {
    switch (op) {
        case 1: return clamp01(b - v);                  // Subtract
        case 2: return clamp01(b * v);                  // Multiply
        case 3: return clamp01(std::max(b, 1.0f - v));  // Not: everything outside the shape
        default: return clamp01(std::max(b, v));        // Add
    }
}

// Size of a mask node's output: its Mask input's, else the working size (or region window).
void maskSize(const ChannelPtr& base, const EvalContext& ctx, int& w, int& h) {
    if (base && !base->constant) {
        w = base->w;
        h = base->h;
    } else {
        resolveSize({}, ctx, w, h);
    }
}

// GLSL versions of smoothstep (with its degenerate case) and combineMask.
const char* const kGlslMatte = R"(
float smoothstepC(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
float combineMask(int op, float b, float v) {
    if (op == 1) return clamp01(b - v);
    if (op == 2) return clamp01(b * v);
    if (op == 3) return clamp01(max(b, 1.0 - v));
    return clamp01(max(b, v));
}
)";

// Masks are drawn per pixel from its position (in the full image), so a region draws its window.
class MaskBase : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }
    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }

protected:
    // Output size on the GPU: maskSize's.
    static void gpuSize(const EvalContext& ctx, const std::vector<Value>& in, int& w, int& h) {
        if (!in[0].size(w, h)) resolveSize({}, ctx, w, h);
    }
    // The full image's size, which the shapes are relative to (a region's buffer is part of it).
    static void gpuFullSize(const EvalContext& ctx, const std::vector<Value>& in, int& w, int& h) {
        gpuSize(ctx, in, w, h);
        const PixelFrame fr = frameOf(ctx, w, h);
        w = fr.fullW, h = fr.fullH;
    }
    // The GPU version: body computes `float shape` at pixel position `xy` (full-image pixels).
    void runGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out, const std::string& body,
                std::vector<float> params, int op) {
        gpu::PointOp g;
        gpuSize(ctx, in, g.w, g.h);
        g.functions = kGlslMatte;
        g.body = "    vec2 xy = vec2(p + uOrigin) + 0.5;\n" + body +
                 "    out0 = combineMask(" + std::to_string(op) + ", has0 ? ch0(p) : 0.0, par1(p) * shape);\n";
        g.params = std::move(params);
        g.defaults = {NAN, 1.0f};
        gpu::runPoint(ctx, *this, g, in, out);
    }
};

class ShapeMaskNode : public MaskBase {
protected:
    // pinned param indices: 0 X, 1 Y, 2 Width, 3 Height, 4 Rotation, 5 Feather, 6 Value, 7 Operation
    void run(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out, bool ellipse) {
        ChannelPtr base = toChannel(in[0]);
        int bw, bh;
        maskSize(base, ctx, bw, bh);
        const PixelFrame fr = frameOf(ctx, bw, bh);
        const int w = fr.fullW, h = fr.fullH;
        if (!base) base = std::make_shared<Channel>(Channel::makeConstant(0.0f));
        ChannelPtr val = channelOr(in[1], 1.0f);
        ChannelSampler sb{base.get(), bw, bh}, sv = paramSampler(*this, 1, val, bw, bh);
        const float cx = paramF(0) * w, cy = paramF(1) * h;
        const float hw = std::max(paramF(2) * w * 0.5f, 1e-3f), hh = std::max(paramF(3) * h * 0.5f, 1e-3f);
        const float a = -paramF(4) * kPi / 180.0f, ca = std::cos(a), sa = std::sin(a);
        const float feather = paramF(5);
        const int op = paramI(7);
        out[0] = Value(ChannelPtr(makeChannel(bw, bh, [&](int bx, int by) {
            const int x = bx + fr.x0, y = by + fr.y0;
            float px = x + 0.5f - cx, py = y + 0.5f - cy;
            float rx = px * ca - py * sa, ry = px * sa + py * ca;
            // Normalized "radius": <1 inside. Box uses the max norm, ellipse the Euclidean one.
            float q = ellipse ? std::hypot(rx / hw, ry / hh) : std::max(std::fabs(rx) / hw, std::fabs(ry) / hh);
            float shape = 1.0f - smoothstep(1.0f - feather, 1.0f + 1e-4f, q);
            return combineMask(op, sb(bx, by), sv(bx, by) * shape);
        })));
    }
    void runGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out, bool ellipse) {
        int w, h;
        gpuFullSize(ctx, in, w, h);
        const float hw = std::max(paramF(2) * w * 0.5f, 1e-3f), hh = std::max(paramF(3) * h * 0.5f, 1e-3f);
        const float a = -paramF(4) * kPi / 180.0f;
        std::string body = R"(
    vec2 d = xy - vec2(P[0], P[1]);
    float rx = d.x * P[4] - d.y * P[5], ry = d.x * P[5] + d.y * P[4];
)";
        body += ellipse ? "    float q = length(vec2(rx / P[2], ry / P[3]));\n"
                        : "    float q = max(abs(rx) / P[2], abs(ry) / P[3]);\n";
        body += "    float shape = 1.0 - smoothstepC(1.0 - P[6], 1.0 + 1e-4, q);\n";
        MaskBase::runGpu(ctx, in, out, body, {paramF(0) * w, paramF(1) * h, hw, hh, std::cos(a), std::sin(a), paramF(5)},
                         paramI(7));
    }
};

#define SHAPE_PARAMS_D(wd, ht, fe)                                                                          \
    {ParamDesc::Float("X", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Y", 0.5f, 0.0f, 1.0f),                    \
     ParamDesc::Float("Width", wd, 0.0f, 2.0f), ParamDesc::Float("Height", ht, 0.0f, 2.0f),               \
     ParamDesc::Float("Rotation", 0.0f, -180.0f, 180.0f), ParamDesc::Float("Feather", fe, 0.0f, 1.0f),    \
     ParamDesc::Float("Value", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Operation", 0, {"Add", "Subtract", "Multiply", "Not"})}
#define SHAPE_PARAMS SHAPE_PARAMS_D(0.4f, 0.3f, 0.1f)

class BoxMaskNode : public ShapeMaskNode {
public:
    NODELAB_NODE({"matte.box_mask", "Box Mask", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 6}},
                  {{"Mask", PinType::Channel}},
                  SHAPE_PARAMS})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { run(ctx, in, out, false); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        runGpu(ctx, in, out, false);
    }
};

class EllipseMaskNode : public ShapeMaskNode {
public:
    NODELAB_NODE({"matte.ellipse_mask", "Ellipse Mask", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 6}},
                  {{"Mask", PinType::Channel}},
                  SHAPE_PARAMS})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { run(ctx, in, out, true); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        runGpu(ctx, in, out, true);
    }
};

// Lightroom's Radial Gradient: an ellipse mask with a wide feather. Operation "Not" is its Invert.
class RadialGradientNode : public ShapeMaskNode {
public:
    NODELAB_NODE({"matte.radial_gradient", "Radial Gradient", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 6}},
                  {{"Mask", PinType::Channel}},
                  SHAPE_PARAMS_D(0.6f, 0.6f, 0.5f)})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { run(ctx, in, out, true); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        runGpu(ctx, in, out, true);
    }
};

// Lightroom's Linear Gradient: full strength before Start, fading to nothing at End. Linear
// projects fade evenly across the whole Start-End span, as Lightroom does; legacy ones keep the
// smoothstep they were made with, which packs most of the change into the middle half.
class LinearGradientNode : public MaskBase {
public:
    NODELAB_NODE({"matte.linear_gradient", "Linear Gradient", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 4}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::FloatFree("Start X", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("Start Y", 0.2f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("End X", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("End Y", 0.6f, 0.0f, 1.0f),
                   ParamDesc::Float("Value", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Operation", 0, {"Add", "Subtract", "Multiply", "Not"})}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ChannelPtr base = toChannel(in[0]);
        int bw, bh;
        maskSize(base, ctx, bw, bh);
        const PixelFrame fr = frameOf(ctx, bw, bh);
        const int w = fr.fullW, h = fr.fullH;
        if (!base) base = std::make_shared<Channel>(Channel::makeConstant(0.0f));
        ChannelPtr val = channelOr(in[1], 1.0f);
        ChannelSampler sb{base.get(), bw, bh}, sv = paramSampler(*this, 1, val, bw, bh);
        // In pixels, so the fade stays perpendicular to the Start-End line on non-square images.
        const float x0 = paramF(0) * w, y0 = paramF(1) * h;
        const float dx = paramF(2) * w - x0, dy = paramF(3) * h - y0;
        const float len2 = std::max(dx * dx + dy * dy, 1e-6f);
        const int op = paramI(5);
        const bool even = ctx.linear();
        out[0] = Value(ChannelPtr(makeChannel(bw, bh, [&](int bx, int by) {
            const int x = bx + fr.x0, y = by + fr.y0;
            float t = ((x + 0.5f - x0) * dx + (y + 0.5f - y0) * dy) / len2;
            const float fade = even ? clamp01(t) : smoothstep(0.0f, 1.0f, t);
            return combineMask(op, sb(bx, by), sv(bx, by) * (1.0f - fade));
        })));
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        gpuFullSize(ctx, in, w, h);
        const float x0 = paramF(0) * w, y0 = paramF(1) * h;
        const float dx = paramF(2) * w - x0, dy = paramF(3) * h - y0;
        runGpu(ctx, in, out,
               R"(    float t = dot(xy - vec2(P[0], P[1]), vec2(P[2], P[3])) / P[4];
    float shape = 1.0 - (P[5] > 0.5 ? clamp(t, 0.0, 1.0) : smoothstepC(0.0, 1.0, t));
)",
               {x0, y0, dx, dy, std::max(dx * dx + dy * dy, 1e-6f), ctx.linear() ? 1.0f : 0.0f}, paramI(5));
    }
};

// ---------------------------------------------------------------- keyers

class ChannelKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.channel_key", "Channel Key", "Matte",
                  {{"Image", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Enum("Channel", 1, {"Red", "Green", "Blue", "Hue", "Saturation", "Value", "Y (luma)", "Cb", "Cr"}),
                   ParamDesc::Float("Low", 0.3f, 0.0f, 1.0f), ParamDesc::Float("High", 0.7f, 0.0f, 1.0f),
                   ParamDesc::Bool("Invert", false)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int ch = paramI(0);
        const float lo = paramF(1), hi = paramF(2);
        // Pixels whose channel is at or above High are keyed out; at or below Low are kept.
        keyOutputs(*src, paramB(3), out, [&](const float* s, int, int) {
            float r = clamp01(s[0]), g = clamp01(s[1]), b = clamp01(s[2]), v;
            if (ch < 3) {
                v = s[ch];
            } else if (ch < 6) {
                float hsv[3];
                rgbToHsv(r, g, b, hsv[0], hsv[1], hsv[2]);
                v = hsv[ch - 3];
            } else {
                float ycc[3];
                rgbToYCbCr(r, g, b, ycc[0], ycc[1], ycc[2]);
                v = ycc[ch - 6];
            }
            return 1.0f - smoothstep(lo, std::max(hi, lo + 1e-4f), v);
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuKey(ctx, *this, in, out, R"(
    vec3 c = clamp01(s.rgb);
    float v = CH < 3 ? s[min(CH, 2)] : (CH < 6 ? rgbToHsv(c)[clamp(CH - 3, 0, 2)] : rgbToYCbCr(c)[max(CH - 6, 0)]);
    m = 1.0 - smoothstepC(P[0], max(P[1], P[0] + 1e-4), v);
)",
               {paramF(1), paramF(2)}, paramB(3), false, std::string(kGlslMatte) + "const int CH = " + std::to_string(paramI(0)) + ";\n");
    }
};

class LuminanceKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.luminance_key", "Luminance Key", "Matte",
                  {{"Image", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Float("Low", 0.2f, 0.0f, 1.0f), ParamDesc::Float("High", 0.8f, 0.0f, 1.0f),
                   ParamDesc::Bool("Keep Bright", true)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float lo = paramF(0), hi = std::max(paramF(1), paramF(0) + 1e-4f);
        keyOutputs(*src, !paramB(2), out, [&](const float* s, int, int) {
            return smoothstep(lo, hi, luminance(s[0], s[1], s[2]));
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuKey(ctx, *this, in, out, "    m = smoothstepC(P[0], P[1], luminance(s.rgb));\n",
               {paramF(0), std::max(paramF(1), paramF(0) + 1e-4f)}, !paramB(2), false, kGlslMatte);
    }
};

class DifferenceKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.difference_key", "Difference Key", "Matte",
                  {{"Image", PinType::Image}, {"Key", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Color("Key Color", 0.1f, 0.8f, 0.1f), ParamDesc::Float("Tolerance", 0.1f, 0.0f, 1.0f),
                   ParamDesc::Float("Falloff", 0.1f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        KeySource key{toImage(in[1], src->w, src->h), {}};
        paramC(0, key.color);
        const float tol = paramF(1), fall = std::max(paramF(2), 1e-4f);
        // Largest per-channel difference from the key: similar pixels (small difference) are keyed out.
        keyOutputs(*src, false, out, [&](const float* s, int x, int y) {
            const float* k = key.at(x, y);
            float diff = std::max({std::fabs(s[0] - k[0]), std::fabs(s[1] - k[1]), std::fabs(s[2] - k[2])});
            return (diff - tol) / fall;
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        float c[3];
        paramC(0, c);
        gpuKey(ctx, *this, in, out, R"(
    vec3 d = abs(s.rgb - k);
    m = (max(d.r, max(d.g, d.b)) - P[3]) / P[4];
)",
               {c[0], c[1], c[2], paramF(1), std::max(paramF(2), 1e-4f)}, false, true);
    }
};

class DistanceKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.distance_key", "Distance Key", "Matte",
                  {{"Image", PinType::Image}, {"Key", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Color("Key Color", 0.1f, 0.8f, 0.1f), ParamDesc::Float("Tolerance", 0.1f, 0.0f, 1.0f),
                   ParamDesc::Float("Falloff", 0.1f, 0.0f, 1.0f), ParamDesc::Enum("Space", 0, {"RGB", "YCbCr (ignore brightness)"})}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        KeySource key{toImage(in[1], src->w, src->h), {}};
        paramC(0, key.color);
        const float tol = paramF(1), fall = std::max(paramF(2), 1e-4f);
        const bool ycc = paramI(3) == 1;
        keyOutputs(*src, false, out, [&](const float* s, int x, int y) {
            const float* k = key.at(x, y);
            float d;
            if (ycc) {
                float a[3], b[3];
                rgbToYCbCr(s[0], s[1], s[2], a[0], a[1], a[2]);
                rgbToYCbCr(k[0], k[1], k[2], b[0], b[1], b[2]);
                d = std::hypot(a[1] - b[1], a[2] - b[2]) * 2.0f;
            } else {
                d = std::sqrt((s[0] - k[0]) * (s[0] - k[0]) + (s[1] - k[1]) * (s[1] - k[1]) + (s[2] - k[2]) * (s[2] - k[2])) / 1.732f;
            }
            return (d - tol) / fall;
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        float c[3];
        paramC(0, c);
        gpuKey(ctx, *this, in, out, R"(
    float d = YCC ? length(rgbToYCbCr(s.rgb).yz - rgbToYCbCr(k).yz) * 2.0 : length(s.rgb - k) / 1.732;
    m = (d - P[3]) / P[4];
)",
               {c[0], c[1], c[2], paramF(1), std::max(paramF(2), 1e-4f)}, false, true,
               paramI(3) == 1 ? "const bool YCC = true;\n" : "const bool YCC = false;\n");
    }
};

class ChromaKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.chroma_key", "Chroma Key", "Matte",
                  {{"Image", PinType::Image}, {"Key", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Color("Key Color", 0.1f, 0.8f, 0.1f), ParamDesc::Float("Acceptance", 25.0f, 1.0f, 80.0f),
                   ParamDesc::Float("Falloff", 15.0f, 0.0f, 60.0f), ParamDesc::Float("Min Saturation", 0.2f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        KeySource key{toImage(in[1], src->w, src->h), {}};
        paramC(0, key.color);
        const float acc = paramF(1), fall = std::max(paramF(2), 1e-3f), minSat = paramF(3);
        // Compare hue angles in the CbCr plane; pixels too gray to have a reliable hue are kept.
        keyOutputs(*src, false, out, [&](const float* s, int x, int y) {
            const float* k = key.at(x, y);
            float ys, cbs, crs, yk, cbk, crk;
            rgbToYCbCr(s[0], s[1], s[2], ys, cbs, crs);
            rgbToYCbCr(k[0], k[1], k[2], yk, cbk, crk);
            float ax = cbs - 0.5f, ay = crs - 0.5f, bx = cbk - 0.5f, by = crk - 0.5f;
            float ma = std::hypot(ax, ay), mb = std::hypot(bx, by);
            if (mb < 1e-5f || ma < minSat * mb) return 1.0f;
            float ang = std::acos(std::clamp((ax * bx + ay * by) / (ma * mb), -1.0f, 1.0f)) * 180.0f / kPi;
            return (ang - acc) / fall;
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        float c[3];
        paramC(0, c);
        gpuKey(ctx, *this, in, out, R"(
    vec2 a = rgbToYCbCr(s.rgb).yz - 0.5, b = rgbToYCbCr(k).yz - 0.5;
    float ma = length(a), mb = length(b);
    if (mb < 1e-5 || ma < P[5] * mb) m = 1.0;
    else m = (acos(clamp(dot(a, b) / (ma * mb), -1.0, 1.0)) * (180.0 / 3.14159265) - P[3]) / P[4];
)",
               {c[0], c[1], c[2], paramF(1), std::max(paramF(2), 1e-3f), paramF(3)}, false, true);
    }
};

class ColorSpillNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.color_spill", "Color Spill", "Matte",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Spill Channel", 1, {"Red", "Green", "Blue"}),
                   ParamDesc::Enum("Limit", 1, {"Single (next channel)", "Average of others"}),
                   ParamDesc::Float("Ratio", 1.0f, 0.5f, 1.5f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        const int c = paramI(1), o1 = (c + 1) % 3, o2 = (c + 2) % 3;
        const bool avg = paramI(2) == 1;
        const float ratio = paramF(3);
        // Spill = how far the spill channel exceeds its limit; subtract it.
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            std::copy(s, s + 4, d);
            float limit = avg ? (s[o1] + s[o2]) * 0.5f : s[o1];
            float spill = std::max(0.0f, s[c] - limit * ratio);
            d[c] = s[c] - spill * sf(x, y);
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int c = paramI(1);
        gpu::PointOp g;
        g.functions = "const int C = " + std::to_string(c) + ", O1 = " + std::to_string((c + 1) % 3) +
                      ", O2 = " + std::to_string((c + 2) % 3) + ";\nconst bool AVG = " + (paramI(2) == 1 ? "true" : "false") + ";\n";
        g.body = R"(
    vec4 s = img0(p);
    float limit = AVG ? (s[O1] + s[O2]) * 0.5 : s[O1];
    float spill = max(0.0, s[C] - limit * P[0]);
    out0 = s;
    out0[C] = s[C] - spill * par1(p);
)";
        g.params = {paramF(3)};
        g.defaults = {NAN, 1.0f};
        gpu::runOver(ctx, *this, g, in, out);
    }
};

class DoubleEdgeMaskNode : public Node {
public:
    NODELAB_NODE({"matte.double_edge_mask", "Double Edge Mask", "Matte",
                  {{"Inner Mask", PinType::Channel}, {"Outer Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ChannelPtr inner = toChannel(in[0]), outer = toChannel(in[1]);
        if (!inner || !outer) return;
        int w = !outer->constant ? outer->w : (!inner->constant ? inner->w : ctx.defaultW);
        int h = !outer->constant ? outer->h : (!inner->constant ? inner->h : ctx.defaultH);
        ChannelSampler si{inner.get(), w, h}, so{outer.get(), w, h};
        std::vector<uint8_t> in_(size_t(w) * h), out_(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                in_[size_t(y) * w + x] = si(x, y) >= 0.5f;
                out_[size_t(y) * w + x] = so(x, y) < 0.5f;  // outside of the outer mask
            }
        auto dIn = imageops::distanceTransform(in_, w, h);
        auto dOut = imageops::distanceTransform(out_, w, h);
        // Gradient from 1 at the inner edge to 0 at the outer edge.
        out[0] = Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
            size_t i = size_t(y) * w + x;
            if (in_[i]) return 1.0f;
            if (out_[i]) return 0.0f;
            return dOut[i] / std::max(dIn[i] + dOut[i], 1e-4f);
        })));
    }
};


// ---------------------------------------------------------------- range mask

// Lightroom's Luminance Range / Color Range: selects by the photo's lightness or colour, usually
// to refine another mask (wired into Mask, which it intersects). Both work in Oklab, so Low/High
// are perceptual lightness (0.5 is a mid tone, whatever the working space) and colour distances
// match what the eye sees.
class RangeMaskNode : public Node {
public:
    enum { Mode, Low, High, Smoothness, KeyColor, Amount, Invert };
    NODELAB_NODE({"matte.range_mask", "Range Mask", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Enum("Mode", 0, {"Luminance", "Color"}),
                   ParamDesc::Float("Low", 0.0f, 0.0f, 1.0f).when(0, 0), ParamDesc::Float("High", 1.0f, 0.0f, 1.0f).when(0, 0),
                   ParamDesc::Float("Smoothness", 0.5f, 0.0f, 1.0f).when(0, 0),
                   ParamDesc::Color("Color", 0.5f, 0.5f, 0.5f).when(0, 1), ParamDesc::Float("Amount", 0.5f, 0.0f, 1.0f).when(0, 1),
                   ParamDesc::Bool("Invert", false)}})

    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel

    // The selection's constants: Luminance's edges (low fade start/end, high fade start/end) or
    // Color's key (Oklab) and its distance range.
    struct Setup {
        bool color = false;
        float e[4] = {};
        float key[3] = {};
    };
    Setup setup(const EvalContext& ctx) const {
        Setup st;
        st.color = paramI(Mode) == 1;
        if (!st.color) {
            const float lo = paramF(Low), hi = std::max(paramF(High), lo);
            // Smoothness feathers both ends, by up to a quarter of the lightness range.
            const float f = paramF(Smoothness) * 0.25f + 1e-4f;
            st.e[0] = lo - f, st.e[1] = lo, st.e[2] = hi, st.e[3] = hi + f;
        } else {
            float c[3];
            paramC(KeyColor, c);
            for (float& v : c) v = ctx.linear() ? std::max(v, 0.0f) : srgbToLinear(std::max(v, 0.0f));
            colorsci::rgbToOklab(c, st.key);
            // Amount widens the range of colours taken in, like Lightroom's Refine.
            const float tol = 0.02f + paramF(Amount) * 0.23f;
            st.e[0] = tol * 0.5f, st.e[1] = tol;
        }
        return st;
    }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int w = src->w, h = src->h;
        ChannelPtr base = toChannel(in[1]);
        ChannelSampler sb{base.get(), w, h};
        const Setup st = setup(ctx);
        const bool encoded = !ctx.linear(), inv = paramB(Invert);
        out[0] = Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
            const float* s = src->pixel(size_t(y) * w + x);
            float c[3], lab[3];
            for (int k = 0; k < 3; ++k) c[k] = encoded ? srgbToLinear(std::max(s[k], 0.0f)) : std::max(s[k], 0.0f);
            colorsci::rgbToOklab(c, lab);
            float m;
            if (!st.color) {
                m = smoothstep(st.e[0], st.e[1], lab[0]) * (1.0f - smoothstep(st.e[2], st.e[3], lab[0]));
            } else {
                // Lightness counts half: a colour in shade is still that colour.
                const float dl = (lab[0] - st.key[0]) * 0.5f, da = lab[1] - st.key[1], db = lab[2] - st.key[2];
                m = 1.0f - smoothstep(st.e[0], st.e[1], std::sqrt(dl * dl + da * da + db * db));
            }
            if (inv) m = 1.0f - m;
            return base ? clamp01(sb(x, y)) * m : m;
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const Setup st = setup(ctx);
        gpu::PointOp g;
        g.functions = std::string(kGlslMatte) + "const bool ENCODED = " + (ctx.linear() ? "false" : "true") +
                      ";\nconst bool COLOR = " + (st.color ? "true" : "false") + ";\nconst bool INV = " +
                      (paramB(Invert) ? "true" : "false") + ";\n";
        g.body = R"(
    vec3 c = max(img0(p).rgb, vec3(0.0));
    vec3 lab = rgbToOklab(ENCODED ? srgbToLinear(c) : c);
    float m;
    if (!COLOR) {
        m = smoothstepC(P[0], P[1], lab.x) * (1.0 - smoothstepC(P[2], P[3], lab.x));
    } else {
        vec3 d = (lab - vec3(P[4], P[5], P[6])) * vec3(0.5, 1.0, 1.0);
        m = 1.0 - smoothstepC(P[0], P[1], length(d));
    }
    if (INV) m = 1.0 - m;
    out0 = has1 ? clamp01(ch1(p)) * m : m;
)";
        g.params = {st.e[0], st.e[1], st.e[2], st.e[3], st.key[0], st.key[1], st.key[2]};
        gpu::runOver(ctx, *this, g, in, out);
    }
};

// An HSL qualifier (DaVinci Resolve's, Nuke's HueKeyer): keeps pixels within a hue, saturation
// and lightness range, each feathered by its softness and switched on separately. Measured in
// Oklch like Range Mask, so ranges are even across colours, but hue is in the familiar degrees:
// Oklch hue is bent so that sRGB's primaries and secondaries sit at 0, 60, 120... (red, yellow,
// green, cyan, blue, magenta). Saturation is Oklch chroma over the most saturated sRGB colour's,
// lightness Oklab L (Range Mask's).
class HslMaskNode : public Node {
public:
    enum { UseHue, Hue, HueWidth, HueSoftness, UseSat, SatLow, SatHigh, SatSoftness, UseLight, LightLow, LightHigh,
           LightSoftness, Invert };
    NODELAB_NODE({"matte.hsl_mask", "HSL Mask", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Bool("Use Hue", true), ParamDesc::Float("Hue", 0.0f, 0.0f, 360.0f).withTrack(SliderTrack::Hue).when(UseHue),
                   ParamDesc::Float("Hue Width", 60.0f, 0.0f, 360.0f).when(UseHue),
                   ParamDesc::Float("Hue Softness", 20.0f, 0.0f, 180.0f).when(UseHue),
                   ParamDesc::Bool("Use Saturation", true), ParamDesc::Float("Saturation Low", 0.15f, 0.0f, 1.0f).when(UseSat),
                   ParamDesc::Float("Saturation High", 1.0f, 0.0f, 1.0f).when(UseSat),
                   ParamDesc::Float("Saturation Softness", 0.1f, 0.0f, 1.0f).when(UseSat),
                   ParamDesc::Bool("Use Lightness", false), ParamDesc::Float("Lightness Low", 0.0f, 0.0f, 1.0f).when(UseLight),
                   ParamDesc::Float("Lightness High", 1.0f, 0.0f, 1.0f).when(UseLight),
                   ParamDesc::Float("Lightness Softness", 0.1f, 0.0f, 1.0f).when(UseLight), ParamDesc::Bool("Invert", false)}})

    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel

    // Oklch chroma of sRGB blue, the most saturated sRGB colour: saturation 1.
    static constexpr float kMaxChroma = 0.3132f;
    // Oklch hue (degrees) of red, yellow, green, cyan, blue and magenta, in that order.
    static const std::array<float, 6>& anchors() {
        static const std::array<float, 6> a = [] {
            const float rgb[6][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};
            std::array<float, 6> h{};
            for (int i = 0; i < 6; ++i) {
                float lab[3];
                colorsci::rgbToOklab(rgb[i], lab);
                h[size_t(i)] = std::fmod(std::atan2(lab[2], lab[1]) * 57.2957795f + 360.0f, 360.0f);
            }
            return h;
        }();
        return a;
    }
    // Oklch hue -> HSL-style degrees, linear between the anchors (which increase around the circle
    // from red's).
    static float hueDegrees(float oklch, const std::array<float, 6>& a) {
        const float rel = std::fmod(oklch - a[0] + 720.0f, 360.0f);
        for (int i = 0; i < 6; ++i) {
            const float lo = std::fmod(a[size_t(i)] - a[0] + 360.0f, 360.0f);
            const float hi = i == 5 ? 360.0f : std::fmod(a[size_t(i) + 1] - a[0] + 360.0f, 360.0f);
            if (rel < hi || i == 5) return float(i) * 60.0f + (rel - lo) / std::max(hi - lo, 1e-3f) * 60.0f;
        }
        return 0.0f;
    }

    // The qualifier's constants: hue centre, half width and softness; saturation and lightness
    // edges (fade-in start/end, fade-out start/end); which parts are on.
    std::vector<float> setup() const {
        const auto& a = anchors();
        std::vector<float> P = {paramB(UseHue) ? 1.0f : 0.0f, paramF(Hue), paramF(HueWidth) * 0.5f,
                                paramF(HueSoftness) + 1e-3f, paramB(UseSat) ? 1.0f : 0.0f};
        // A range's edges: fade in from e0 to e1, out from e2 to e3. A range reaching 0 or 1 doesn't
        // fade at that end (saturation 0 is inside "0 to 0.3", and colours outside sRGB can pass 1).
        auto edges = [&](int lo, int hi, int soft) {
            const float l = paramF(lo), u = std::max(paramF(hi), l), f = paramF(soft) * 0.5f + 1e-4f;
            P.push_back(l <= 0.0f ? -1.0f : l - f), P.push_back(l <= 0.0f ? -1.0f : l + f);
            P.push_back(u >= 1.0f ? 1e9f : u - f), P.push_back(u >= 1.0f ? 1e9f : u + f);
        };
        edges(SatLow, SatHigh, SatSoftness);
        P.push_back(paramB(UseLight) ? 1.0f : 0.0f);
        edges(LightLow, LightHigh, LightSoftness);
        P.insert(P.end(), a.begin(), a.end());
        return P;
    }
    static float band(float v, const float* e) { return smoothstep(e[0], e[1], v) * (1.0f - smoothstep(e[2], e[3], v)); }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int w = src->w, h = src->h;
        ChannelPtr base = toChannel(in[1]);
        ChannelSampler sb{base.get(), w, h};
        const std::vector<float> P = setup();
        const std::array<float, 6> a = {P[14], P[15], P[16], P[17], P[18], P[19]};
        const bool encoded = !ctx.linear(), inv = paramB(Invert);
        out[0] = Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
            const float* s = src->pixel(size_t(y) * w + x);
            float c[3], lab[3];
            for (int k = 0; k < 3; ++k) c[k] = encoded ? srgbToLinear(std::max(s[k], 0.0f)) : std::max(s[k], 0.0f);
            colorsci::rgbToOklab(c, lab);
            const float chroma = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
            float m = 1.0f;
            if (P[0] > 0.5f) {
                const float hue = hueDegrees(std::atan2(lab[2], lab[1]) * 57.2957795f, a);
                const float d = std::fabs(std::fmod(hue - P[1] + 540.0f, 360.0f) - 180.0f);
                // Neutral pixels have no hue, so a hue range leaves them out.
                m *= (1.0f - smoothstep(P[2], P[2] + P[3], d)) * smoothstep(0.004f, 0.02f, chroma);
            }
            if (P[4] > 0.5f) m *= band(chroma / kMaxChroma, &P[5]);
            if (P[9] > 0.5f) m *= band(lab[0], &P[10]);
            if (inv) m = 1.0f - m;
            return base ? clamp01(sb(x, y)) * m : m;
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp g;
        g.functions = std::string(kGlslMatte) + "const bool ENCODED = " + (ctx.linear() ? "false" : "true") +
                      ";\nconst bool INV = " + (paramB(Invert) ? "true" : "false") + ";\n" + R"(
float band(float v, float e0, float e1, float e2, float e3) { return smoothstepC(e0, e1, v) * (1.0 - smoothstepC(e2, e3, v)); }
float hueDegrees(float h) {
    float rel = mod(h - P[14] + 720.0, 360.0);
    for (int i = 0; i < 6; ++i) {
        float lo = mod(P[14 + i] - P[14] + 360.0, 360.0);
        float hi = i == 5 ? 360.0 : mod(P[15 + i] - P[14] + 360.0, 360.0);
        if (rel < hi || i == 5) return float(i) * 60.0 + (rel - lo) / max(hi - lo, 1e-3) * 60.0;
    }
    return 0.0;
}
)";
        g.body = R"(
    vec3 c = max(img0(p).rgb, vec3(0.0));
    vec3 lab = rgbToOklab(ENCODED ? srgbToLinear(c) : c);
    float chroma = length(lab.yz);
    float m = 1.0;
    if (P[0] > 0.5) {
        float d = abs(mod(hueDegrees(atan2C(lab.z, lab.y) * 57.2957795) - P[1] + 540.0, 360.0) - 180.0);
        m *= (1.0 - smoothstepC(P[2], P[2] + P[3], d)) * smoothstepC(0.004, 0.02, chroma);
    }
    if (P[4] > 0.5) m *= band(chroma / 0.3132, P[5], P[6], P[7], P[8]);
    if (P[9] > 0.5) m *= band(lab.x, P[10], P[11], P[12], P[13]);
    if (INV) m = 1.0 - m;
    out0 = has1 ? clamp01(ch1(p)) * m : m;
)";
        g.params = setup();
        gpu::runOver(ctx, *this, g, in, out);
    }
};

}  // namespace

// ---------------------------------------------------------------- brush

namespace {

// Auto Mask: how alike two Oklab colours must be to be painted. Full coverage up to kAutoNear,
// none beyond kAutoFar (Oklab distance; 0.02 is about a just-noticeable difference).
constexpr float kAutoNear = 0.03f, kAutoFar = 0.09f;

// The image in Oklab at mask resolution, for Auto Mask (computed once per evaluation).
struct AutoMaskImage {
    std::vector<float> lab;  // w * h * 3
    int w = 0, h = 0;
    const float* at(int x, int y) const { return &lab[(size_t(y) * w + x) * 3]; }
};

AutoMaskImage toOklabPlane(const Image& img, int w, int h, bool encoded) {
    AutoMaskImage a;
    a.w = w, a.h = h;
    a.lab.resize(size_t(w) * h * 3);
    parallelFor(h, [&](int y) {
        const int iy = std::min(img.h - 1, int((y + 0.5f) * img.h / h));
        for (int x = 0; x < w; ++x) {
            const int ix = std::min(img.w - 1, int((x + 0.5f) * img.w / w));
            const float* s = img.pixel(size_t(iy) * img.w + ix);
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = encoded ? srgbToLinear(std::max(s[k], 0.0f)) : std::max(s[k], 0.0f);
            colorsci::rgbToOklab(c, &a.lab[(size_t(y) * w + x) * 3]);
        }
    });
    return a;
}

// The colour under a brush point: an average over a tenth of the radius, so it is the same
// at the proxy and at full resolution (and not one noisy pixel).
std::array<float, 3> keyColour(const AutoMaskImage& a, float px, float py, float r) {
    const int k = std::clamp(int(std::lround(r * 0.1f)), 0, 8);
    const int cx = std::clamp(int(px), 0, a.w - 1), cy = std::clamp(int(py), 0, a.h - 1);
    std::array<float, 3> sum{0, 0, 0};
    int n = 0;
    for (int y = std::max(0, cy - k); y <= std::min(a.h - 1, cy + k); ++y)
        for (int x = std::max(0, cx - k); x <= std::min(a.w - 1, cx + k); ++x, ++n)
            for (int c = 0; c < 3; ++c) sum[c] += a.at(x, y)[c];
    for (float& v : sum) v /= float(std::max(n, 1));
    return sum;
}

bool anyAuto(const std::vector<BrushMaskNode::Stroke>& strokes) {
    for (const BrushMaskNode::Stroke& st : strokes)
        if (st.autoMask) return true;
    return false;
}

// Paints strokes [begin, end) over the mask. `autoImg` may be empty (Auto Mask then does nothing).
void paintRange(std::vector<float>& mask, int w, int h, const std::vector<BrushMaskNode::Stroke>& strokes, size_t begin,
                size_t end, const AutoMaskImage& autoImg) {
    const float longEdge = float(std::max(w, h));
    std::vector<float> cov;  // coverage of the stroke being drawn
    for (size_t si = begin; si < end; ++si) {
        const BrushMaskNode::Stroke& st = strokes[si];
        if (st.pts.empty()) continue;
        if (cov.empty()) cov.assign(mask.size(), 0.0f);
        const float r = std::max(st.radius * longEdge, 0.5f);
        const float inner = r * (1.0f - std::clamp(st.feather, 0.0f, 1.0f));
        // Points in pixels, and the stroke's bounding box.
        std::vector<std::array<float, 2>> p(st.pts.size());
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (size_t i = 0; i < p.size(); ++i) {
            p[i] = {st.pts[i][0] * w, st.pts[i][1] * h};
            bx0 = std::min(bx0, p[i][0]), bx1 = std::max(bx1, p[i][0]);
            by0 = std::min(by0, p[i][1]), by1 = std::max(by1, p[i][1]);
        }
        const int x0 = std::max(0, int(std::floor(bx0 - r))), x1 = std::min(w - 1, int(std::ceil(bx1 + r)));
        const int y0 = std::max(0, int(std::floor(by0 - r))), y1 = std::min(h - 1, int(std::ceil(by1 + r)));
        if (x0 > x1 || y0 > y1) continue;
        // Auto Mask: each point's colour, sampled as Lightroom does under the brush centre.
        const bool autoMask = st.autoMask && !autoImg.lab.empty();
        std::vector<std::array<float, 3>> keys;
        if (autoMask)
            for (const auto& q : p) keys.push_back(keyColour(autoImg, q[0], q[1], r));
        // Within one stroke coverage is the max over its segments, so a slow drag (many points
        // close together) doesn't build up more than a fast one; strokes then accumulate.
        parallelFor(y1 - y0 + 1, [&](int row) {
            const int y = y0 + row;
            const float py = y + 0.5f;
            for (size_t s = 0; s < p.size(); ++s) {
                const auto& a = p[s];
                const auto& b = s + 1 < p.size() ? p[s + 1] : p[s];
                if (py < std::min(a[1], b[1]) - r || py > std::max(a[1], b[1]) + r) continue;
                const int sx0 = std::max(x0, int(std::floor(std::min(a[0], b[0]) - r)));
                const int sx1 = std::min(x1, int(std::ceil(std::max(a[0], b[0]) + r)));
                const float ex = b[0] - a[0], ey = b[1] - a[1], el = ex * ex + ey * ey;
                for (int x = sx0; x <= sx1; ++x) {
                    float& cv = cov[size_t(y) * w + x];
                    // Already at the stroke's full flow from an earlier segment: nothing to add.
                    if (cv >= st.flow) continue;
                    const float px = x + 0.5f;
                    float t = el > 0 ? std::clamp(((px - a[0]) * ex + (py - a[1]) * ey) / el, 0.0f, 1.0f) : 0.0f;
                    const float dx = px - a[0] - t * ex, dy = py - a[1] - t * ey, d2 = dx * dx + dy * dy;
                    // Cheap reject first (with slack); then hypot, as before, so masks stay bit-identical.
                    if (d2 > r * r * 1.001f) continue;
                    const float d = std::hypot(dx, dy);
                    if (d >= r) continue;
                    float c = (1.0f - smoothstep(inner, r, d)) * st.flow;
                    if (autoMask) {
                        // Against the nearer end's colour: a stroke crossing an edge switches
                        // sides at the segment's middle.
                        const auto& k = keys[t < 0.5f || s + 1 >= p.size() ? s : s + 1];
                        const float* q = autoImg.at(x, y);
                        const float dc = std::sqrt((q[0] - k[0]) * (q[0] - k[0]) + (q[1] - k[1]) * (q[1] - k[1]) +
                                                   (q[2] - k[2]) * (q[2] - k[2]));
                        c *= 1.0f - smoothstep(kAutoNear, kAutoFar, dc);
                    }
                    if (c > cv) cv = c;
                }
            }
        });
        parallelFor(y1 - y0 + 1, [&](int row) {
            const int y = y0 + row;
            for (int x = x0; x <= x1; ++x) {
                float& cv = cov[size_t(y) * w + x];
                if (cv <= 0) continue;
                float& m = mask[size_t(y) * w + x];
                m = st.erase ? m * (1.0f - cv) : m + (1.0f - m) * cv;
                cv = 0.0f;
            }
        });
    }
}

// FNV-1a over strokes [0, n): a stroke list's identity for the paint cache and the evaluator.
uint64_t hashStrokes(const std::vector<BrushMaskNode::Stroke>& strokes, size_t n, uint64_t hsh = 1469598103934665603ull) {
    auto mix = [&](const void* data, size_t len) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < len; ++i) hsh = (hsh ^ b[i]) * 1099511628211ull;
    };
    for (size_t i = 0; i < n; ++i) {
        const BrushMaskNode::Stroke& s = strokes[i];
        mix(&s.radius, sizeof s.radius), mix(&s.feather, sizeof s.feather), mix(&s.flow, sizeof s.flow);
        mix(&s.erase, sizeof s.erase), mix(&s.autoMask, sizeof s.autoMask);
        if (!s.pts.empty()) mix(s.pts.data(), s.pts.size() * sizeof s.pts[0]);
    }
    return hsh;
}

// While painting, each new dab re-evaluates the node, and the evaluator rebuilds the graph for
// every job, so state on the node itself doesn't last. This keeps, process-wide, the mask as it
// was before the stroke being painted (and the Oklab image Auto Mask reads), keyed by content:
// size, working space, the upstream values (alive in the evaluator's cache, so a live weak_ptr
// to the same object means the same pixels) and a hash of the strokes painted so far. A dab
// then repaints only the current stroke instead of all of them.
struct PaintCacheEntry {
    int w = 0, h = 0;
    bool encoded = false;
    const void* baseRaw = nullptr;
    std::weak_ptr<const Channel> base;
    const void* imageRaw = nullptr;
    std::weak_ptr<const Image> image;
    size_t count = 0;   // strokes painted into `mask`
    uint64_t hash = 0;  // hashStrokes over them
    std::vector<float> mask;
    std::shared_ptr<const AutoMaskImage> lab;  // when the image is used by Auto Mask
    uint64_t used = 0;
};

std::mutex gPaintCacheMutex;
std::vector<PaintCacheEntry> gPaintCache;  // most recent few: preview and draft sizes
uint64_t gPaintClock = 0;
constexpr size_t kPaintCacheEntries = 3;

template <class T>
bool sameValue(const void* raw, const std::weak_ptr<const T>& weak, const T* cur) {
    if (raw != cur) return false;
    return !cur || weak.lock().get() == cur;
}

}  // namespace

void paintStrokes(std::vector<float>& mask, int w, int h, const std::vector<BrushMaskNode::Stroke>& strokes,
                  const Image* image, bool encoded) {
    AutoMaskImage autoImg;
    if (image && image->w > 0 && image->h > 0 && anyAuto(strokes)) autoImg = toOklabPlane(*image, w, h, encoded);
    paintRange(mask, w, h, strokes, 0, strokes.size(), autoImg);
}

void BrushMaskNode::evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    ChannelPtr base = toChannel(in[0]);
    int w, h;
    maskSize(base, ctx, w, h);
    // The photo Auto Mask strokes follow (nothing to follow without it).
    ImagePtr image = toImage(in.size() > 1 ? in[1] : Value(), 0, 0);
    if (image && (image->w <= 0 || image->h <= 0)) image.reset();
    const bool encoded = !ctx.linear(), useImage = image && anyAuto(strokes);
    const Image* imageKey = useImage ? image.get() : nullptr;
    const size_t n = strokes.size(), prefix = n > 0 ? n - 1 : 0;

    // The longest cached prefix of these strokes, for these inputs.
    std::vector<float> start;
    size_t from = 0;
    std::shared_ptr<const AutoMaskImage> lab;
    {
        std::lock_guard lock(gPaintCacheMutex);
        const PaintCacheEntry* best = nullptr;
        for (const PaintCacheEntry& e : gPaintCache) {
            if (e.w != w || e.h != h || e.encoded != encoded || !sameValue(e.baseRaw, e.base, base.get())) continue;
            if (useImage && !lab && e.lab && sameValue(e.imageRaw, e.image, imageKey)) lab = e.lab;
            if (!sameValue(e.imageRaw, e.image, imageKey) || e.count > prefix) continue;
            if ((!best || e.count > best->count) && hashStrokes(strokes, e.count) == e.hash) best = &e;
        }
        if (best) {
            start = best->mask;
            from = best->count;
            const_cast<PaintCacheEntry*>(best)->used = ++gPaintClock;
        }
    }
    const bool labComputed = useImage && !lab;
    if (labComputed) lab = std::make_shared<AutoMaskImage>(toOklabPlane(*image, w, h, encoded));
    const bool hit = !start.empty();
    static const AutoMaskImage kNoImage;
    const AutoMaskImage& autoImg = lab ? *lab : kNoImage;

    if (start.empty()) {
        start.assign(size_t(w) * h, 0.0f);
        if (base) {
            ChannelSampler sb{base.get(), w, h};
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) start[size_t(y) * w + x] = clamp01(sb(x, y));
            });
        }
    }
    // Everything but the stroke being painted, cached for the next dab.
    paintRange(start, w, h, strokes, from, prefix, autoImg);
    if (!hit || from < prefix || labComputed) {
        std::lock_guard lock(gPaintCacheMutex);
        PaintCacheEntry e;
        e.w = w, e.h = h, e.encoded = encoded;
        e.baseRaw = base.get(), e.base = base;
        e.imageRaw = imageKey;
        if (imageKey) e.image = image;
        e.count = prefix, e.hash = hashStrokes(strokes, prefix);
        e.mask = start;
        e.lab = lab;
        e.used = ++gPaintClock;
        // Replace an entry for the same inputs (it holds a shorter prefix), else the oldest.
        auto same = std::find_if(gPaintCache.begin(), gPaintCache.end(), [&](const PaintCacheEntry& o) {
            return o.w == w && o.h == h && o.encoded == encoded && o.baseRaw == e.baseRaw && o.imageRaw == e.imageRaw;
        });
        if (same == gPaintCache.end() && gPaintCache.size() >= kPaintCacheEntries)
            same = std::min_element(gPaintCache.begin(), gPaintCache.end(),
                                    [](const PaintCacheEntry& a, const PaintCacheEntry& b) { return a.used < b.used; });
        if (same == gPaintCache.end()) gPaintCache.push_back(std::move(e));
        else *same = std::move(e);
    }
    auto ch = std::make_shared<Channel>(Channel::makeSized(w, h));
    ch->data = std::move(start);
    paintRange(ch->data, w, h, strokes, prefix, n, autoImg);
    if (paramB(3))
        for (float& v : ch->data) v = 1.0f - v;
    out[0] = Value(ChannelPtr(ch));
}

void BrushMaskNode::saveExtra(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const Stroke& s : strokes) {
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : s.pts) pts.push_back({p[0], p[1]});
        nlohmann::json o = {{"radius", s.radius}, {"feather", s.feather}, {"flow", s.flow}, {"erase", s.erase}, {"pts", pts}};
        if (s.autoMask) o["auto"] = true;
        arr.push_back(std::move(o));
    }
    j["strokes"] = arr;
}

void BrushMaskNode::loadExtra(const nlohmann::json& j) {
    strokes.clear();
    auto it = j.find("strokes");
    if (it == j.end() || !it->is_array()) return;
    for (const auto& o : *it) {
        if (!o.is_object()) continue;
        Stroke s;
        s.radius = o.value("radius", 0.04f);
        s.feather = o.value("feather", 0.5f);
        s.flow = o.value("flow", 1.0f);
        s.erase = o.value("erase", false);
        s.autoMask = o.value("auto", false);
        if (auto p = o.find("pts"); p != o.end() && p->is_array())
            for (const auto& q : *p)
                if (q.is_array() && q.size() == 2 && q[0].is_number() && q[1].is_number())
                    s.pts.push_back({q[0].get<float>(), q[1].get<float>()});
        strokes.push_back(std::move(s));
    }
}

std::string BrushMaskNode::signatureExtra() const {
    // FNV-1a over the stroke data: cheaper than serializing thousands of points every evaluation.
    return "brush:" + std::to_string(strokes.size()) + ":" + std::to_string(hashStrokes(strokes, strokes.size()));
}

void BrushMaskNode::beginStroke(float u, float v, bool erase) {
    Stroke s;
    s.radius = paramF(0);
    s.feather = paramF(1);
    s.flow = paramF(2);
    s.erase = erase;
    s.autoMask = paramB(4);
    s.pts.push_back({u, v});
    strokes.push_back(std::move(s));
}

bool BrushMaskNode::extendStroke(float u, float v, int imageW, int imageH) {
    if (strokes.empty()) return false;
    Stroke& s = strokes.back();
    const float longEdge = float(std::max(imageW, imageH));
    const auto& last = s.pts.back();
    // Points closer than a fifth of the radius add nothing visible.
    const float d = std::hypot((u - last[0]) * imageW, (v - last[1]) * imageH);
    if (d < std::max(s.radius * longEdge * 0.2f, 1.0f)) return false;
    s.pts.push_back({u, v});
    return true;
}

void registerMatteNodes(NodeRegistry& r) {
    r.add<BoxMaskNode>();
    r.add<EllipseMaskNode>();
    r.add<RadialGradientNode>();
    r.add<LinearGradientNode>();
    r.add<BrushMaskNode>();
    r.add<RangeMaskNode>();
    r.add<HslMaskNode>();
    r.add<SelectSubjectNode>();
    r.add<SelectSkyNode>();
    r.add<ChannelKeyNode>();
    r.add<LuminanceKeyNode>();
    r.add<DifferenceKeyNode>();
    r.add<DistanceKeyNode>();
    r.add<ChromaKeyNode>();
    r.add<ColorSpillNode>();
    r.add<DoubleEdgeMaskNode>();
}
