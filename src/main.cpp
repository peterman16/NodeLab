#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "core/Version.h"
#include "gpu/Device.h"
#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "io/Exif.h"
#include "io/Export.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "ml/Models.h"
#include "nodes/io/IONodes.h"
#include "nodes/utility/UtilityNodes.h"
#include "ui/App.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

// --device gpu for the command-line modes: GLFW started only for the GPU device (its context lives
// on a hidden window). Falls back to the CPU, saying why, when there is no usable GPU.
struct HeadlessDevice {
    bool gpu = false;
    bool half = false;
    explicit HeadlessDevice(int argc, char** argv, bool halfByDefault) {
        bool want = false;
        half = halfByDefault;
        for (int i = 1; i + 1 < argc; ++i) {
            const std::string a = argv[i], b = argv[i + 1];
            if (a == "--device") want = b == "gpu";
            if (a == "--precision") half = b != "full";
        }
        if (!want) return;
        if (!glfwInit()) {
            std::fprintf(stderr, "GPU unavailable (GLFW failed to start); using the CPU\n");
            return;
        }
        started_ = true;
        std::string why;
        gpu = gpu::init(&why);
        if (!gpu) std::fprintf(stderr, "GPU unavailable (%s); using the CPU\n", why.c_str());
    }
    ~HeadlessDevice() {
        if (!started_) return;
        gpu::shutdown();
        glfwTerminate();
    }
    void apply(EvalContext& ctx) const {
        ctx.gpu = gpu;
        ctx.gpuHalf = half;
    }
    std::string name() const { return gpu ? std::string("GPU (") + (half ? "half" : "full") + ")" : "CPU"; }

private:
    bool started_ = false;
};

// NodeLab.exe --render project.nlproj out.png [--depth N] [--device gpu [--precision half]] [--timings] :
// evaluate at full resolution without a window. The extension picks the format (.png, .jpg, .tif, .exr); --depth 16 for 16-bit PNG/TIFF,
// 32 for full-float EXR. --timings prints how long loading and evaluating, the view transform and
// encoding took.
static int renderHeadless(const std::string& project, const std::string& outPath, int depth, const HeadlessDevice& dev,
                          bool timings) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    Graph g;
    nlohmann::json ui;
    std::string err;
    if (!loadProject(project, g, ui, err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    int outId = g.firstOfType(OutputNode::staticInfo().type);
    if (!outId) {
        std::fprintf(stderr, "project has no Output node\n");
        return 1;
    }
    ImageCache cache;
    EvalContext ctx;
    ctx.proxy = false;
    ctx.cache = &cache;
    initContextSize(g, ctx);
    dev.apply(ctx);
    try {
        Evaluator ev;
        // As File > Export: each node's result is freed once the nodes reading it are done, which
        // lowers the peak by several full-resolution buffers.
        ev.releaseIntermediates = true;
        ImagePtr img = ev.evaluateDisplay(g, outId, ctx);
        if (ev.gpuFallbacks) std::fprintf(stderr, "%d nodes ran on the CPU instead: %s\n", ev.gpuFallbacks, ev.lastGpuError.c_str());
        if (!img) {
            std::fprintf(stderr, "Output node produced no image\n");
            return 1;
        }
        const auto t1 = Clock::now();
        SaveOptions opt;
        opt.format = formatFromPath(outPath);
        opt.depth = depth;
        if (opt.format == FileFormat::JPEG) opt.exif = exif::exportBlock(metadataSource(g), img->w, img->h);
        if (!saveRendered(outPath, img, ctx.colorManagement, opt, err)) {
            std::fprintf(stderr, "save failed: %s\n", err.c_str());
            return 1;
        }
        if (timings)
            std::fprintf(stderr, "%dx%d: load and evaluate %.0f ms, save %.0f ms\n", img->w, img->h, ms(t0, t1),
                         ms(t1, Clock::now()));
        for (const auto& line : writeFileOutputs(g, cache)) std::printf("%s\n", line.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "evaluation failed: %s\n", e.what());
        return 1;
    }
    return 0;
}

// NodeLab.exe --benchmark project.nlproj [--full] [--runs N] : evaluates the Output node from an
// empty cache N times (after one warm-up run that also loads the images) and prints per-node and
// total median milliseconds. Proxy resolution unless --full. --device gpu runs GPU nodes there
// (marked "gpu"), with --precision full or half (the default, like Precision: Auto). --sync waits
// for the device around each GPU node and runs fused chains unfused, so each node's time is its
// own (the total is then slower than a normal run).
static int benchmarkHeadless(const std::string& project, bool full, int runs, bool sync, const HeadlessDevice& dev) {
    Graph g;
    nlohmann::json ui;
    std::string err;
    if (!loadProject(project, g, ui, err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    int outId = g.firstOfType(OutputNode::staticInfo().type);
    if (!outId) {
        std::fprintf(stderr, "project has no Output node\n");
        return 1;
    }
    ImageCache cache;
    EvalContext ctx;
    ctx.proxy = !full;
    ctx.cache = &cache;
    initContextSize(g, ctx);
    dev.apply(ctx);
    std::unordered_map<int, std::vector<double>> per;
    std::unordered_map<int, bool> onGpu;
    int fallbacks = 0;
    std::string fallbackWhy;
    std::vector<double> totals;
    try {
        for (int r = 0; r <= runs; ++r) {
            Evaluator ev;
            ev.syncTimings = sync;
            const auto t0 = std::chrono::steady_clock::now();
            ev.evaluateDisplay(g, outId, ctx);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (r == 0) continue;  // warm-up: image decoding
            totals.push_back(ms);
            for (const auto& [id, t] : ev.timings()) per[id].push_back(t);
            onGpu = ev.gpuNodes();
            fallbacks = ev.gpuFallbacks;
            fallbackWhy = ev.lastGpuError;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "evaluation failed: %s\n", e.what());
        return 1;
    }
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    std::vector<std::pair<double, int>> rows;
    for (const auto& [id, v] : per) rows.push_back({median(v), id});
    std::sort(rows.rbegin(), rows.rend());
    std::printf("%s  %dx%d (scale %.4g)  %s  %s\n", project.c_str(), ctx.defaultW, ctx.defaultH, ctx.scale,
                full ? "full" : "proxy", dev.name().c_str());
    for (const auto& [ms, id] : rows) {
        const Node* n = g.find(id);
        std::string name = n->label.empty() ? n->info().displayName : n->label;
        std::printf("%9.2f ms  %-6d %-28s %-3s %s\n", ms, id, name.c_str(), onGpu[id] ? "gpu" : "", n->info().type.c_str());
    }
    if (fallbacks) std::printf("%d GPU nodes ran on the CPU instead: %s\n", fallbacks, fallbackWhy.c_str());
    std::printf("%9.2f ms  total (median of %d)\n", median(totals), runs);
    return 0;
}

// NodeLab.exe --batch project.nlproj outDir [--png|--jpg|--tif|--exr] [--depth N] in1 in2 ... : runs
// each source image through the project (fed into its first Image Input) and writes
// outDir/<name>.<ext>, or as the project's filename template names it. Unset options come
// from the project's Export settings.
static int batchHeadless(const std::string& project, const std::string& outDir, std::vector<std::string> args) {
    Graph g;
    nlohmann::json ui;
    std::string err;
    if (!loadProject(project, g, ui, err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    ExportSettings s;
    if (auto e = ui.find("export"); e != ui.end()) s.fromJson(*e);
    std::vector<ExportItem> items;
    std::vector<std::string> sources;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--jpg") s.format = ExportSettings::JPEG;
        else if (a == "--png") s.format = ExportSettings::PNG;
        else if (a == "--tif") s.format = ExportSettings::TIFF;
        else if (a == "--exr") s.format = ExportSettings::EXR;
        else if (a == "--depth" && i + 1 < args.size()) s.depth = std::atoi(args[++i].c_str());
        else if (a.rfind("--", 0) != 0) sources.push_back(a);
    }
    std::vector<NameSource> names;
    for (const std::string& a : sources) names.push_back({a});
    const std::vector<std::string> outputs = batchOutputPaths(names, outDir, s);
    for (size_t i = 0; i < sources.size(); ++i) items.push_back({sources[i], outputs[i]});
    const int input = g.firstOfType(ImageInputNode::staticInfo().type);
    if (!input || items.empty()) {
        std::fprintf(stderr, input ? "no source images given\n" : "project has no Image Input node\n");
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(u8ToPath(outDir), ec);
    Exporter ex;
    ex.start(g.toJson(), std::move(items), input, s);
    ex.wait();
    for (const std::string& line : ex.takeLog()) std::printf("%s\n", line.c_str());
    return ex.progress().failed ? 1 : 0;
}

// The Release exe is a GUI app (-mwindows) with no console; when started from a terminal for a
// command-line mode, reattach to that terminal so printf output shows up.
// NodeLab.exe --list-nodes : every registered node with its pins and params (for keeping GUIDE.md
// in sync). Hidden internal nodes are skipped.
static void listNodes() {
    const NodeRegistry& reg = NodeRegistry::instance();
    for (const std::string& type : reg.types()) {
        const NodeInfo* inf = reg.find(type);
        if (!inf || inf->hidden) continue;
        std::printf("%s | %s | %s\n", inf->category.c_str(), inf->displayName.c_str(), type.c_str());
        for (const PinDesc& p : inf->inputs) std::printf("  in  %s (%s)\n", p.name.c_str(), pinTypeName(p.type));
        for (const PinDesc& p : inf->outputs) std::printf("  out %s (%s)\n", p.name.c_str(), pinTypeName(p.type));
        for (const ParamDesc& p : inf->params) {
            std::string opts;
            for (const std::string& o : p.options) opts += (opts.empty() ? "" : ", ") + o;
            std::printf("  param %s = %s [%g..%g] %s\n", p.name.c_str(), p.def.dump().c_str(), p.min, p.max, opts.c_str());
        }
    }
}

static void attachParentConsole() {
#ifdef _WIN32
    // Output already redirected to a pipe or file: leave it there.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // argv is in the ANSI code page, which can't hold every file name (a photo named in another
    // script, opened from Explorer, arrived mangled). Rebuild it as UTF-8, which every path
    // in NodeLab is, from the wide command line.
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Argv;
    int wideCount = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &wideCount)) {
        for (int i = 0; i < wideCount; ++i) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(size_t(std::max(len - 1, 0)), '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), len, nullptr, nullptr);
            utf8Args.push_back(std::move(s));
        }
        LocalFree(wide);
        for (std::string& s : utf8Args) utf8Argv.push_back(s.data());
        utf8Argv.push_back(nullptr);
        argc = wideCount, argv = utf8Argv.data();
    }
#endif
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        attachParentConsole();
        std::printf("NodeLab %s\n", versionString().c_str());
        return 0;
    }
    registerAllNodes();
    if (argc >= 2 && std::string(argv[1]) == "--list-nodes") {
        attachParentConsole();
        listNodes();
        return 0;
    }
    // NodeLab.exe --install-model subject|subject-light|sky : downloads an AI model (and the runtime) as the
    // Inspector's Download button does.
    if (argc >= 3 && std::string(argv[1]) == "--install-model") {
        attachParentConsole();
        const ml::ModelSpec* m = ml::findModel(argv[2]);
        if (!m || std::string(argv[2]) == ml::kRuntime) {
            std::fprintf(stderr, "Unknown model '%s' (subject or sky)\n", argv[2]);
            return 1;
        }
        std::printf("Installing %s: %.0f MB to %s\n", m->title, double(ml::downloadSize(argv[2])) / 1e6,
                    ml::folder().c_str());
        const std::string err = ml::installNow(argv[2], [](uint64_t done, uint64_t total) {
            std::printf("\r%5.1f%%", total ? 100.0 * double(done) / double(total) : 100.0);
            std::fflush(stdout);
        });
        std::printf("\n%s\n", err.empty() ? "Done" : err.c_str());
        return err.empty() ? 0 : 1;
    }
    if (argc >= 4 && std::string(argv[1]) == "--render") {
        attachParentConsole();
        int depth = 8;
        for (int i = 4; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--depth") depth = std::atoi(argv[i + 1]);
        bool timings = false;
        for (int i = 4; i < argc; ++i) timings |= std::string(argv[i]) == "--timings";
        HeadlessDevice dev(argc, argv, false);
        return renderHeadless(argv[2], argv[3], depth, dev, timings);
    }

    if (argc >= 3 && std::string(argv[1]) == "--benchmark") {
        attachParentConsole();
        bool full = false, sync = false;
        int runs = 5;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--full") full = true;
            else if (a == "--sync") sync = true;
            else if (a == "--runs" && i + 1 < argc) runs = std::max(1, std::atoi(argv[++i]));
        }
        HeadlessDevice dev(argc, argv, true);
        return benchmarkHeadless(argv[2], full, runs, sync, dev);
    }

    // NodeLab.exe --gpu-info : the GPU device NodeLab would use, or why there is none.
    if (argc >= 2 && std::string(argv[1]) == "--gpu-info") {
        attachParentConsole();
        char gpuArg[] = "gpu", devArg[] = "--device";
        char* args[] = {argv[0], devArg, gpuArg};
        HeadlessDevice dev(3, args, true);
        std::printf("%s\n", gpu::description().c_str());
        return dev.gpu ? 0 : 1;
    }

    if (argc >= 5 && std::string(argv[1]) == "--batch") {
        attachParentConsole();
        return batchHeadless(argv[2], argv[3], std::vector<std::string>(argv + 4, argv + argc));
    }

    App::RunOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) opt.screenshot = argv[++i];
        else if (a == "--script" && i + 1 < argc) opt.script = argv[++i];
        else if (a == "--device" && i + 1 < argc) opt.gpu = std::string(argv[++i]) == "gpu";
        else opt.project = a;
    }
    App app;
    return app.run(opt);
}
