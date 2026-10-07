#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ColorManagement.h"
#include "core/Value.h"

class ImageCache;

struct PinDesc {
    std::string name;
    PinType type;
    // For inputs: index of a param whose value is used when the pin is unconnected
    // (so a slider can be replaced by a wire, e.g. a channel driving "Amount" per pixel).
    int fallbackParam = -1;
};

enum class ParamKind { Float, Int, Bool, Enum, Path, Text, Curve, Ramp, Color, SavePath };

// What a Float slider's track shows, like Lightroom's coloured sliders: the colours the value
// gives along its range (ui/SliderTrack.cpp draws them).
enum class SliderTrack {
    None,
    Temperature,  // cool blue to warm yellow
    Tint,         // green to magenta
    Kelvin,       // the black-body colour at each temperature
    Hue,          // the hue in degrees (0..360)
    HueShift,     // trackHue moved by up to +-trackSpan degrees across the range
    Saturation,   // trackHue from grey to vivid
    Luminance,    // trackHue from dark to bright
};

struct ParamDesc {
    std::string name;
    ParamKind kind = ParamKind::Float;
    float min = 0.0f, max = 1.0f;          // slider (soft) range
    float hardMin = 0.0f, hardMax = 1.0f;  // values are clamped to this
    nlohmann::json def;
    std::vector<std::string> options;  // Enum labels
    // Shown only while param `showIf` equals `showIfValue` (a Bool counts as 0/1), like Blender
    // hiding Factor X/Y until Blur's Relative is on. Hidden params keep their values.
    int showIf = -1;
    int showIfValue = 1;
    bool gammaColor = false;  // Color kind: see ColorGamma()
    SliderTrack track = SliderTrack::None;
    float trackHue = 0.0f, trackSpan = 0.0f;  // degrees, for the band tracks

    ParamDesc when(int param, int value = 1) const {
        ParamDesc d = *this;
        d.showIf = param;
        d.showIfValue = value;
        return d;
    }
    ParamDesc withTrack(SliderTrack t, float hue = 0.0f, float span = 0.0f) const {
        ParamDesc d = *this;
        d.track = t;
        d.trackHue = hue;
        d.trackSpan = span;
        return d;
    }

    // Clamped to [mn, mx].
    static ParamDesc Float(std::string n, float def, float mn, float mx) { return make(std::move(n), ParamKind::Float, mn, mx, mn, mx, def); }
    // Slider shows [mn, mx] but any value is allowed (math inputs).
    static ParamDesc FloatFree(std::string n, float def, float mn, float mx) {
        return make(std::move(n), ParamKind::Float, mn, mx, -1e6f, 1e6f, def);
    }
    static ParamDesc Int(std::string n, int def, int mn, int mx) {
        return make(std::move(n), ParamKind::Int, float(mn), float(mx), float(mn), float(mx), def);
    }
    static ParamDesc Bool(std::string n, bool def) { return make(std::move(n), ParamKind::Bool, 0, 1, 0, 1, def); }
    static ParamDesc Enum(std::string n, int def, std::vector<std::string> opts) {
        float mx = float(opts.size()) - 1;
        ParamDesc d = make(std::move(n), ParamKind::Enum, 0, mx, 0, mx, def);
        d.options = std::move(opts);
        return d;
    }
    static ParamDesc Path(std::string n) { return make(std::move(n), ParamKind::Path, 0, 0, 0, 0, ""); }
    static ParamDesc Text(std::string n, std::string def) { return make(std::move(n), ParamKind::Text, 0, 0, 0, 0, std::move(def)); }
    // Tone curves: {"master":[[x,y],...], "r":[...], "g":[...], "b":[...]}.
    static ParamDesc Curve(std::string n);
    // Curves with custom channels. keys: "key:Label" entries (add "@hue" for a hue-strip
    // background); def: {"key": [[x,y],...], ...}.
    static ParamDesc CurveKeys(std::string n, std::vector<std::string> keys, nlohmann::json def) {
        ParamDesc d = make(std::move(n), ParamKind::Curve, 0, 0, 0, 0, std::move(def));
        d.options = std::move(keys);
        return d;
    }
    // RGB color stored as [r, g, b]; range is the allowed component range. In scene-linear
    // projects the value is linear and the picker shows it sRGB-encoded, as in Blender.
    static ParamDesc Color(std::string n, float r, float g, float b, float mx = 1.0f) {
        return make(std::move(n), ParamKind::Color, 0, mx, 0, mx, nlohmann::json::array({r, g, b}));
    }
    // A colour of multipliers (lift / gamma / gain), stored and shown as it is in every project,
    // like Blender's gamma-corrected colour properties.
    static ParamDesc ColorGamma(std::string n, float r, float g, float b, float mx = 1.0f) {
        ParamDesc d = Color(std::move(n), r, g, b, mx);
        d.gammaColor = true;
        return d;
    }
    // File path chosen with a Save dialog (File Output node).
    static ParamDesc SavePath(std::string n) { return make(std::move(n), ParamKind::SavePath, 0, 0, 0, 0, ""); }
    // Color ramp: {"interp": 0..3, "stops": [[pos, r, g, b, a], ...]}.
    static ParamDesc Ramp(std::string n);

private:
    static ParamDesc make(std::string n, ParamKind k, float mn, float mx, float hmn, float hmx, nlohmann::json def) {
        ParamDesc d;
        d.name = std::move(n);
        d.kind = k;
        d.min = mn;
        d.max = mx;
        d.hardMin = hmn;
        d.hardMax = hmx;
        d.def = std::move(def);
        return d;
    }
};

struct NodeInfo {
    std::string type;         // stable id used in save files, e.g. "color.saturation"
    std::string displayName;  // shown in the UI
    std::string category;     // add-menu grouping
    std::vector<PinDesc> inputs;
    std::vector<PinDesc> outputs;
    std::vector<ParamDesc> params;
    bool hidden = false;  // not offered in the add-node menu (group internals)
    // Params are edited only in the Inspector (like Blender's sidebar-only node properties), for
    // nodes with too many sliders to fit on the node body. Pin-backed params still show on pins.
    bool compact = false;
};

// A rectangle of pixels.
struct PixelRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool operator==(const PixelRect&) const = default;
};

// Region of interest, like darktable's: when the viewer is zoomed in, only the visible part of the
// image is evaluated, at the resolution the screen shows. A node then receives and returns
// buffers covering `rect` of its full image (canvasW x canvasH) instead of the whole of it.
struct RoiWindow {
    PixelRect rect;
    int canvasW = 0, canvasH = 0;
    // For nodes that map regions (Node::roiMap): the part of their input (inputW x inputH) the
    // input buffers hold.
    PixelRect input;
    int inputW = 0, inputH = 0;
};

struct EvalContext {
    int defaultW = 512, defaultH = 512;  // size used when a node has no sized inputs
    bool proxy = true;                   // preview-resolution sources
    int proxyEdge = 1280;                // long edge of the preview sources (ImageCache::kProxyEdge)
    // Preview pixels per full-resolution pixel. Sizes in params (blur radius, offsets) are in
    // full-resolution pixels and multiplied by this, so previews match the exported image.
    float scale = 1.0f;
    ImageCache* cache = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    // An interactive preview, cancelled and restarted on every edit: slow work that doesn't
    // depend on the edits (AI models) runs in the background, and the node shows what it has
    // meanwhile. Exports and renders wait for it.
    bool interactive = false;
    // The project's (root graph's) colour management. Nodes check linear() for the working space;
    // File Output uses the view transform to write display images.
    ColorManagement colorManagement;
    bool linear() const { return colorManagement.linear; }

    // Set while a node runs on part of its image (see RoiWindow and Node::roiPadding); null for
    // whole images. Nodes that compute positions read it through nodeutil::frameOf.
    const RoiWindow* roi = nullptr;
    // Global statistics (Normalize's percentiles), as darktable does for its preview pipe: a
    // preview run records them in statsOut, and a region run reuses them from previewStats, so a
    // zoomed-in region looks the same as the whole image and needn't be computed in full.
    std::vector<float>* statsOut = nullptr;
    const std::vector<float>* previewStats = nullptr;

    // GPU compositing (gpu/Device.h), like Blender's compositor Device and Precision: nodes with
    // a GPU version run there and their results stay on the GPU. gpuHalf stores images in half
    // floats (Precision: Auto), which halves the memory traffic per-pixel nodes are bound by.
    bool gpu = false;
    bool gpuHalf = false;
};

class Node {
public:
    virtual ~Node() = default;
    virtual const NodeInfo& info() const = 0;
    // inputs[i] is already filled with the fallback param value for unconnected pins that have one.
    virtual void evaluate(EvalContext& ctx, const std::vector<Value>& inputs, std::vector<Value>& outputs) = 0;

    // Extra per-node state beyond params (node groups store their inner graph here).
    virtual void saveExtra(nlohmann::json&) const {}
    virtual void loadExtra(const nlohmann::json&) {}
    // Folded into the evaluation cache key so changes to extra state trigger recomputation.
    virtual std::string signatureExtra() const { return {}; }

    // ---- Region of interest (see RoiWindow). Each node says what it reads to produce a region.
    static constexpr int kRoiWhole = -1;
    // How far around each output pixel the node reads its inputs, in working pixels: 0 for
    // per-pixel operations, a blur's reach, or kRoiWhole when it needs the whole image (geometry
    // changes, global statistics). Padded nodes don't need to know about regions: they run on the
    // padded window and the evaluator keeps the middle. The default covers the per-pixel families.
    // ctx.roi holds the requested region and the node's full size (for sizes relative to it), and
    // ctx.previewStats what the node recorded in the preview: nodes with global statistics run on
    // their window when they have them, and need the whole image otherwise.
    virtual int roiPadding(const EvalContext&) const {
        const std::string& c = info().category;
        return c == "Color" || c == "Mix" || c == "Converter" ? 0 : kRoiWhole;
    }
    // Nodes that move pixels without needing the whole image (Crop) map the region themselves:
    // `in` is the part of their input (inW x inH) needed for `out`. They then run with ctx.roi
    // set to their output region and must return buffers of exactly that size.
    virtual bool roiMap(const EvalContext&, int /*inW*/, int /*inH*/, const PixelRect& /*out*/, PixelRect& /*in*/) const {
        return false;
    }
    // Full output size for an input of inW x inH, for nodes that change it (Crop).
    virtual void roiOutputSize(int inW, int inH, int& w, int& h) const {
        w = inW;
        h = inH;
    }
    // Full size of a source's image (Image Input) at the region's scale; false for other nodes.
    virtual bool roiSourceSize(const EvalContext&, int& /*w*/, int& /*h*/) const { return false; }

    // ---- GPU (see gpu/Device.h and gpu/PointOp.h). A node with a GPU version says which inputs
    // it handles (what they are, not where they are). The evaluator then uploads its sized inputs
    // (numbers and constant channels stay as they are) and calls evaluateGpu, whose outputs may
    // stay on the GPU. Otherwise evaluate() runs on CPU copies of the inputs.
    virtual bool gpuSupported(const EvalContext&, const std::vector<Value>& /*in*/) const { return false; }
    virtual void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) { evaluate(ctx, in, out); }

    void initParams() {
        params.clear();
        for (const auto& p : info().params) params.push_back(p.def);
    }
    // Reset to Defaults (Blender's Reset to Default Values): every param back to its default,
    // except file paths, which say what the node works on rather than how.
    virtual void resetParams() {
        const auto& ps = info().params;
        for (size_t i = 0; i < ps.size() && i < params.size(); ++i)
            if (ps[i].kind != ParamKind::Path && ps[i].kind != ParamKind::SavePath) params[i] = ps[i].def;
    }

    // One param back to its default, as resetParams would set it (a RAW's own defaults too).
    void resetParam(int i) {
        if (i < 0 || i >= int(params.size())) return;
        nlohmann::json keep = params;
        resetParams();
        nlohmann::json v = std::move(params[size_t(i)]);
        params = std::move(keep);
        params[size_t(i)] = std::move(v);
    }

    float paramF(int i) const { return params[i].is_number() ? params[i].get<float>() : 0.0f; }
    int paramI(int i) const { return params[i].is_number() ? params[i].get<int>() : 0; }
    bool paramB(int i) const { return params[i].is_boolean() ? params[i].get<bool>() : false; }
    // False when the param's ParamDesc::showIf condition (or the node's paramHidden) hides it (UI only).
    bool paramVisible(int i) const {
        if (paramHidden(i)) return false;
        const ParamDesc& d = info().params[i];
        if (d.showIf < 0 || d.showIf >= int(params.size())) return true;
        const nlohmann::json& v = params[d.showIf];
        const int cur = v.is_boolean() ? int(v.get<bool>()) : (v.is_number() ? v.get<int>() : 0);
        return cur == d.showIfValue;
    }
    // Node-specific hiding for conditions showIf can't express (e.g. Image Input's RAW-only params).
    virtual bool paramHidden(int) const { return false; }
    std::string paramS(int i) const { return params[i].is_string() ? params[i].get<std::string>() : std::string(); }
    void paramC(int i, float out[3]) const {
        for (int k = 0; k < 3; ++k)
            out[k] = params[i].is_array() && params[i].size() == 3 && params[i][k].is_number() ? params[i][k].get<float>() : 0.0f;
    }

    int id = 0;
    float x = 0.0f, y = 0.0f;  // editor grid position
    std::vector<nlohmann::json> params;
    bool muted = false;      // M: bypass, inputs pass straight through to matching outputs
    bool collapsed = false;  // H: drawn as a compact title bar
    std::string label;       // F2: custom title (empty = type name)

    const std::string& title() const { return label.empty() ? info().displayName : label; }
};
