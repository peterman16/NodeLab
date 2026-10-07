#include <cmath>

#include "core/ColorMath.h"
#include "core/Ramp.h"
#include "gpu/PointOp.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

// Per-pixel function of N channel inputs. If none of them has a resolution the result is a
// constant (sizeless) channel, so scalar math stays cheap.
template <int N, typename Fn>
Value channelOp(const Node& node, const std::vector<Value>& in, const int (&pins)[N], const float (&defs)[N], Fn&& fn) {
    ChannelPtr c[N];
    int w = 0, h = 0;
    bool sized = false;
    for (int k = 0; k < N; ++k) {
        c[k] = channelOr(in[pins[k]], defs[k]);
        if (!c[k]->constant && !sized) {
            w = c[k]->w;
            h = c[k]->h;
            sized = true;
        }
    }
    ChannelSampler s[N];
    for (int k = 0; k < N; ++k) s[k] = paramSampler(node, pins[k], c[k], sized ? w : 1, sized ? h : 1);
    if (!sized) {
        float v[N];
        for (int k = 0; k < N; ++k) v[k] = s[k](0, 0);
        return Value(ChannelPtr(std::make_shared<Channel>(Channel::makeConstant(fn(v)))));
    }
    return Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
        float v[N];
        for (int k = 0; k < N; ++k) v[k] = s[k](x, y);
        return fn(v);
    })));
}

float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 1 inside [lo, hi], fading to 0 over `soft` outside it.
float band(float v, float lo, float hi, float soft) {
    return smoothstep(lo - soft, lo, v) * (1.0f - smoothstep(hi, hi + soft, v));
}

// The GPU versions of the two above.
const char* const kGlslBand = R"(
float sstep(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
float band(float v, float lo, float hi, float soft) { return sstep(lo - soft, lo, v) * (1.0 - sstep(hi, hi + soft, v)); }
)";

// channelOp on the GPU, sized like it (by the first input with pixels; missing inputs read as
// `defaults`). Inputs that are all sizeless stay on the CPU, where the result is one number.
bool gpuChannelOpSupported(const std::vector<Value>& in) {
    for (const Value& v : in)
        if (gpu::sizedValue(v)) return true;
    return false;
}

void gpuChannelOp(EvalContext& ctx, const Node& node, gpu::PointOp op, std::vector<float> defaults,
                  const std::vector<Value>& in, std::vector<Value>& out) {
    for (const Value& v : in)
        if (gpu::sizedValue(v) && v.size(op.w, op.h)) break;
    op.defaults = std::move(defaults);
    gpu::runPoint(ctx, node, op, in, out);
}

// ---------------------------------------------------------------- ramps / keys

class ColorRampNode : public Node {
public:
    NODELAB_NODE({"conv.color_ramp", "Color Ramp", "Converter",
                  {{"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}, {"Alpha", PinType::Channel}},
                  {ParamDesc::Float("Factor", 0.5f, 0.0f, 1.0f), ParamDesc::Ramp("Ramp")}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ColorRamp ramp = rampFromJson(params[1]);
        ChannelPtr fac = channelOr(in[0], 0.5f);
        int w = fac->constant ? ctx.defaultW : fac->w, h = fac->constant ? ctx.defaultH : fac->h;
        ChannelSampler sf = paramSampler(*this, 0, fac, w, h);
        auto img = std::make_shared<Image>(w, h);
        auto alpha = std::make_shared<Channel>(Channel::makeSized(w, h));
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                size_t i = size_t(y) * w + x;
                float c[4];
                ramp.eval(sf(x, y), c);
                float* d = img->pixel(i);
                d[0] = c[0], d[1] = c[1], d[2] = c[2], d[3] = 1.0f;
                alpha->data[i] = c[3];
            }
        });
        out[0] = Value(ImagePtr(img));
        out[1] = Value(ChannelPtr(alpha));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const ColorRamp ramp = rampFromJson(params[1]);
        gpu::PointOp op;
        if (!in[0].size(op.w, op.h)) op.w = ctx.defaultW, op.h = ctx.defaultH;
        // The stops as params (position, RGBA); their count and the interpolation compiled in.
        for (const RampStop& s : ramp.stops) op.params.insert(op.params.end(), {s.pos, s.c[0], s.c[1], s.c[2], s.c[3]});
        op.functions = "const int N = " + std::to_string(ramp.stops.size()) + ", INTERP = " + std::to_string(ramp.interp) +
                       ";\n" + R"(
// max(): with no stops, N - 1 would be a constant out-of-range index, which fails to compile.
float stopPos(int i) { return P[max(i, 0) * 5]; }
vec4 stopColor(int i) { i = max(i, 0); return vec4(P[i * 5 + 1], P[i * 5 + 2], P[i * 5 + 3], P[i * 5 + 4]); }
// As ColorRamp::eval.
vec4 ramp(float t) {
    if (N == 0) return vec4(vec3(t), 1.0);
    if (t <= stopPos(0)) return stopColor(0);
    if (t >= stopPos(N - 1)) return stopColor(N - 1);
    int lo = 0, hi = N - 1;
    for (int i = 0; i + 1 < N; ++i)
        if (t >= stopPos(i) && t <= stopPos(i + 1)) {
            lo = i;
            hi = i + 1;
            break;
        }
    float f = (t - stopPos(lo)) / max(stopPos(hi) - stopPos(lo), 1e-6);
    if (INTERP == 1) f = 0.0;
    else if (INTERP == 2) f = f * f * (3.0 - 2.0 * f);
    else if (INTERP == 3) f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return stopColor(lo) + (stopColor(hi) - stopColor(lo)) * f;
}
)";
        op.body = "    vec4 c = ramp(has0 ? par0(p) : 0.5);\n    out0 = vec4(c.rgb, 1.0);\n    out1 = c.a;";
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

class ColorKeyNode : public Node {
public:
    NODELAB_NODE({"conv.color_key", "Color Key", "Converter",
                  {{"Image", PinType::Image}},
                  {{"Mask", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Float("Hue", 120.0f, 0.0f, 360.0f).withTrack(SliderTrack::Hue), ParamDesc::Float("Hue Range", 30.0f, 0.0f, 180.0f),
                   ParamDesc::Float("Sat Min", 0.15f, 0.0f, 1.0f), ParamDesc::Float("Sat Max", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Value Min", 0.05f, 0.0f, 1.0f), ParamDesc::Float("Value Max", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Softness", 0.1f, 0.0f, 0.5f), ParamDesc::Bool("Invert", false)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float hue = paramF(0) / 360.0f, range = paramF(1) / 360.0f;
        const float smin = paramF(2), smax = paramF(3), vmin = paramF(4), vmax = paramF(5), soft = paramF(6);
        const bool inv = paramB(7);
        auto mask = std::make_shared<Channel>(Channel::makeSized(src->w, src->h));
        auto keyed = mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float h, sat, v;
            colormath::rgbToHsv(clamp01(s[0]), clamp01(s[1]), clamp01(s[2]), h, sat, v);
            float dh = std::fabs(h - hue);
            dh = std::min(dh, 1.0f - dh);  // hue is circular
            float m = (1.0f - smoothstep(range, range + soft * 0.5f, dh)) * band(sat, smin, smax, soft) *
                      band(v, vmin, vmax, soft);
            if (range >= 0.5f) m = band(sat, smin, smax, soft) * band(v, vmin, vmax, soft);
            if (inv) m = 1.0f - m;
            mask->data[size_t(y) * src->w + x] = m;
            for (int k = 0; k < 3; ++k) d[k] = s[k] * m;  // keyed color, black elsewhere
            d[3] = s[3];
        });
        out[0] = Value(ChannelPtr(mask));
        out[1] = Value(ImagePtr(keyed));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.params = {paramF(0) / 360.0f, paramF(1) / 360.0f, paramF(2), paramF(3), paramF(4), paramF(5), paramF(6)};
        op.functions = kGlslBand + std::string("const bool INV = ") + (paramB(7) ? "true" : "false") + ";\n";
        op.body = R"(
    vec4 s = img0(p);
    vec3 hsv = rgbToHsv(clamp01(s.rgb));
    float dh = abs(hsv.x - P[0]);
    dh = min(dh, 1.0 - dh);
    float m = band(hsv.y, P[2], P[3], P[6]) * band(hsv.z, P[4], P[5], P[6]);
    if (P[1] < 0.5) m *= 1.0 - sstep(P[1], P[1] + P[6] * 0.5, dh);
    if (INV) m = 1.0 - m;
    out0 = m;
    out1 = vec4(s.rgb * m, s.a);)";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

// ---------------------------------------------------------------- scalar math

class MapRangeNode : public Node {
public:
    NODELAB_NODE({"conv.map_range", "Map Range", "Converter",
                  {{"Value", PinType::Channel, 0}, {"From Min", PinType::Channel, 1}, {"From Max", PinType::Channel, 2},
                   {"To Min", PinType::Channel, 3}, {"To Max", PinType::Channel, 4}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::FloatFree("Value", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("From Min", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("From Max", 1.0f, 0.0f, 1.0f), ParamDesc::FloatFree("To Min", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("To Max", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Enum("Interpolation", 0, {"Linear", "Smooth Step", "Stepped (4)", "Stepped (8)"}),
                   ParamDesc::Bool("Clamp", true)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int interp = paramI(5);
        const bool clampOut = paramB(6);
        const int pins[5] = {0, 1, 2, 3, 4};
        const float defs[5] = {0.5f, 0, 1, 0, 1};
        out[0] = channelOp<5>(*this, in, pins, defs, [&](const float* v) {
            float t = (v[0] - v[1]) / (std::fabs(v[2] - v[1]) < 1e-9f ? 1e-9f : (v[2] - v[1]));
            if (clampOut) t = std::clamp(t, 0.0f, 1.0f);
            if (interp == 1) t = smoothstep(0.0f, 1.0f, t);
            else if (interp == 2) t = std::floor(t * 4.0f) / 4.0f;
            else if (interp == 3) t = std::floor(t * 8.0f) / 8.0f;
            return v[3] + t * (v[4] - v[3]);
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpuChannelOpSupported(in); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.functions = kGlslBand + ("const int INTERP = " + std::to_string(paramI(5)) + ";\nconst bool CLAMP = ") +
                       (paramB(6) ? "true" : "false") + ";\n";
        op.body = R"(
    float v0 = par0(p), v1 = par1(p), v2 = par2(p);
    float t = (v0 - v1) / (abs(v2 - v1) < 1e-9 ? 1e-9 : (v2 - v1));
    if (CLAMP) t = clamp(t, 0.0, 1.0);
    if (INTERP == 1) t = sstep(0.0, 1.0, t);
    else if (INTERP == 2) t = floor(t * 4.0) / 4.0;
    else if (INTERP == 3) t = floor(t * 8.0) / 8.0;
    out0 = par3(p) + t * (par4(p) - par3(p));)";
        gpuChannelOp(ctx, *this, op, {0.5f, 0, 1, 0, 1}, in, out);
    }
};

class ClampNode : public Node {
public:
    NODELAB_NODE({"conv.clamp", "Clamp", "Converter",
                  {{"Value", PinType::Channel, 0}, {"Min", PinType::Channel, 1}, {"Max", PinType::Channel, 2}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::FloatFree("Value", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("Min", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("Max", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int pins[3] = {0, 1, 2};
        const float defs[3] = {0.5f, 0, 1};
        out[0] = channelOp<3>(*this, in, pins, defs, [](const float* v) {
            return std::clamp(v[0], std::min(v[1], v[2]), std::max(v[1], v[2]));
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpuChannelOpSupported(in); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.body = "    out0 = clamp(par0(p), min(par1(p), par2(p)), max(par1(p), par2(p)));";
        gpuChannelOp(ctx, *this, op, {0.5f, 0, 1}, in, out);
    }
};

class ThresholdNode : public Node {
public:
    NODELAB_NODE({"conv.threshold", "Threshold", "Converter",
                  {{"Value", PinType::Channel, 0}, {"Threshold", PinType::Channel, 1}, {"Softness", PinType::Channel, 2}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Float("Value", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Threshold", 0.5f, 0.0f, 1.0f),
                   ParamDesc::Float("Softness", 0.0f, 0.0f, 0.5f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int pins[3] = {0, 1, 2};
        const float defs[3] = {0.5f, 0.5f, 0};
        out[0] = channelOp<3>(*this, in, pins, defs, [](const float* v) {
            return smoothstep(v[1] - v[2], v[1] + v[2], v[0]);
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpuChannelOpSupported(in); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.functions = kGlslBand;
        op.body = "    out0 = sstep(par1(p) - par2(p), par1(p) + par2(p), par0(p));";
        gpuChannelOp(ctx, *this, op, {0.5f, 0.5f, 0}, in, out);
    }
};

class MathNode : public Node {
public:
    enum Op {
        Add, Subtract, Multiply, Divide, Power, Logarithm, SquareRoot, Absolute, Minimum, Maximum, LessThan,
        GreaterThan, Modulo, Floor, Ceil, Round, Fract, Sine, Cosine, Snap, PingPong
    };
    NODELAB_NODE({"conv.math", "Math", "Converter",
                  {{"A", PinType::Channel, 0}, {"B", PinType::Channel, 1}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::FloatFree("A", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("B", 0.5f, 0.0f, 1.0f),
                   ParamDesc::Enum("Operation", Add,
                                   {"Add", "Subtract", "Multiply", "Divide", "Power", "Logarithm (base B)", "Square Root",
                                    "Absolute", "Minimum", "Maximum", "Less Than", "Greater Than", "Modulo", "Floor",
                                    "Ceil", "Round", "Fraction", "Sine", "Cosine", "Snap (to B)", "Ping-Pong (B)"}),
                   ParamDesc::Bool("Clamp", false)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int op = paramI(2);
        const bool clampOut = paramB(3);
        const int pins[2] = {0, 1};
        const float defs[2] = {0.5f, 0.5f};
        out[0] = channelOp<2>(*this, in, pins, defs, [&](const float* v) {
            float a = v[0], b = v[1], r = 0.0f;
            switch (op) {
                case Add: r = a + b; break;
                case Subtract: r = a - b; break;
                case Multiply: r = a * b; break;
                case Divide: r = std::fabs(b) < 1e-9f ? 0.0f : a / b; break;
                case Power: r = (a < 0 && b != std::floor(b)) ? 0.0f : std::pow(a, b); break;
                case Logarithm: r = (a > 0 && b > 0 && b != 1) ? std::log(a) / std::log(b) : 0.0f; break;
                case SquareRoot: r = a > 0 ? std::sqrt(a) : 0.0f; break;
                case Absolute: r = std::fabs(a); break;
                case Minimum: r = std::min(a, b); break;
                case Maximum: r = std::max(a, b); break;
                case LessThan: r = a < b ? 1.0f : 0.0f; break;
                case GreaterThan: r = a > b ? 1.0f : 0.0f; break;
                case Modulo: r = std::fabs(b) < 1e-9f ? 0.0f : a - b * std::floor(a / b); break;
                case Floor: r = std::floor(a); break;
                case Ceil: r = std::ceil(a); break;
                case Round: r = std::round(a); break;
                case Fract: r = a - std::floor(a); break;
                case Sine: r = std::sin(a); break;
                case Cosine: r = std::cos(a); break;
                case Snap: r = std::fabs(b) < 1e-9f ? a : std::floor(a / b) * b; break;
                case PingPong: {
                    if (std::fabs(b) < 1e-9f) break;
                    float t = a / b - std::floor(a / b * 0.5f) * 2.0f;
                    r = (t > 1.0f ? 2.0f - t : t) * b;
                    break;
                }
            }
            return clampOut ? std::clamp(r, 0.0f, 1.0f) : r;
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpuChannelOpSupported(in); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        op.functions = "const int OP = " + std::to_string(paramI(2)) + ";\nconst bool CLAMP = " +
                       (paramB(3) ? "true" : "false") + ";\n";
        // C's pow (negative bases with integer exponents) and round (halves away from zero), which
        // GLSL leaves undefined.
        op.functions += R"(
float cPow(float a, float b) {
    if (a >= 0.0) return powPos(a, b);
    float r = powPos(-a, b);
    return mod(b, 2.0) == 1.0 ? -r : r;
}
)";
        op.body = R"(
    float a = par0(p), b = par1(p), r = 0.0;
    switch (OP) {
        case 0: r = a + b; break;
        case 1: r = a - b; break;
        case 2: r = a * b; break;
        case 3: r = abs(b) < 1e-9 ? 0.0 : a / b; break;
        case 4: r = (a < 0.0 && b != floor(b)) ? 0.0 : cPow(a, b); break;
        case 5: r = (a > 0.0 && b > 0.0 && b != 1.0) ? log(a) / log(b) : 0.0; break;
        case 6: r = a > 0.0 ? sqrt(a) : 0.0; break;
        case 7: r = abs(a); break;
        case 8: r = min(a, b); break;
        case 9: r = max(a, b); break;
        case 10: r = a < b ? 1.0 : 0.0; break;
        case 11: r = a > b ? 1.0 : 0.0; break;
        case 12: r = abs(b) < 1e-9 ? 0.0 : a - b * floor(a / b); break;
        case 13: r = floor(a); break;
        case 14: r = ceil(a); break;
        case 15: r = a < 0.0 ? -floor(-a + 0.5) : floor(a + 0.5); break;
        case 16: r = a - floor(a); break;
        case 17: r = sin(a); break;
        case 18: r = cos(a); break;
        case 19: r = abs(b) < 1e-9 ? a : floor(a / b) * b; break;
        case 20:
            if (abs(b) >= 1e-9) {
                float t = a / b - floor(a / b * 0.5) * 2.0;
                r = (t > 1.0 ? 2.0 - t : t) * b;
            }
            break;
    }
    out0 = CLAMP ? clamp(r, 0.0, 1.0) : r;)";
        gpuChannelOp(ctx, *this, op, {0.5f, 0.5f}, in, out);
    }
};

}  // namespace

void registerExpressionNodes(NodeRegistry& r);  // Expression.cpp

void registerConverterNodes(NodeRegistry& r) {
    r.add<ColorRampNode>();
    r.add<ColorKeyNode>();
    r.add<MapRangeNode>();
    r.add<MathNode>();
    r.add<ClampNode>();
    r.add<ThresholdNode>();
    registerExpressionNodes(r);
}
