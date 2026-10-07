#include "core/ColorMath.h"
#include <string>

#include "core/Curve.h"
#include "gpu/PointOp.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace colormath;

// Color adjustment nodes produce displayable color, so their outputs are clamped to 0..1, and
// channels driving a parameter are clamped to that parameter's range.

namespace {

// ---------------------------------------------------------------- split / combine

// Shared implementation for split nodes: fn(r, g, b, out[3]).
template <typename Fn>
void splitImage(const Value& in, std::vector<Value>& out, int count, Fn&& fn) {
    ImagePtr img = toImage(in, 0, 0);
    if (!img) return;
    std::vector<std::shared_ptr<Channel>> ch(count);
    for (auto& c : ch) c = std::make_shared<Channel>(Channel::makeSized(img->w, img->h));
    parallelFor(img->h, [&](int y) {
        float o[4];
        for (int x = 0; x < img->w; ++x) {
            size_t i = size_t(y) * img->w + x;
            const float* p = img->pixel(i);
            fn(p, o);
            for (int k = 0; k < count; ++k) ch[k]->data[i] = o[k];
        }
    });
    for (int k = 0; k < count; ++k) out[k] = Value(ChannelPtr(ch[k]));
}

// Shared implementation for combine nodes: fn(c0, c1, c2, rgbOut[3]); input 3 is alpha.
template <typename Fn>
void combineImage(const Node& node, EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out,
                  const float defs[4], bool clampOut, Fn&& fn) {
    int w, h;
    resolveSize(in, ctx, w, h);
    ChannelPtr c[4];
    for (int k = 0; k < 4; ++k) c[k] = channelOr(in[k], defs[k]);
    auto img = std::make_shared<Image>(w, h);
    parallelFor(h, [&](int y) {
        ChannelSampler s[4] = {paramSampler(node, 0, c[0], w, h), paramSampler(node, 1, c[1], w, h),
                               paramSampler(node, 2, c[2], w, h), paramSampler(node, 3, c[3], w, h)};
        for (int x = 0; x < w; ++x) {
            float* d = img->pixel(size_t(y) * w + x);
            fn(s[0](x, y), s[1](x, y), s[2](x, y), d);
            if (clampOut)
                for (int k = 0; k < 3; ++k) d[k] = clampColor(ctx.linear(), d[k]);
            d[3] = clamp01(s[3](x, y));
        }
    });
    out[0] = Value(ImagePtr(img));
}

// The GPU versions: a split runs over its image (and has nothing to split without one), a
// combine over the size its inputs resolve to.
bool gpuSplit(EvalContext& ctx, const Node& node, const std::vector<Value>& in, std::vector<Value>& out,
              const char* body) {
    gpu::PointOp op;
    if (!in[0].size(op.w, op.h)) return false;
    op.body = body;
    gpu::runPoint(ctx, node, op, in, out);
    return true;
}

void gpuCombine(EvalContext& ctx, const Node& node, const std::vector<Value>& in, std::vector<Value>& out,
                const char* body) {
    gpu::PointOp op;
    resolveSize(in, ctx, op.w, op.h);
    op.body = body;
    gpu::runPoint(ctx, node, op, in, out);
}

class SplitRGBNode : public Node {
public:
    NODELAB_NODE({"color.split_rgb", "Split RGB", "Color",
                  {{"Image", PinType::Image}},
                  {{"R", PinType::Channel}, {"G", PinType::Channel}, {"B", PinType::Channel}, {"A", PinType::Channel}},
                  {}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        splitImage(in[0], out, 4, [](const float* p, float* o) {
            for (int k = 0; k < 4; ++k) o[k] = p[k];
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuSplit(ctx, *this, in, out, "    vec4 c = img0(p);\n    out0 = c.r; out1 = c.g; out2 = c.b; out3 = c.a;");
    }
};

class CombineRGBNode : public Node {
public:
    NODELAB_NODE({"color.combine_rgb", "Combine RGB", "Color",
                  {{"R", PinType::Channel, 0}, {"G", PinType::Channel, 1}, {"B", PinType::Channel, 2}, {"A", PinType::Channel, 3}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::FloatFree("R", 0.0f, 0.0f, 1.0f), ParamDesc::FloatFree("G", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("B", 0.0f, 0.0f, 1.0f), ParamDesc::Float("A", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const float defs[4] = {0, 0, 0, 1};
        // Unclamped like Blender's Combine Color: it also packs data (HDR, masks, signed values).
        combineImage(*this, ctx, in, out, defs, false, [](float r, float g, float b, float* d) {
            d[0] = r, d[1] = g, d[2] = b;
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuCombine(ctx, *this, in, out, "    out0 = vec4(par0(p), par1(p), par2(p), has3 ? clamp01(par3(p)) : 1.0);");
    }
};

class SplitHSVNode : public Node {
public:
    NODELAB_NODE({"color.split_hsv", "Split HSV", "Color",
                  {{"Image", PinType::Image}},
                  {{"H", PinType::Channel}, {"S", PinType::Channel}, {"V", PinType::Channel}, {"A", PinType::Channel}},
                  {}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        splitImage(in[0], out, 4, [lin](const float* p, float* o) {
            rgbToHsv(clampColor(lin, p[0]), clampColor(lin, p[1]), clampColor(lin, p[2]), o[0], o[1], o[2]);
            o[3] = p[3];
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuSplit(ctx, *this, in, out,
                 "    vec4 c = img0(p);\n    vec3 hsv = rgbToHsv(clampColor(uLinear, c.rgb));\n"
                 "    out0 = hsv.x; out1 = hsv.y; out2 = hsv.z; out3 = c.a;");
    }
};

class CombineHSVNode : public Node {
public:
    NODELAB_NODE({"color.combine_hsv", "Combine HSV", "Color",
                  {{"H", PinType::Channel, 0}, {"S", PinType::Channel, 1}, {"V", PinType::Channel, 2}, {"A", PinType::Channel, 3}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::FloatFree("H", 0.0f, 0.0f, 1.0f), ParamDesc::Float("S", 0.0f, 0.0f, 1.0f),
                   ParamDesc::Float("V", 0.0f, 0.0f, 1.0f), ParamDesc::Float("A", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const float defs[4] = {0, 0, 0, 1};
        combineImage(*this, ctx, in, out, defs, true, [](float h, float s, float v, float* d) {
            hsvToRgb(h, s, v, d[0], d[1], d[2]);  // hue wraps, so it is left unclamped
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuCombine(ctx, *this, in, out,
                   "    vec3 c = hsvToRgb(vec3(par0(p), par1(p), par2(p)));\n"
                   "    out0 = vec4(clampColor(uLinear, c), has3 ? clamp01(par3(p)) : 1.0);");
    }
};

class SplitLabNode : public Node {
public:
    NODELAB_NODE({"color.split_lab", "Split Lab", "Color",
                  {{"Image", PinType::Image}},
                  {{"L", PinType::Channel}, {"a", PinType::Channel}, {"b", PinType::Channel}, {"A", PinType::Channel}},
                  {}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        splitImage(in[0], out, 4, [lin](const float* p, float* o) {
            rgbToLab(clampColor(lin, p[0]), clampColor(lin, p[1]), clampColor(lin, p[2]), o[0], o[1], o[2], !lin);
            o[3] = p[3];
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuSplit(ctx, *this, in, out,
                 "    vec4 c = img0(p);\n    vec3 v = rgbToLab(clampColor(uLinear, c.rgb), !uLinear);\n"
                 "    out0 = v.x; out1 = v.y; out2 = v.z; out3 = c.a;");
    }
};

class CombineLabNode : public Node {
public:
    NODELAB_NODE({"color.combine_lab", "Combine Lab", "Color",
                  {{"L", PinType::Channel, 0}, {"a", PinType::Channel, 1}, {"b", PinType::Channel, 2}, {"A", PinType::Channel, 3}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("L", 0.5f, 0.0f, 1.0f), ParamDesc::Float("a", 0.0f, -1.0f, 1.0f),
                   ParamDesc::Float("b", 0.0f, -1.0f, 1.0f), ParamDesc::Float("A", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const float defs[4] = {0.5f, 0, 0, 1};
        const bool lin = ctx.linear();
        combineImage(*this, ctx, in, out, defs, true, [lin](float l, float a, float b, float* d) {
            labToRgb(l, a, b, d[0], d[1], d[2], !lin);
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuCombine(ctx, *this, in, out,
                   "    vec3 c = labToRgb(vec3(has0 ? par0(p) : 0.5, par1(p), par2(p)), !uLinear);\n"
                   "    out0 = vec4(clampColor(uLinear, c), has3 ? clamp01(par3(p)) : 1.0);");
    }
};

class LuminanceNode : public Node {
public:
    NODELAB_NODE({"color.luminance", "Luminance", "Color",
                  {{"Image", PinType::Image}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::Enum("Method", 0, {"Rec.709 luma", "Average", "Max (HSV value)", "Lab lightness"})}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr img = toImage(in[0], 0, 0);
        if (!img) return;
        const int method = paramI(0);
        out[0] = Value(ChannelPtr(makeChannel(img->w, img->h, [&](int x, int y) {
            const float* p = img->pixel(size_t(y) * img->w + x);
            switch (method) {
                case 1: return (p[0] + p[1] + p[2]) / 3.0f;
                case 2: return std::max({p[0], p[1], p[2]});
                case 3: {
                    float L, a, b;
                    rgbToLab(clampColor(lin, p[0]), clampColor(lin, p[1]), clampColor(lin, p[2]), L, a, b, !lin);
                    return L;
                }
                default: return luminance(p[0], p[1], p[2]);
            }
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.functions = "const int METHOD = " + std::to_string(paramI(0)) + ";\n";
        op.body =
            "vec4 s = img0(p);\n"
            "if (METHOD == 1) out0 = (s.r + s.g + s.b) / 3.0;\n"
            "else if (METHOD == 2) out0 = max(s.r, max(s.g, s.b));\n"
            "else if (METHOD == 3) out0 = rgbToLab(clampColor(uLinear, s.rgb), !uLinear).x;\n"
            "else out0 = luminance(s.rgb);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

// ---------------------------------------------------------------- adjustments

class BrightnessContrastNode : public Node {
public:
    NODELAB_NODE({"color.brightness_contrast", "Brightness / Contrast", "Color",
                  {{"Image", PinType::Image}, {"Brightness", PinType::Channel, 0}, {"Contrast", PinType::Channel, 1}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Brightness", 0.0f, -1.0f, 1.0f), ParamDesc::Float("Contrast", 0.0f, -1.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr br = channelOr(in[1], 0.0f), co = channelOr(in[2], 0.0f);
        ChannelSampler sb = paramSampler(*this, 1, br, src->w, src->h), sc = paramSampler(*this, 2, co, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            // Contrast in -1..1 maps to a slope of 0..inf around mid-gray.
            float c = std::min(sc(x, y), 0.999f);
            float slope = (1.0f + c) / (1.0f - c);
            float b = sb(x, y);
            if (lin)
                // Scene values: the same slope in log space around middle grey (0.18), so contrast
                // bends shadows and highlights apart without pushing dark values below zero.
                for (int k = 0; k < 3; ++k) d[k] = clampColor(true, 0.18f * std::pow(std::max(s[k], 0.0f) / 0.18f, slope) + b);
            else
                for (int k = 0; k < 3; ++k) d[k] = clampColor(false, (s[k] - 0.5f) * slope + 0.5f + b);
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 0.0f, 0.0f};
        op.body =
            "vec4 s = img0(p);\n"
            "float c = min(par2(p), 0.999);\n"
            "float slope = (1.0 + c) / (1.0 - c), b = par1(p);\n"
            "vec3 d = uLinear ? 0.18 * powPos(max(s.rgb, 0.0) / 0.18, vec3(slope)) + b : (s.rgb - 0.5) * slope + 0.5 + b;\n"
            "out0 = vec4(clampColor(uLinear, d), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class SaturationNode : public Node {
public:
    NODELAB_NODE({"color.saturation", "Saturation", "Color",
                  {{"Image", PinType::Image}, {"Amount", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Amount", 1.0f, 0.0f, 4.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr amt = channelOr(in[1], 1.0f);
        ChannelSampler sa = paramSampler(*this, 1, amt, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float a = sa(x, y);
            float l = luminance(s[0], s[1], s[2]);
            for (int k = 0; k < 3; ++k) d[k] = clampColor(lin, l + (s[k] - l) * a);
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        op.body =
            "vec4 s = img0(p);\n"
            "float l = luminance(s.rgb);\n"
            "out0 = vec4(clampColor(uLinear, l + (s.rgb - l) * par1(p)), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class HueShiftNode : public Node {
public:
    NODELAB_NODE({"color.hue_shift", "Hue Shift", "Color",
                  {{"Image", PinType::Image}, {"Degrees", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Degrees", 0.0f, -180.0f, 180.0f).withTrack(SliderTrack::HueShift, 0.0f, 180.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr deg = channelOr(in[1], 0.0f);
        ChannelSampler sd = paramSampler(*this, 1, deg, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float h, sat, v;
            rgbToHsv(clampColor(lin, s[0]), clampColor(lin, s[1]), clampColor(lin, s[2]), h, sat, v);
            hsvToRgb(h + sd(x, y) / 360.0f, sat, v, d[0], d[1], d[2]);
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 0.0f};
        op.body =
            "vec4 s = img0(p);\n"
            "vec3 hsv = rgbToHsv(clampColor(uLinear, s.rgb));\n"
            "out0 = vec4(hsvToRgb(vec3(hsv.x + par1(p) / 360.0, hsv.yz)), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class GammaNode : public Node {
public:
    NODELAB_NODE({"color.gamma", "Gamma", "Color",
                  {{"Image", PinType::Image}, {"Gamma", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Gamma", 1.0f, 0.1f, 5.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr gm = channelOr(in[1], 1.0f);
        ChannelSampler sg = paramSampler(*this, 1, gm, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float inv = 1.0f / sg(x, y);  // > 1 brightens midtones, like the Levels gamma
            for (int k = 0; k < 3; ++k) d[k] = std::pow(clampColor(lin, s[k]), inv);
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        op.body =
            "vec4 s = img0(p);\n"
            "out0 = vec4(powPos(clampColor(uLinear, s.rgb), vec3(1.0 / par1(p))), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class ExposureNode : public Node {
public:
    NODELAB_NODE({"color.exposure", "Exposure", "Color",
                  {{"Image", PinType::Image}, {"Stops", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Stops", 0.0f, -5.0f, 5.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr st = channelOr(in[1], 0.0f);
        ChannelSampler ss = paramSampler(*this, 1, st, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float m = std::exp2(ss(x, y));  // scale in linear light, like a camera exposure change
            if (lin)  // unclamped, like Blender's: +1 stop doubles every value, highlights included
                for (int k = 0; k < 3; ++k) d[k] = s[k] * m;
            else
                for (int k = 0; k < 3; ++k) d[k] = clamp01(linearToSrgb(srgbToLinear(clamp01(s[k])) * m));
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 0.0f};
        op.body =
            "vec4 s = img0(p);\n"
            "float m = exp2(par1(p));\n"
            "out0 = vec4(uLinear ? s.rgb * m : clamp01(linearToSrgb(srgbToLinear(clamp01(s.rgb)) * m)), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class InvertNode : public Node {
public:
    NODELAB_NODE({"color.invert", "Invert", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float f = sf(x, y);
            for (int k = 0; k < 3; ++k) {
                float v = clamp01(s[k]);
                d[k] = v + ((1.0f - v) - v) * f;
            }
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        op.body =
            "vec4 s = img0(p);\n"
            "vec3 v = clamp01(s.rgb);\n"
            "out0 = vec4(v + ((1.0 - v) - v) * par1(p), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class LevelsNode : public Node {
public:
    NODELAB_NODE({"color.levels", "Levels", "Color",
                  {{"Image", PinType::Image}, {"In Black", PinType::Channel, 0}, {"In White", PinType::Channel, 1},
                   {"Gamma", PinType::Channel, 2}, {"Out Black", PinType::Channel, 3}, {"Out White", PinType::Channel, 4}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("In Black", 0.0f, 0.0f, 1.0f), ParamDesc::Float("In White", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Gamma", 1.0f, 0.1f, 5.0f), ParamDesc::Float("Out Black", 0.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Out White", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Channel", 0, {"RGB", "Red", "Green", "Blue"})}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int w = src->w, h = src->h;
        ChannelPtr c[5];
        const float defs[5] = {0, 1, 1, 0, 1};
        for (int k = 0; k < 5; ++k) c[k] = channelOr(in[k + 1], defs[k]);
        ChannelSampler s[5] = {paramSampler(*this, 1, c[0], w, h), paramSampler(*this, 2, c[1], w, h),
                               paramSampler(*this, 3, c[2], w, h), paramSampler(*this, 4, c[3], w, h),
                               paramSampler(*this, 5, c[4], w, h)};
        const int only = paramI(5) - 1;  // -1 = all channels
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* p, float* d) {
            float ib = s[0](x, y), iw = s[1](x, y), gm = s[2](x, y), ob = s[3](x, y), ow = s[4](x, y);
            for (int k = 0; k < 3; ++k) {
                if (only >= 0 && k != only) {
                    d[k] = p[k];
                    continue;
                }
                float v = clampColor(lin, (p[k] - ib) / std::max(iw - ib, 1e-4f));
                v = std::pow(v, 1.0f / gm);
                d[k] = clampColor(lin, ob + v * (ow - ob));
            }
            d[3] = p[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        // A channel mask rather than an index: GLSL rejects a constant index of -1 even when unreached.
        const int only = paramI(5) - 1;
        op.functions = "const bool ONLY = " + std::string(only >= 0 ? "true" : "false") + ";\nconst vec3 MASK = vec3(" +
                       std::to_string(only == 0) + ", " + std::to_string(only == 1) + ", " + std::to_string(only == 2) +
                       ");\n";
        op.body =
            "vec4 s = img0(p);\n"
            "float ib = par1(p), iw = par2(p), gm = par3(p), ob = par4(p), ow = par5(p);\n"
            "vec3 v = clampColor(uLinear, (s.rgb - ib) / max(iw - ib, 1e-4));\n"
            "v = clampColor(uLinear, ob + powPos(v, vec3(1.0 / gm)) * (ow - ob));\n"
            "if (ONLY) {\n"
            "    vec3 d = mix(s.rgb, v, MASK);\n"
            "    v = d;\n"
            "}\n"
            "out0 = vec4(v, s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class CurvesNode : public Node {
public:
    NODELAB_NODE({"color.curves", "Curves", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f), ParamDesc::Curve("Curves")}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const nlohmann::json& cj = params[1];
        auto lut = [&](const char* key) {
            return curveLut(curveFromJson(cj.is_object() && cj.contains(key) ? cj[key] : nlohmann::json()));
        };
        const std::vector<float> master = lut("master"), ch[3] = {lut("r"), lut("g"), lut("b")};
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float f = sf(x, y);
            for (int k = 0; k < 3; ++k) {
                // Per-channel curve first, then the master curve (same order as Blender / Photoshop).
                float v = lutLookup(master, lutLookup(ch[k], clamp01(s[k])));
                d[k] = s[k] + (v - s[k]) * f;
            }
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        const nlohmann::json& cj = params[1];
        // Master, then R, G and B, as one table.
        for (const char* key : {"master", "r", "g", "b"}) {
            const std::vector<float> t = curveLut(curveFromJson(cj.is_object() && cj.contains(key) ? cj[key] : nlohmann::json()));
            op.lut.insert(op.lut.end(), t.begin(), t.end());
        }
        op.functions = "const int N = " + std::to_string(op.lut.size() / 4) + ";\n";
        op.body =
            "vec4 s = img0(p);\n"
            "float f = par1(p);\n"
            "vec3 d;\n"
            "for (int k = 0; k < 3; ++k) {\n"
            "    float v = lutLookup(0, N, lutLookup(N * (k + 1), N, clamp01(s[k])));\n"
            "    d[k] = s[k] + (v - s[k]) * f;\n"
            "}\n"
            "out0 = vec4(d, s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};


// ---------------------------------------------------------------- more color spaces

#define SPLIT3_NODE(Cls, type, title, n0, n1, n2, conv, glsl)                                          \
    class Cls : public Node {                                                                          \
    public:                                                                                            \
        NODELAB_NODE({type, title, "Color", {{"Image", PinType::Image}},                               \
                      {{n0, PinType::Channel}, {n1, PinType::Channel}, {n2, PinType::Channel}, {"A", PinType::Channel}}, \
                      {}})                                                                             \
        void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { \
            const bool lin = ctx.linear();                                                             \
            splitImage(in[0], out, 4, [lin](const float* p, float* o) {                                \
                conv(clampColor(lin, p[0]), clampColor(lin, p[1]), clampColor(lin, p[2]), o[0], o[1], o[2]); \
                o[3] = p[3];                                                                           \
            });                                                                                        \
        }                                                                                              \
        bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override {           \
            return gpu::sizedValue(in[0]);                                                             \
        }                                                                                              \
        void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { \
            gpuSplit(ctx, *this, in, out,                                                              \
                     "    vec4 c = img0(p);\n    vec3 v = " glsl "(clampColor(uLinear, c.rgb));\n"        \
                     "    out0 = v.x; out1 = v.y; out2 = v.z; out3 = c.a;");                           \
        }                                                                                              \
    };

#define COMBINE3_NODE(Cls, type, title, n0, n1, n2, d0, lo1, hi1, d1, lo2, hi2, d2, conv, glsl)         \
    class Cls : public Node {                                                                          \
    public:                                                                                            \
        NODELAB_NODE({type, title, "Color",                                                            \
                      {{n0, PinType::Channel, 0}, {n1, PinType::Channel, 1}, {n2, PinType::Channel, 2}, {"A", PinType::Channel, 3}}, \
                      {{"Image", PinType::Image}},                                                     \
                      {ParamDesc::Float(n0, d0, 0.0f, 1.0f), ParamDesc::Float(n1, d1, lo1, hi1),       \
                       ParamDesc::Float(n2, d2, lo2, hi2), ParamDesc::Float("A", 1.0f, 0.0f, 1.0f)}})  \
        void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { \
            const float defs[4] = {d0, d1, d2, 1};                                                     \
            combineImage(*this, ctx, in, out, defs, true, [](float a, float b, float c, float* d) {    \
                conv(a, b, c, d[0], d[1], d[2]);                                                       \
            });                                                                                        \
        }                                                                                              \
        bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; } \
        void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { \
            gpu::PointOp op;                                                                           \
            resolveSize(in, ctx, op.w, op.h);                                                          \
            op.defaults = {d0, d1, d2, 1.0f};                                                          \
            op.body = "    vec3 c = " glsl "(vec3(par0(p), par1(p), par2(p)));\n"                      \
                      "    out0 = vec4(clampColor(uLinear, c), clamp01(par3(p)));";                    \
            gpu::runPoint(ctx, *this, op, in, out);                                                    \
        }                                                                                              \
    };

SPLIT3_NODE(SplitYCbCrNode, "color.split_ycbcr", "Split YCbCr", "Y", "Cb", "Cr", rgbToYCbCr, "rgbToYCbCr")
COMBINE3_NODE(CombineYCbCrNode, "color.combine_ycbcr", "Combine YCbCr", "Y", "Cb", "Cr", 0.5f, 0.0f, 1.0f, 0.5f, 0.0f, 1.0f, 0.5f, yCbCrToRgb, "yCbCrToRgb")
SPLIT3_NODE(SplitYUVNode, "color.split_yuv", "Split YUV", "Y", "U", "V", rgbToYuv, "rgbToYuv")
COMBINE3_NODE(CombineYUVNode, "color.combine_yuv", "Combine YUV", "Y", "U", "V", 0.5f, -0.5f, 0.5f, 0.0f, -0.7f, 0.7f, 0.0f, yuvToRgb, "yuvToRgb")
SPLIT3_NODE(SplitHSLNode, "color.split_hsl", "Split HSL", "H", "S", "L", rgbToHsl, "rgbToHsl")
COMBINE3_NODE(CombineHSLNode, "color.combine_hsl", "Combine HSL", "H", "S", "L", 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f, hslToRgb, "hslToRgb")

// ---------------------------------------------------------------- grading

nlohmann::json flatHueCurves() {
    nlohmann::json flat = nlohmann::json::array();
    for (int i = 0; i <= 6; ++i) flat.push_back({i / 6.0f, 0.5f});
    return {{"h", flat}, {"s", flat}, {"v", flat}};
}

class HueCorrectNode : public Node {
public:
    NODELAB_NODE({"color.hue_correct", "Hue Correct", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::CurveKeys("Hue Curves", {"h:Hue", "s:Saturation", "v:Value", "@hue"}, flatHueCurves())}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const nlohmann::json& cj = params[1];
        auto lut = [&](const char* key) {
            return curveLut(curveFromJson(cj.is_object() && cj.contains(key) ? cj[key] : nlohmann::json()));
        };
        const std::vector<float> lh = lut("h"), ls = lut("s"), lv = lut("v");
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        // Each curve is a function of the pixel's hue; 0.5 means "no change".
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float h, sat, v;
            rgbToHsv(clampColor(lin, s[0]), clampColor(lin, s[1]), clampColor(lin, s[2]), h, sat, v);
            float nh = h + (lutLookup(lh, h) - 0.5f);
            float ns = clamp01(sat * lutLookup(ls, h) * 2.0f);
            float nv = clampColor(lin, v * lutLookup(lv, h) * 2.0f);
            float r, g, b;
            hsvToRgb(nh, ns, nv, r, g, b);
            float f = sf(x, y);
            d[0] = s[0] + (r - s[0]) * f;
            d[1] = s[1] + (g - s[1]) * f;
            d[2] = s[2] + (b - s[2]) * f;
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        const nlohmann::json& cj = params[1];
        for (const char* key : {"h", "s", "v"}) {
            const std::vector<float> t = curveLut(curveFromJson(cj.is_object() && cj.contains(key) ? cj[key] : nlohmann::json()));
            op.lut.insert(op.lut.end(), t.begin(), t.end());
        }
        op.functions = "const int N = " + std::to_string(op.lut.size() / 3) + ";\n";
        op.body =
            "vec4 s = img0(p);\n"
            "vec3 hsv = rgbToHsv(clampColor(uLinear, s.rgb));\n"
            "float hu = hsv.x;\n"
            "float nh = hu + (lutLookup(0, N, hu) - 0.5);\n"
            "float ns = clamp01(hsv.y * lutLookup(N, N, hu) * 2.0);\n"
            "float nv = clampColor(uLinear, hsv.z * lutLookup(2 * N, N, hu) * 2.0);\n"
            "out0 = vec4(s.rgb + (hsvToRgb(vec3(nh, ns, nv)) - s.rgb) * par1(p), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class ColorBalanceNode : public Node {
public:
    NODELAB_NODE({"color.color_balance", "Color Balance", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Mode", 0, {"Lift / Gamma / Gain", "Offset / Power / Slope"}),
                   ParamDesc::ColorGamma("Lift", 1, 1, 1, 2), ParamDesc::ColorGamma("Gamma", 1, 1, 1, 2), ParamDesc::ColorGamma("Gain", 1, 1, 1, 2),
                   ParamDesc::ColorGamma("Offset", 0, 0, 0, 1), ParamDesc::ColorGamma("Power", 1, 1, 1, 2), ParamDesc::ColorGamma("Slope", 1, 1, 1, 2)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const bool cdl = paramI(1) == 1, lin = ctx.linear();
        float lift[3], gamma[3], gain[3], offset[3], power[3], slope[3];
        paramC(2, lift), paramC(3, gamma), paramC(4, gain), paramC(5, offset), paramC(6, power), paramC(7, slope);
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float f = sf(x, y);
            for (int k = 0; k < 3; ++k) {
                float c = clampColor(lin, s[k]), v;
                if (cdl) {
                    // ASC CDL: out = (in * slope + offset) ^ power
                    v = std::pow(std::max(c * slope[k] + offset[k], 0.0f), power[k]);
                } else {
                    // Lift raises shadows, gain scales highlights, gamma bends midtones (1 = neutral).
                    float lifted = (c - 1.0f) * (2.0f - lift[k]) + 1.0f;
                    v = std::pow(std::max(lifted * gain[k], 0.0f), 1.0f / std::max(gamma[k], 1e-3f));
                }
                d[k] = clampColor(lin, c + (v - c) * f);
            }
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.defaults = {NAN, 1.0f};
        for (int i = 2; i < 8; ++i) {
            float c[3];
            paramC(i, c);
            op.params.insert(op.params.end(), c, c + 3);
        }
        op.functions = std::string("const bool CDL = ") + (paramI(1) == 1 ? "true" : "false") + ";\n";
        op.body =
            "vec4 s = img0(p);\n"
            "vec3 c = clampColor(uLinear, s.rgb), v;\n"
            "vec3 lift = vec3(P[0], P[1], P[2]), gamma = vec3(P[3], P[4], P[5]), gain = vec3(P[6], P[7], P[8]);\n"
            "vec3 offset = vec3(P[9], P[10], P[11]), power = vec3(P[12], P[13], P[14]), slope = vec3(P[15], P[16], P[17]);\n"
            "if (CDL) v = powPos(max(c * slope + offset, 0.0), power);\n"
            "else v = powPos(max(((c - 1.0) * (2.0 - lift) + 1.0) * gain, 0.0), 1.0 / max(gamma, 1e-3));\n"
            "out0 = vec4(clampColor(uLinear, c + (v - c) * par1(p)), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class ToneMapNode : public Node {
public:
    NODELAB_NODE({"color.tone_map", "Tone Map", "Color",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Exposure", 0.0f, -4.0f, 4.0f), ParamDesc::Float("White Point", 2.0f, 1.0f, 16.0f),
                   ParamDesc::Enum("Operator", 0, {"Reinhard", "Filmic (ACES fit)"})}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const bool lin = ctx.linear();
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float m = std::exp2(paramF(0)), wp = paramF(1);
        const bool aces = paramI(2) == 1;
        // Works on unclamped linear light, so it can compress results of Add / Exposure chains
        // that went above 1 (turn off Clamp upstream).
        out[0] = Value(ImagePtr(mapImage(*src, [&](int, int, const float* s, float* d) {
            for (int k = 0; k < 3; ++k) {
                float x = std::max(s[k], 0.0f), v;
                x = (lin ? x : srgbToLinear(x)) * m;
                if (aces) v = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
                else v = x * (1.0f + x / (wp * wp)) / (1.0f + x);
                d[k] = lin ? clamp01(v) : clamp01(linearToSrgb(clamp01(v)));
            }
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.params = {std::exp2(paramF(0)), paramF(1)};
        op.functions = std::string("const bool ACES = ") + (paramI(2) == 1 ? "true" : "false") + ";\n";
        op.body =
            "vec4 s = img0(p);\n"
            "vec3 x = max(s.rgb, 0.0);\n"
            "x = (uLinear ? x : srgbToLinear(x)) * P[0];\n"
            "vec3 v = ACES ? (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14) : x * (1.0 + x / (P[1] * P[1])) / (1.0 + x);\n"
            "out0 = vec4(uLinear ? clamp01(v) : clamp01(linearToSrgb(clamp01(v))), s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

class ConvertColorspaceNode : public Node {
public:
    NODELAB_NODE({"color.convert_colorspace", "Convert Colorspace", "Color",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Enum("Conversion", 0, {"sRGB -> Linear", "Linear -> sRGB", "sRGB -> Gamma 2.2", "Gamma 2.2 -> sRGB"})}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int mode = paramI(0);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int, int, const float* s, float* d) {
            for (int k = 0; k < 3; ++k) {
                float c = std::max(s[k], 0.0f);
                switch (mode) {
                    case 1: d[k] = linearToSrgb(c); break;
                    case 2: d[k] = std::pow(srgbToLinear(c), 1.0f / 2.2f); break;
                    case 3: d[k] = linearToSrgb(std::pow(c, 2.2f)); break;
                    default: d[k] = srgbToLinear(c); break;
                }
            }
            d[3] = s[3];
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.functions = "const int MODE = " + std::to_string(paramI(0)) + ";\n";
        op.body =
            "vec4 s = img0(p);\n"
            "vec3 c = max(s.rgb, 0.0), d;\n"
            "if (MODE == 1) d = linearToSrgb(c);\n"
            "else if (MODE == 2) d = powPos(srgbToLinear(c), vec3(1.0 / 2.2));\n"
            "else if (MODE == 3) d = linearToSrgb(powPos(c, vec3(2.2)));\n"
            "else d = srgbToLinear(c);\n"
            "out0 = vec4(d, s.a);\n";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

}  // namespace

void registerColorNodes(NodeRegistry& r) {
    r.add<BrightnessContrastNode>();
    r.add<SaturationNode>();
    r.add<HueShiftNode>();
    r.add<ExposureNode>();
    r.add<GammaNode>();
    r.add<LevelsNode>();
    r.add<CurvesNode>();
    r.add<InvertNode>();
    r.add<SplitRGBNode>();
    r.add<CombineRGBNode>();
    r.add<SplitHSVNode>();
    r.add<CombineHSVNode>();
    r.add<SplitLabNode>();
    r.add<CombineLabNode>();
    r.add<LuminanceNode>();
    r.add<HueCorrectNode>();
    r.add<ColorBalanceNode>();
    r.add<ToneMapNode>();
    r.add<ConvertColorspaceNode>();
    r.add<SplitYCbCrNode>();
    r.add<CombineYCbCrNode>();
    r.add<SplitYUVNode>();
    r.add<CombineYUVNode>();
    r.add<SplitHSLNode>();
    r.add<CombineHSLNode>();
}
