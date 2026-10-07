#include "io/Export.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <numbers>
#include <optional>
#include <set>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "gpu/Device.h"
#include "graph/Evaluator.h"
#include "graph/Graph.h"
#include "io/Exif.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Library.h"
#include "nodes/ImageOps.h"
#include "io/Paths.h"
#include "nodes/io/IONodes.h"
#include "nodes/utility/UtilityNodes.h"

namespace fs = std::filesystem;

SaveOptions ExportSettings::saveOptions(const std::string& source, int w, int h) const {
    SaveOptions o;
    o.format = FileFormat(format);
    o.depth = depth;
    o.jpegQuality = jpegQuality;
    if (format == JPEG) o.exif = exif::exportBlock(source, w, h);
    return o;
}

nlohmann::json ExportSettings::toJson() const {
    return {{"format", format},     {"depth", depth},           {"jpegQuality", jpegQuality},
            {"sizeMode", sizeMode}, {"longEdge", longEdge},     {"percent", percent},
            {"fileOutputs", fileOutputs}, {"nameTemplate", nameTemplate},   {"sharpenFor", sharpenFor},
            {"sharpenAmount", sharpenAmount}};
}

void ExportSettings::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    format = std::clamp(j.value("format", format), 0, 3);
    depth = std::clamp(j.value("depth", depth), 8, 32);
    jpegQuality = std::clamp(j.value("jpegQuality", jpegQuality), 1, 100);
    sizeMode = std::clamp(j.value("sizeMode", sizeMode), 0, 2);
    longEdge = std::clamp(j.value("longEdge", longEdge), 16, 65536);
    percent = std::clamp(j.value("percent", percent), 1, 100);
    fileOutputs = j.value("fileOutputs", fileOutputs);
    sharpenFor = std::clamp(j.value("sharpenFor", sharpenFor), 0, 3);
    sharpenAmount = std::clamp(j.value("sharpenAmount", sharpenAmount), 0, 2);
    // Before templates there was only a suffix after the source's name.
    if (auto t = j.find("nameTemplate"); t != j.end() && t->is_string() && !t->get<std::string>().empty())
        nameTemplate = t->get<std::string>();
    else if (auto sfx = j.find("suffix"); sfx != j.end() && sfx->is_string())
        nameTemplate = "{name}" + sfx->get<std::string>();
    // Projects and presets saved the template whatever it was, so the old default "{name}_edit"
    // is in many that never chose it: they get the source's own name too (an export still never
    // replaces its source).
    if (nameTemplate == "{name}_edit") nameTemplate = "{name}";
}

bool ExportSettings::sameOutput(const ExportSettings& o) const {
    nlohmann::json a = toJson(), b = o.toJson();
    a.erase("fileOutputs");
    b.erase("fileOutputs");
    // The depth means nothing for JPEG.
    if (format == JPEG) a.erase("depth"), b.erase("depth");
    return a == b;
}

const std::vector<ExportPreset>& builtInExportPresets() {
    static const std::vector<ExportPreset> presets = [] {
        std::vector<ExportPreset> v;
        auto add = [&](const char* name, auto&& set) {
            ExportPreset p{name, {}};
            set(p.settings);
            v.push_back(std::move(p));
        };
        add("Full-Size JPEG", [](ExportSettings& s) { s.format = ExportSettings::JPEG; });
        add("Web JPEG (2048 px)", [](ExportSettings& s) {
            s.format = ExportSettings::JPEG, s.jpegQuality = 85;
            s.sizeMode = ExportSettings::LongEdge, s.longEdge = 2048;
            s.sharpenFor = ExportSettings::Screen;
        });
        add("Email (1000 px)", [](ExportSettings& s) {
            s.format = ExportSettings::JPEG, s.jpegQuality = 75;
            s.sizeMode = ExportSettings::LongEdge, s.longEdge = 1000;
            s.sharpenFor = ExportSettings::Screen;
        });
        add("Full-Size PNG", [](ExportSettings& s) { s.format = ExportSettings::PNG; });
        add("16-bit TIFF", [](ExportSettings& s) { s.format = ExportSettings::TIFF, s.depth = 16; });
        add("OpenEXR (Half Float)", [](ExportSettings& s) { s.format = ExportSettings::EXR, s.depth = 16; });
        return v;
    }();
    return presets;
}

namespace {

float lanczos3(double x) {
    x = std::abs(x);
    if (x < 1e-8) return 1.0f;
    if (x >= 3.0) return 0.0f;
    const double px = std::numbers::pi * x;
    return float(3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px));
}

// Per output sample: the first source sample and `stride` weights (zero-padded), normalised.
struct Taps {
    std::vector<int> first;
    std::vector<float> w;
    int stride = 0;
};

Taps lanczosTaps(int n, int m) {
    const double scale = double(m) / n;
    const double fs = std::min(scale, 1.0);  // downscaling widens the kernel by 1/scale
    const double support = 3.0 / fs;
    Taps t;
    t.stride = int(std::ceil(support * 2)) + 2;
    t.first.resize(m);
    t.w.assign(size_t(m) * t.stride, 0.0f);
    for (int i = 0; i < m; ++i) {
        const double center = (i + 0.5) / scale;  // in source pixels
        const int lo = std::max(0, int(std::floor(center - support)));
        const int hi = std::min(n - 1, int(std::ceil(center + support)));
        t.first[i] = lo;
        float* w = &t.w[size_t(i) * t.stride];
        double sum = 0;
        for (int j = lo; j <= hi && j - lo < t.stride; ++j) sum += w[j - lo] = lanczos3((j + 0.5 - center) * fs);
        // Taps cut off at the image edge are dropped, so renormalise.
        if (sum != 0)
            for (int k = 0; k < t.stride; ++k) w[k] = float(w[k] / sum);
    }
    return t;
}

}  // namespace

std::shared_ptr<Image> resizeLanczos(const Image& src, int w, int h, bool srgbEncoded) {
    w = std::max(1, w);
    h = std::max(1, h);
    // Linear light, colour premultiplied by alpha.
    Image in(src.w, src.h);
    parallelFor(src.h, [&](int y) {
        for (int x = 0; x < src.w; ++x) {
            const size_t i = size_t(y) * src.w + x;
            const float* s = src.pixel(i);
            float* d = in.pixel(i);
            const float a = std::clamp(s[3], 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) d[c] = (srgbEncoded ? colormath::srgbToLinear(s[c]) : s[c]) * a;
            d[3] = a;
        }
    });
    const Taps tx = lanczosTaps(src.w, w), ty = lanczosTaps(src.h, h);

    // Horizontal pass into src.h rows of w pixels.
    Image mid(w, src.h);
    parallelFor(src.h, [&](int y) {
        const float* row = in.pixel(size_t(y) * src.w);
        for (int x = 0; x < w; ++x) {
            const float* wt = &tx.w[size_t(x) * tx.stride];
            float acc[4] = {0, 0, 0, 0}, lo[4], hi[4];
            for (int c = 0; c < 4; ++c) lo[c] = INFINITY, hi[c] = -INFINITY;
            for (int k = 0; k < tx.stride && tx.first[x] + k < src.w; ++k) {
                if (wt[k] == 0.0f) continue;
                const float* p = row + size_t(tx.first[x] + k) * 4;
                for (int c = 0; c < 4; ++c) {
                    acc[c] += wt[k] * p[c];
                    lo[c] = std::min(lo[c], p[c]);
                    hi[c] = std::max(hi[c], p[c]);
                }
            }
            float* d = mid.pixel(size_t(y) * w + x);
            for (int c = 0; c < 4; ++c) d[c] = std::clamp(acc[c], lo[c], hi[c]);
        }
    });

    // Vertical pass, a whole output row at a time so the reads run along rows.
    auto out = std::make_shared<Image>(w, h);
    parallelFor(h, [&](int y) {
        const float* wt = &ty.w[size_t(y) * ty.stride];
        std::vector<float> acc(size_t(w) * 4, 0.0f), lo(size_t(w) * 4, INFINITY), hi(size_t(w) * 4, -INFINITY);
        for (int k = 0; k < ty.stride && ty.first[y] + k < src.h; ++k) {
            if (wt[k] == 0.0f) continue;
            const float* row = mid.pixel(size_t(ty.first[y] + k) * w);
            for (size_t i = 0; i < size_t(w) * 4; ++i) {
                acc[i] += wt[k] * row[i];
                lo[i] = std::min(lo[i], row[i]);
                hi[i] = std::max(hi[i], row[i]);
            }
        }
        for (int x = 0; x < w; ++x) {
            float* d = out->pixel(size_t(y) * w + x);
            const size_t i = size_t(x) * 4;
            const float a = std::clamp(acc[i + 3], lo[i + 3], hi[i + 3]);
            d[3] = a;
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(acc[i + c], lo[i + c], hi[i + c]);
                const float un = a > 0.0f ? v / a : 0.0f;
                d[c] = srgbEncoded ? colormath::linearToSrgb(un) : un;
            }
        }
    });
    return out;
}

std::shared_ptr<const Image> resizeForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s,
                                             bool srgbEncoded) {
    if (!img || s.sizeMode == ExportSettings::Original) return img;
    const int edge = std::max(img->w, img->h);
    const int target = std::max(1, s.sizeMode == ExportSettings::LongEdge
                                       ? s.longEdge
                                       : int(std::lround(edge * std::clamp(s.percent, 1, 100) / 100.0)));
    if (edge <= target) return img;  // never enlarge
    const double scale = double(target) / edge;
    return resizeLanczos(*img, std::max(1, int(std::lround(img->w * scale))), std::max(1, int(std::lround(img->h * scale))),
                         srgbEncoded);
}

std::shared_ptr<const Image> sharpenForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s,
                                              bool linear) {
    if (!img || img->empty() || s.sharpenFor <= ExportSettings::SharpenOff || s.sharpenFor > ExportSettings::Glossy)
        return img;
    // Screen wants a fine radius (pixels are seen one to one); prints spread ink, so paper wants
    // a wider one, and matte paper, which softens more than glossy, the strongest.
    static const float kRadius[4] = {0.0f, 0.6f, 1.0f, 0.8f};
    static const float kAmount[4][3] = {{0, 0, 0}, {25, 45, 75}, {40, 65, 100}, {30, 55, 85}};
    imageops::SharpenSettings st;
    st.radius = kRadius[s.sharpenFor];
    st.amount = kAmount[s.sharpenFor][std::clamp(s.sharpenAmount, 0, 2)];
    st.detail = 50.0f;
    auto out = std::make_shared<Image>(*img);
    imageops::sharpenImage(*out, st, linear);
    if (!linear)
        for (size_t i = 0; i < out->px.size(); i += 4)
            for (int c = 0; c < 3; ++c) out->px[i + c] = std::min(out->px[i + c], 1.0f);
    return out;
}

bool saveRendered(const std::string& pathU8, const std::shared_ptr<const Image>& scene, const ColorManagement& cm,
                  const SaveOptions& opt, std::string& err) {
    if (!scene) {
        err = "nothing to save";
        return false;
    }
    if (opt.format != FileFormat::EXR) return writeImage(pathU8, *colormgmt::displayImage(scene, cm), opt, err);
    if (cm.linear) return writeImage(pathU8, *scene, opt, err);
    // Legacy projects hold sRGB-encoded values; EXR stores linear light.
    Image lin(scene->w, scene->h);
    parallelFor(scene->h, [&](int y) {
        for (int x = 0; x < scene->w; ++x) {
            const size_t i = size_t(y) * scene->w + x;
            const float* s = scene->pixel(i);
            float* d = lin.pixel(i);
            for (int c = 0; c < 3; ++c) d[c] = colormath::srgbToLinear(s[c]);
            d[3] = s[3];
        }
    });
    return writeImage(pathU8, lin, opt, err);
}

std::string metadataSource(const Graph& g) {
    for (const auto& [id, n] : g.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type && !n->paramS(0).empty()) return n->paramS(0);
    return {};
}

const NameToken kNameTokens[] = {
    {"{name}", "File name, without extension"},
    {"{seq}", "Sequence number (1, 2, 3...)"},
    {"{seq:3}", "Sequence number, 3 digits (001)"},
    {"{date}", "Capture date (YYYY-MM-DD)"},
    {"{time}", "Capture time (HHMMSS)"},
    {"{year}", "Capture year"},
    {"{month}", "Capture month (01-12)"},
    {"{day}", "Capture day (01-31)"},
    {"{camera}", "Camera model"},
    {"{make}", "Camera make"},
    {"{lens}", "Lens"},
    {"{iso}", "ISO"},
    {"{focal}", "Focal length (50mm)"},
    {"{aperture}", "Aperture (f2.8)"},
    {"{shutter}", "Shutter speed (1-250s)"},
    {"{copy}", "Virtual copy name (Copy 1)"},
    {"{folder}", "Folder name"},
    {"{today}", "Export date (YYYY-MM-DD)"},
};
const int kNameTokenCount = int(std::size(kNameTokens));

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// "%.*g": no trailing ".0".
std::string number(double v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*g", digits, v);
    return buf;
}

// Windows forbids <>:"/\|?* and control characters in names, and trailing dots or spaces.
std::string sanitizeName(std::string s) {
    for (char& c : s)
        if ((unsigned char)c < 32 || std::strchr("<>:\"/\\|?*", c)) c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// A time as EXIF writes it, "YYYY:MM:DD HH:MM:SS".
std::string exifTime(std::time_t t) {
    std::tm tm{};
    char buf[20];
    if (localtime_s(&tm, &t) != 0 || !std::strftime(buf, sizeof buf, "%Y:%m:%d %H:%M:%S", &tm)) return {};
    return buf;
}

}  // namespace

std::string expandNameTemplate(const std::string& tmpl, const NameSource& src) {
    const fs::path path = u8ToPath(src.path);
    // EXIF is read only when the template asks for it (a RAW that isn't TIFF-based is read whole).
    std::optional<exif::PhotoInfo> info;
    auto meta = [&]() -> const exif::PhotoInfo& {
        if (!info) {
            info.emplace();
            if (!src.path.empty()) exif::readInfo(src.path, *info);
        }
        return *info;
    };
    // The capture time, else the file's modification time (as Lightroom falls back to it).
    std::optional<std::string> when;
    auto captured = [&]() -> const std::string& {
        if (!when) {
            when = meta().captureTime;
            if (when->size() < 19 && !src.path.empty()) {
                std::error_code ec;
                const auto ft = fs::last_write_time(path, ec);
                if (!ec)
                    when = exifTime(std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(ft)));
            }
            if (when->size() < 19) when = std::string();
        }
        return *when;
    };
    auto part = [&](size_t at, size_t len) { return captured().size() >= at + len ? captured().substr(at, len) : std::string(); };

    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        const size_t close = tmpl[i] == '{' ? tmpl.find('}', i) : std::string::npos;
        if (close == std::string::npos) {
            out += tmpl[i++];
            continue;
        }
        const std::string raw = tmpl.substr(i, close - i + 1);
        std::string key = lowerAscii(raw.substr(1, raw.size() - 2));
        int pad = 0;
        if (const size_t colon = key.find(':'); colon != std::string::npos) {
            pad = std::clamp(std::atoi(key.c_str() + colon + 1), 0, 9);
            key.resize(colon);
        }
        std::string v;
        bool known = true;
        if (key == "name") v = src.path.empty() ? "export" : pathToU8(path.stem());
        else if (key == "folder") v = pathToU8(path.parent_path().filename());
        else if (key == "seq") {
            v = std::to_string(src.sequence);
            if (int(v.size()) < pad) v.insert(0, size_t(pad) - v.size(), '0');
        } else if (key == "copy") v = src.copy > 0 ? "Copy " + std::to_string(src.copy) : "";
        else if (key == "date") v = captured().empty() ? "" : part(0, 4) + "-" + part(5, 2) + "-" + part(8, 2);
        else if (key == "year") v = part(0, 4);
        else if (key == "month") v = part(5, 2);
        else if (key == "day") v = part(8, 2);
        else if (key == "hour") v = part(11, 2);
        else if (key == "minute") v = part(14, 2);
        else if (key == "second") v = part(17, 2);
        else if (key == "time") v = part(11, 2) + part(14, 2) + part(17, 2);
        else if (key == "today") {
            const std::string t = exifTime(std::time(nullptr));
            v = t.size() >= 10 ? t.substr(0, 4) + "-" + t.substr(5, 2) + "-" + t.substr(8, 2) : "";
        } else if (key == "camera") v = meta().model;
        else if (key == "make") v = meta().make;
        else if (key == "lens") v = meta().lens;
        else if (key == "iso") v = meta().iso > 0 ? number(meta().iso, 6) : "";
        else if (key == "focal") v = meta().focalLength > 0 ? number(meta().focalLength, 4) + "mm" : "";
        else if (key == "aperture") v = meta().fNumber > 0 ? "f" + number(meta().fNumber, 2) : "";
        else if (key == "shutter") {
            const float t = meta().exposureTime;
            v = t <= 0 ? "" : t < 0.4f ? "1-" + number(std::round(1.0 / t), 6) + "s" : number(t, 3) + "s";
        } else known = false;
        out += known ? v : raw;
        i = close + 1;
    }
    out = sanitizeName(out);
    return out.empty() ? sanitizeName(src.path.empty() ? "export" : pathToU8(path.stem())) : out;
}

std::string batchOutputPath(const std::string& sourceU8, const std::string& outDirU8, const ExportSettings& s,
                            int sequence, int copy) {
    const auto src = u8ToPath(sourceU8);
    const std::string name = expandNameTemplate(s.nameTemplate, {sourceU8, sequence, copy});
    auto out = u8ToPath(outDirU8) / u8ToPath(name + s.extension());
    // A template naming the file as it is, into the source folder, would overwrite the original.
    std::error_code ec;
    if (std::filesystem::equivalent(out, src, ec) || out.lexically_normal() == src.lexically_normal())
        out = u8ToPath(outDirU8) / u8ToPath(name + "_edit" + s.extension());
    return pathToU8(out);
}

std::vector<std::string> batchOutputPaths(const std::vector<NameSource>& sources, const std::string& outDirU8,
                                          const ExportSettings& s) {
    std::vector<std::string> out;
    // Lower case: Windows names ignore case.
    auto key = [](const fs::path& p) { return lowerAscii(pathToU8(p.lexically_normal())); };
    std::set<std::string> used, sourceFiles;
    for (const NameSource& src : sources)
        if (!src.path.empty()) sourceFiles.insert(key(u8ToPath(src.path)));
    // Files already in the output folder are previous exports, which a new export replaces,
    // unless the folder holds the sources: there they are originals (the camera JPEG of a
    // RAW+JPEG pair), as is a Library photo (one with a sidecar) wherever it is.
    std::error_code ec;
    const fs::path outDir = u8ToPath(outDirU8);
    bool holdsSources = false;
    for (const NameSource& src : sources)
        if (!src.path.empty() && !holdsSources) {
            const fs::path dir = u8ToPath(src.path).parent_path();
            holdsSources = key(dir) == key(outDir) || fs::equivalent(dir, outDir, ec);
        }
    auto taken = [&](const fs::path& p) {
        if (used.count(key(p)) || sourceFiles.count(key(p))) return true;
        if (!fs::exists(p, ec)) return false;
        return holdsSources || fs::exists(u8ToPath(library::sidecarPath(pathToU8(p))), ec);
    };
    for (size_t i = 0; i < sources.size(); ++i) {
        fs::path p = u8ToPath(batchOutputPath(sources[i].path, outDirU8, s, int(i) + 1, sources[i].copy));
        if (taken(p)) {
            const fs::path base = p;
            for (int k = 2;; ++k) {
                p = base.parent_path() / u8ToPath(pathToU8(base.stem()) + " (" + std::to_string(k) + ")" + pathToU8(base.extension()));
                if (!taken(p)) break;
            }
        }
        used.insert(key(p));
        out.push_back(pathToU8(p));
    }
    return out;
}

Exporter::~Exporter() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void Exporter::start(const nlohmann::json& graph, std::vector<ExportItem> items, int inputNode, const ExportSettings& s,
                     bool gpu) {
    if (thread_.joinable()) thread_.join();  // a finished job's thread
    cancel_ = false;
    busy_ = true;
    {
        std::lock_guard lock(mutex_);
        progress_ = {};
        progress_.total = int(items.size());
    }
    thread_ = std::thread([this, graph, items = std::move(items), inputNode, s, gpu]() mutable {
        run(std::move(graph), std::move(items), inputNode, std::move(s), gpu);
    });
}

void Exporter::wait() {
    if (thread_.joinable()) thread_.join();
}

Exporter::Progress Exporter::progress() const {
    std::lock_guard lock(mutex_);
    return progress_;
}

std::vector<std::string> Exporter::takeLog() {
    std::lock_guard lock(mutex_);
    return std::exchange(log_, {});
}

void Exporter::setStage(const std::string& s) {
    std::lock_guard lock(mutex_);
    progress_.stage = s;
}

void Exporter::log(const std::string& line) {
    std::lock_guard lock(mutex_);
    log_.push_back(line);
}

void Exporter::run(nlohmann::json graphJson, std::vector<ExportItem> items, int inputNode, ExportSettings s, bool gpu) {
    Graph jobGraph;
    try {
        if (!graphJson.is_null()) jobGraph.fromJson(graphJson);
    } catch (const std::exception& e) {
        log(std::string("Export failed: ") + e.what());
        busy_ = false;
        return;
    }
    const bool batch = inputNode != 0;
    int failed = 0;
    for (size_t i = 0; i < items.size() && !cancel_; ++i) {
        const ExportItem& item = items[i];
        // An item with its own edit (the library) renders that instead of the job's graph.
        Graph own;
        const bool ownGraph = !item.graph.is_null();
        if (ownGraph) {
            try {
                own.fromJson(item.graph);
            } catch (const std::exception& e) {
                ++failed;
                log("Failed " + item.source + ": " + e.what());
                continue;
            }
        }
        Graph& g = ownGraph ? own : jobGraph;
        const int outId = g.firstOfType(OutputNode::staticInfo().type);
        const std::string outName = pathToU8(u8ToPath(item.output).filename());
        // A fresh cache per item: batches would otherwise keep every source's preview in memory.
        ImageCache cache;
        EvalContext ctx;
        ctx.proxy = false;
        ctx.cache = &cache;
        ctx.cancel = &cancel_;
        ctx.colorManagement = g.colorManagement;
        ctx.gpu = gpu && gpu::available();
        ctx.gpuHalf = false;
        try {
            if (batch && !ownGraph) {
                Node* in = g.find(inputNode);
                if (!in) throw std::runtime_error("the batch Image Input node is gone");
                // As if chosen in the UI: a RAW batch from a JPEG project gets the RAW defaults.
                static_cast<ImageInputNode&>(*in).chooseFile(item.source);
                setStage("Loading " + pathToU8(u8ToPath(item.source).filename()));
                std::string err;
                ImagePtr src = cache.get(item.source, false, &err,
                                         static_cast<const ImageInputNode&>(*in).decode(ctx.linear()));
                if (!src) throw std::runtime_error(err.empty() ? "could not load " + item.source : err);
                // Size from this source, not whichever Image Input happens to come first.
                ctx.defaultW = src->w;
                ctx.defaultH = src->h;
                ctx.scale = 1.0f;
            } else {
                initContextSize(g, ctx);
            }
            Evaluator ev;
            // File Output nodes have fixed paths, so a batch would overwrite them on every item.
            const bool fileOutputs = !batch && !ownGraph && (s.fileOutputs || item.output.empty());
            // A full-resolution render needs only the outputs still to be read: dropping the rest
            // keeps a big photo's peak memory to a few images instead of one per node. File
            // Outputs read the graph again afterwards, so they keep everything.
            ev.releaseIntermediates = !fileOutputs;
            if (!item.output.empty()) {
                setStage("Rendering " + outName);
                ImagePtr img;
                if (outId) {
                    // The device is held while evaluating only (the previews wait meanwhile), not
                    // while resizing and saving.
                    std::optional<gpu::Scope> device;
                    if (ctx.gpu) device.emplace();
                    img = ev.evaluateDisplay(g, outId, ctx);
                }
                if (!img) throw std::runtime_error("the Output node has no input");
                // Before the view transform: resampling in scene light.
                img = resizeForExport(img, s, !ctx.linear());
                img = sharpenForExport(img, s, ctx.linear());
                if (cancel_) break;
                setStage("Saving " + outName);
                std::string err;
                const SaveOptions opt = s.saveOptions(batch || ownGraph ? item.source : metadataSource(g), img->w, img->h);
                if (!saveRendered(item.output, img, ctx.colorManagement, opt, err)) throw std::runtime_error(err);
                log("Wrote " + item.output + " (" + std::to_string(img->w) + " x " + std::to_string(img->h) + ")");
            }
            if (fileOutputs) {
                setStage("Writing File Outputs");
                for (const std::string& line : writeFileOutputs(g, ev, ctx)) log(line);
            }
            if (ev.gpuFallbacks)
                log(std::to_string(ev.gpuFallbacks) + " nodes ran on the CPU instead: " + ev.lastGpuError);
        } catch (const EvalCancelled&) {
            break;
        } catch (const std::exception& e) {
            ++failed;
            log("Failed " + (batch ? item.source : outName) + ": " + e.what());
        }
        if (ctx.gpu) {
            // A full-resolution image's textures are hundreds of MB: keep only a preview's worth.
            gpu::Scope device;
            gpu::trimPool(size_t(128) << 20);
        }
        std::lock_guard lock(mutex_);
        progress_.done = int(i + 1);
        progress_.failed = failed;
    }
    {
        std::lock_guard lock(mutex_);
        progress_.cancelled = cancel_;
        progress_.stage = cancel_ ? "Cancelled" : "Done";
    }
    if (cancel_) log("Cancelled");
    busy_ = false;
}
