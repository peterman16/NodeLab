#include "ui/App.h"
#include "ml/Models.h"
#include "ml/Onnx.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>

#include "core/ColorManagement.h"
#include "core/Guide.h"
#include "core/Version.h"
#include "gpu/Device.h"
#include "graph/Recipes.h"
#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Library.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "nodes/group/GroupNodes.h"
#include "nodes/io/IONodes.h"
#include "core/Parallel.h"
#include "nodes/matte/AutoMask.h"
#include "nodes/matte/MatteNodes.h"
#include "nodes/transform/TransformNodes.h"
#include "nodes/utility/UtilityNodes.h"
#include "ui/ColorDisplay.h"
#include "ui/Eyedropper.h"
#include "ui/FileDialog.h"
#include "ui/GuideWindow.h"
#include "nodes/color/AutoTone.h"
#include "nodes/filter/SpotRemoval.h"
#include "ui/Inspector.h"
#include "ui/NodeInspectors.h"
#include "ui/SystemStats.h"
#include "ui/UiScript.h"

namespace fs = std::filesystem;

static const char* kProjectFilter = "NodeLab project (*.nlproj)|*.nlproj|All files|*.*";

static bool isImageFile(const std::filesystem::path& p) { return isImageFile(pathToU8(p)); }
static const char* kDockName = "NodeLabDockSpace";

void dropCallback(GLFWwindow* w, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(w));
    for (int i = 0; i < count; ++i) app->drops_.emplace_back(paths[i]);
}

// Windows runs a modal loop while the main window is resized or moved, and glfwWaitEvents only
// returns when it ends: frames drawn from here meanwhile keep the window from showing black.
void refreshCallback(GLFWwindow* w) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(w));
    if (app->redraw_ && !app->inFrame_) app->redraw_();
}

namespace {
// New floating panels (ImGui viewports) are shown once their first frame is drawn: shown at
// creation, as ImGui does, a window is black until its first swap, which flashed.
void (*g_showWindow)(ImGuiViewport*) = nullptr;
std::vector<ImGuiID> g_toShow;
void deferShowWindow(ImGuiViewport* vp) { g_toShow.push_back(vp->ID); }
void showDeferredWindows() {
    for (ImGuiID id : g_toShow)
        if (ImGuiViewport* vp = ImGui::FindViewportByID(id); vp && vp->PlatformWindowCreated) g_showWindow(vp);
    g_toShow.clear();
}
}  // namespace

void closeCallback(GLFWwindow* w) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(w));
    glfwSetWindowShouldClose(w, GLFW_FALSE);
    app->requestAction(App::Pending::Quit);
}

// Per-user settings folder (%APPDATA%\NodeLab), created on demand.
static fs::path settingsDir() {
#ifdef _WIN32
    if (const wchar_t* appdata = _wgetenv(L"APPDATA")) {
        fs::path p = fs::path(appdata) / "NodeLab";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }
#endif
    return fs::current_path();
}

App::App() = default;
App::~App() {
    if (prefetch_.joinable()) prefetch_.join();  // it decodes into cache_
}

void App::loadPreferences() {
    try {
        std::ifstream f(settingsDir() / "preferences.json");
        if (!f) return;
        const nlohmann::json j = nlohmann::json::parse(f);
        gpuDevice_ = j.value("compositorDevice", std::string("GPU")) == "GPU";
        gpuFull_ = j.value("compositorPrecision", std::string("Auto")) == "Full";
        ml::setUseGpu(j.value("aiDevice", std::string("CPU")) == "GPU");
        inspectorOverlay_ = j.value("inspector", std::string("Overlay")) == "Overlay";
        editor_.showTimings = j.value("nodeTimings", editor_.showTimings);
        autosave_ = j.value("autosave", autosave_);
        autosaveMinutes_ = std::clamp(j.value("autosaveMinutes", autosaveMinutes_), 1, 120);
        const std::string layout = j.value("layout", std::string());
        for (int i = 0; i < kLayouts; ++i)
            if (layout == kLayoutNames[i]) layoutPreset_ = i;
        newView_ = j.value("newProjectView", std::string()) == "AgX" ? ColorManagement::AgX : ColorManagement::Standard;
        newLook_ = std::clamp(j.value("newProjectLook", 0), 0, 2);
        if (const auto c = j.find("customThemes"); c != j.end() && c->is_array())
            for (const auto& e : *c) customThemes_.push_back(theme::Theme::fromJson(e));
        if (const auto th = j.find("theme"); th != j.end()) theme::current() = theme::Theme::fromJson(*th);
        if (const auto ep = j.find("exportPresets"); ep != j.end() && ep->is_array())
            for (const auto& e : *ep)
                if (e.is_object() && e.value("name", std::string()).size()) {
                    ExportPreset p{e.value("name", std::string()), {}};
                    p.settings.fromJson(e.value("settings", nlohmann::json::object()));
                    exportPresets_.push_back(std::move(p));
                }
        library_.sortBy = std::clamp(j.value("librarySort", 0), 0, LibraryPanel::kSortCount - 1);
        library_.sortDescending = j.value("librarySortDescending", false);
        overlay_.cropGuide = std::clamp(j.value("cropGuide", int(NodeOverlay::Thirds)), 0, NodeOverlay::kCropGuideCount - 1);
        overlay_.cropGuideTurn = std::clamp(j.value("cropGuideTurn", 0), 0, 3);
        loupe_.grid = j.value("loupeGrid", false);
        loupe_.guides = j.value("loupeGuides", false);
        loupe_.gridSize = std::clamp(j.value("loupeGridSize", 50.0f), 8.0f, 400.0f);
    } catch (const std::exception&) {
        // A damaged file keeps the defaults; it is rewritten on the next change.
    }
    library::setDefaultView(newView_, newLook_);
    theme::apply();
}

void App::savePreferences() const {
    if (automated_) return;  // test runs never touch the user's preferences
    nlohmann::json custom = nlohmann::json::array();
    for (const theme::Theme& t : customThemes_) custom.push_back(t.toJson());
    nlohmann::json presets = nlohmann::json::array();
    for (const ExportPreset& p : exportPresets_) presets.push_back({{"name", p.name}, {"settings", p.settings.toJson()}});
    // A temporary file renamed over the old one, so a crash mid-write keeps the old preferences.
    const std::filesystem::path path = settingsDir() / "preferences.json", tmp = settingsDir() / "preferences.json.tmp";
    std::ofstream f(tmp, std::ios::trunc);
    f << nlohmann::json{{"compositorDevice", gpuDevice_ ? "GPU" : "CPU"},
                        {"compositorPrecision", gpuFull_ ? "Full" : "Auto"},
                        {"aiDevice", ml::useGpu() ? "GPU" : "CPU"},
                        {"inspector", inspectorOverlay_ ? "Overlay" : "Panel"},
                        {"nodeTimings", editor_.showTimings},
                        {"autosave", autosave_},
                        {"autosaveMinutes", autosaveMinutes_},
                        {"layout", kLayoutNames[layoutPreset_]},
                        {"newProjectView", newView_ == ColorManagement::AgX ? "AgX" : "Standard"},
                        {"newProjectLook", newLook_},
                        {"theme", theme::current().toJson()},
                        {"customThemes", custom},
                        {"exportPresets", presets},
                        {"librarySort", library_.sortBy},
                        {"librarySortDescending", library_.sortDescending},
                        {"cropGuide", overlay_.cropGuide},
                        {"cropGuideTurn", overlay_.cropGuideTurn},
                        {"loupeGrid", loupe_.grid},
                        {"loupeGuides", loupe_.guides},
                        {"loupeGridSize", loupe_.gridSize}}
             .dump(2);
    f.close();
    std::error_code ec;
    if (f) std::filesystem::rename(tmp, path, ec);
    else std::filesystem::remove(tmp, ec);
}

ColorManagement App::newProjectColor() const {
    ColorManagement cm = ColorManagement::sceneLinear();
    cm.view = newView_;
    cm.look = newView_ == ColorManagement::AgX ? newLook_ : ColorManagement::None;
    return cm;
}

// ---------------------------------------------------------------- main loop

int App::run(const RunOptions& opt) {
    UiScript script;
    if (!opt.script.empty()) {
        std::string err;
        if (!script.load(opt.script, err)) {
            std::fprintf(stderr, "script: %s\n", err.c_str());
            return 1;
        }
    }
    automated_ = !opt.screenshot.empty() || script.active();
    if (automated_) inspectorOverlay_ = false;
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // Automated runs use a fixed size and never take focus from whatever the user is doing.
    glfwWindowHint(GLFW_MAXIMIZED, automated_ ? GLFW_FALSE : GLFW_TRUE);
    if (automated_) {
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    }
    window_ = glfwCreateWindow(1600, 900, "NodeLab", nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "failed to create window (OpenGL 3.0 required)\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    glfwSetWindowUserPointer(window_, this);
    glfwSetDropCallback(window_, dropCallback);
    glfwSetWindowCloseCallback(window_, closeCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Panels can be dragged out of the main window into their own OS windows (not in test runs,
    // which need a deterministic single-window frame).
    if (!automated_) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Layout persists per user; automated runs always start from the default layout.
    iniPath_ = pathToU8(settingsDir() / "layout.ini");
    bool haveLayout = false;
    if (automated_) {
        io.IniFilename = nullptr;
    } else {
        io.IniFilename = iniPath_.c_str();
        haveLayout = fs::exists(u8ToPath(iniPath_));
    }
    resetLayout_ = !haveLayout;

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    const float dpi = std::max(1.0f, xscale);

    theme::apply();  // the default theme; loadPreferences applies the user's
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScaleAllSizes(dpi);

    const char* uiFont = "C:/Windows/Fonts/segoeui.ttf";
    if (fs::exists(uiFont)) io.Fonts->AddFontFromFileTTF(uiFont, 17.0f * dpi);
    else io.FontGlobalScale = dpi;
    // Extra faces for the guide: bold, headings and code. Glyphs cover the guide's own text.
    {
        static ImVector<ImWchar> ranges;
        ImFontGlyphRangesBuilder rb;
        rb.AddRanges(io.Fonts->GetGlyphRangesDefault());
        const std::string_view guide = guideMarkdown();
        rb.AddText(guide.data(), guide.data() + guide.size());
        rb.BuildRanges(&ranges);
        auto load = [&](const char* file, float size) -> ImFont* {
            return fs::exists(file) ? io.Fonts->AddFontFromFileTTF(file, size * dpi, nullptr, ranges.Data) : nullptr;
        };
        GuideFonts gf;
        gf.bold = load("C:/Windows/Fonts/segoeuib.ttf", 17.0f);
        gf.h1 = load("C:/Windows/Fonts/segoeuib.ttf", 30.0f);
        gf.h2 = load("C:/Windows/Fonts/segoeuib.ttf", 24.0f);
        gf.h3 = load("C:/Windows/Fonts/segoeuib.ttf", 20.0f);
        gf.code = load("C:/Windows/Fonts/consola.ttf", 16.0f);
        setGuideFonts(gf);
    }

    // While a script runs, OS input is not forwarded to ImGui so the real mouse can't interfere.
    ImGui_ImplGlfw_InitForOpenGL(window_, !script.active());
    ImGui_ImplOpenGL3_Init("#version 130");
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        g_showWindow = pio.Platform_ShowWindow;
        pio.Platform_ShowWindow = deferShowWindow;
    }

    // The GPU device: a hidden context sharing textures with the UI's, so viewers draw its
    // display textures directly. Test runs stay on the CPU so their screenshots don't depend on
    // the machine's GPU, unless given --device gpu.
    if (automated_) {
        gpuDevice_ = opt.gpu && gpu::init(&gpuError_, window_);
    } else {
        loadPreferences();
        if (!gpu::init(&gpuError_, window_)) gpuError_ = "GPU unavailable: " + gpuError_;
    }

    eval_ = std::make_unique<AsyncEvaluator>(cache_);
    auto main = std::make_unique<Viewer>();
    main->id = 0;
    viewers_.push_back(std::move(main));

    // An image instead of a project (NodeLab.exe photo.CR2, or Open with): a new project with it.
    std::error_code dirEc;
    if (!opt.project.empty() && fs::is_directory(u8ToPath(opt.project), dirEc)) {
        newProject();
        openFolder(opt.project);
    } else if (!opt.project.empty() && isImageFile(u8ToPath(opt.project))) {
        newProject();
        importImage(opt.project);
    } else if (opt.project.empty() || !openProject(opt.project)) {
        newProject();
    }

    auto renderMain = [&] {
        int fbw, fbh;
        glfwGetFramebufferSize(window_, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    };
    auto renderPlatformWindows = [&] {
        // Floating panels live in their own OS windows.
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup);
            showDeferredWindows();
        }
    };
    if (!automated_) {
        redraw_ = [&] {
            inFrame_ = true;
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            drawFrame();
            ImGui::Render();
            renderMain();
            renderPlatformWindows();
            glfwSwapBuffers(window_);
            GLTexture::endFrame();
            inFrame_ = false;
        };
        glfwSetWindowRefreshCallback(window_, refreshCallback);
    }

    int frame = 0, settled = 0;
    while (!quit_) {
        glfwWaitEventsTimeout(eval_->busy() || display_.busy() || evalDirty_ || automated_ ? 0.01 : 0.05);

        inFrame_ = true;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        std::string shotPath;
        if (script.active()) shotPath = script.step(idle(), quit_);
        ImGui::NewFrame();
        drawFrame();
        ImGui::Render();
        renderMain();

        if (!opt.screenshot.empty() && !script.active()) {
            // Plain --screenshot: wait for evaluation to settle and layout to stabilize.
            ++frame;
            settled = idle() ? settled + 1 : 0;
            if ((frame > 30 && settled > 10) || frame > 600) {
                shotPath = opt.screenshot;
                quit_ = true;
            }
        }
        if (!shotPath.empty() && !saveFramebuffer(shotPath)) std::fprintf(stderr, "screenshot failed\n");

        renderPlatformWindows();
        glfwSwapBuffers(window_);
        GLTexture::endFrame();
        inFrame_ = false;
    }

    glfwSetWindowRefreshCallback(window_, nullptr);
    redraw_ = nullptr;
    eval_.reset();
    // Viewers may hold device textures: return them to the pool before the device goes.
    leftTex_.reset();
    viewers_.clear();
    maskTex_.reset();
    glFinish();
    for (int i = 0; i < 8; ++i) GLTexture::endFrame();
    gpu::shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    return 0;
}

void App::drawFrame() {
    handleDrops();
    handleShortcuts();

    drawMainMenu();
    drawStatusBar();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID dockId = ImHashStr(kDockName);
    if (resetLayout_) {
        buildLayout(dockId);
        resetLayout_ = false;
    }
    ImGui::DockSpaceOverViewport(dockId, vp);

    originalDrawn_ = false;
    if (showOriginal_) drawOriginalWindow();
    editorShown_ = false;
    if (showEditor_) drawEditorWindow();
    if (showInspector_ && !inspectorOverlay_) drawInspectorWindow();
    if (showResult_) drawViewerWindow(*viewers_[0], true);
    for (size_t i = 1; i < viewers_.size(); ++i) drawViewerWindow(*viewers_[i], false);
    if (library_.active() && showLibrary_) drawLibraryWindow();
    if (showSnapshots_) drawSnapshotsWindow();
    if (library_.active() && library_.grid) drawLibraryGrid();
    gridShown_ = library_.active() && library_.grid;
    library_.poll();
    if (!library_.status.empty()) status_ = std::move(library_.status), library_.status.clear();
    if (inspectorOverlay_) drawInspectorOverlay();
    drawGuideWindow();
    drawExportWindow();
    drawPreferencesWindow();
    pollExport();
    std::erase_if(viewers_, [](const std::unique_ptr<Viewer>& v) { return v->id != 0 && !v->open; });

    drawUnsavedModal();
    drawConvertModal();

    // The selected node's on-image controls decide two extra things about the evaluation: a
    // selected Crop shows the whole frame (its rectangle is drawn instead, like Lightroom's crop
    // tool), and a selected mask is evaluated too, for the tinted mask overlay.
    Node* ov = overlayNode();
    NodePath ovPath;
    if (ov) ovPath = groupPath_, ovPath.push_back(ov->id);
    const bool wantMask = ov && NodeOverlay::isMask(*ov) && maskOverlay_;
    if (ovPath != overlayPath_ || wantMask != maskWanted_) {
        overlayPath_ = ovPath;
        maskWanted_ = wantMask;
        maskTex_.reset();
        dropDisplay(kSlotMask);
        evalDirty_ = true;
    }

    // An AI model was installed or removed: Select Subject and Select Sky re-run.
    if (const int g = ml::generation(); g != mlGeneration_) {
        mlGeneration_ = g;
        evalDirty_ = true;
    }
    // A model finished in the background: its nodes show the real mask now.
    if (const int g = AutoMaskNode::resultGeneration(); g != maskGeneration_) {
        maskGeneration_ = g;
        evalDirty_ = true;
    }
    // Kick evaluation after the UI had a chance to change the graph this frame.
    const bool gesture = ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left) || editor_.interacting();
    // The proxy follows the views' size; changed when no gesture (such as resizing a panel) is on.
    if (!gesture) {
        const int edge = wantedProxyEdge();
        if (edge != proxyEdge_) {
            proxyEdge_ = edge;
            evalDirty_ = true;
        }
    }
    // Zoomed-in views ask for details of what they show once the view has settled.
    const std::vector<AsyncEvaluator::Detail> wanted = wantedDetails();
    for (Viewer* v : detailViews()) {
        const int tag = v == &left_ ? -1 : v->id;
        if (std::none_of(wanted.begin(), wanted.end(), [&](const auto& d) { return d.tag == tag; })) {
            dropDetail(*v);  // not zoomed in (any more)
        }
        v->info = ViewInfo{};  // refilled when the view is drawn next frame
    }
    const double now = ImGui::GetTime();
    if (!sameDetails(wanted, lastWanted_)) {
        lastWanted_ = wanted;
        detailsChangedAt_ = now;
    }
    if (!gesture && !wanted.empty() && !sameDetails(wanted, details_) && now - detailsChangedAt_ > 0.25)
        evalDirty_ = true;
    // A gesture ended: replace its drafts with the full preview and its details.
    if (!gesture && refineAfterGesture_) {
        refineAfterGesture_ = false;
        evalDirty_ = true;
    }
    if (evalDirty_) {
        submittedTargets_.clear();
        std::vector<int> pins;
        for (auto& v : viewers_) {
            const bool followsPreview = v->id == 0 || v->pin.empty();
            submittedTargets_.push_back(followsPreview ? resultTarget() : v->pin);
            pins.push_back(followsPreview && pathValid(previewPath_) ? previewPin_ : 0);
        }
        submittedViewers_ = viewers_.size();
        if (maskWanted_) {
            submittedTargets_.push_back(overlayPath_);
            pins.push_back(0);
        }
        nlohmann::json gj;
        if (ov && ov->info().type == crop::kType) {
            const std::vector<nlohmann::json> saved = ov->params;
            ov->params[crop::Left] = 0.0f, ov->params[crop::Right] = 1.0f;
            ov->params[crop::Top] = 0.0f, ov->params[crop::Bottom] = 1.0f;
            ov->params[crop::Aspect] = 0;
            gj = graph_.toJson();
            ov->params = saved;
        } else {
            gj = graph_.toJson();
        }
        submittedPins_ = pins;
        AsyncEvaluator::Options opt;
        opt.proxyEdge = proxyEdge_;
        // A slow graph shows a half-size draft while dragging (progressive refinement), and
        // details wait for the gesture to end.
        opt.draft = gesture && previewMs_ > kDraftAfterMs;
        opt.gpu = gpuDevice_ && gpu::available();
        opt.gpuHalf = !gpuFull_;
        if (!gesture) {
            opt.details = wanted;
            details_ = wanted;
        }
        refineAfterGesture_ = gesture && (opt.draft || !wanted.empty());
        eval_->submit(std::move(gj), submittedTargets_, pins, !gesture, std::move(opt));
        evalDirty_ = false;
    }
    // A drag just ended: its last value is queued behind an intermediate one; skip the latter.
    if (gestureWas_ && !gesture) eval_->preempt();
    gestureWas_ = gesture;
    applyRawLook();
    updateTextures();
    updateTitle();
    tickPrefetch();

    // Snapshot for undo once the current gesture (drag, slider, text entry) has finished, so one
    // drag becomes one undo step.
    if (historyDirty_ && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !editor_.interacting()) {
        commitHistory();
        historyDirty_ = false;
    }
    tickAutosave();
}

void App::tickAutosave() {
    // Only a project that has been saved once (or a library photo, whose sidecar is its file):
    // an untitled one waits for the first Save to say where it belongs.
    if (!modified_ || (projectPath_.empty() && !libraryPhotoOpen())) {
        unsavedSince_ = -1;
        return;
    }
    const double now = ImGui::GetTime();
    if (unsavedSince_ < 0) unsavedSince_ = now;
    // Test runs never write the user's files; and never in the middle of a drag or a typed value.
    if (automated_ || !autosave_ || now - unsavedSince_ < autosaveMinutes_ * 60.0 || ImGui::IsAnyItemActive() ||
        ImGui::IsMouseDown(ImGuiMouseButton_Left) || editor_.interacting())
        return;
    unsavedSince_ = now;  // a failed save tries again after another interval
    if (libraryPhotoOpen()) {
        saveLibraryPhoto();
        return;
    }
    std::string err;
    if (::saveProject(projectPath_, graph_, uiState(), err)) {
        modified_ = false;
        status_ = "Auto saved " + pathToU8(u8ToPath(projectPath_).filename());
    } else {
        status_ = "Auto save failed: " + err;
    }
}

// ---------------------------------------------------------------- layout & panels

void App::buildLayout(unsigned dockIdU) {
    const ImGuiID dockId = dockIdU;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);
    ImGuiID center = dockId;
    auto split = [](ImGuiID& node, ImGuiDir dir, float ratio) {
        return ImGui::DockBuilderSplitNode(node, dir, ratio, nullptr, &node);
    };
    // The Library's filmstrip runs along the bottom, under everything (it shows only while a
    // folder is open; the panels above take its space otherwise).
    const ImGuiID filmstrip = split(center, ImGuiDir_Down, 0.2f);
    // With the inspector overlay there is no Inspector panel to make room for.
    const bool panel = !inspectorOverlay_;
    ImGuiID original = 0, result = 0, editor = 0, inspector = 0;
    switch (layoutPreset_) {
        default:
        case LayoutDefault:
            original = split(center, ImGuiDir_Left, 0.27f);
            result = split(center, ImGuiDir_Right, 0.37f);
            if (panel) inspector = split(center, ImGuiDir_Down, 0.34f);
            editor = center;
            break;
        case LayoutCompositing:
            // Blender's Compositing workspace: the backdrop (Result) above a wide node editor;
            // Original is a tab behind Result.
            result = original = split(center, ImGuiDir_Up, 0.55f);
            if (panel) inspector = split(center, ImGuiDir_Right, 0.3f);
            editor = center;
            break;
        case LayoutPhoto:
            // Lightroom's Develop module: the photo in the middle, its settings on the right,
            // the graph tucked underneath, the original on the left.
            if (panel) inspector = split(center, ImGuiDir_Right, 0.26f);
            original = split(center, ImGuiDir_Left, 0.22f);
            editor = split(center, ImGuiDir_Down, 0.32f);
            result = center;
            break;
        case LayoutSideBySide: {
            // Before / after at equal sizes over the graph.
            ImGuiID top = split(center, ImGuiDir_Up, 0.58f);
            original = split(top, ImGuiDir_Left, 0.5f);
            result = top;
            if (panel) inspector = split(center, ImGuiDir_Right, 0.34f);
            editor = center;
            break;
        }
        case LayoutNodeFocus: {
            // A big node editor, the images stacked in a column on the right.
            ImGuiID column = split(center, ImGuiDir_Right, 0.3f);
            result = split(column, ImGuiDir_Up, panel ? 0.4f : 0.5f);
            if (panel) inspector = split(column, ImGuiDir_Down, 0.45f);
            original = column;
            editor = center;
            break;
        }
    }
    ImGui::DockBuilderDockWindow("###Original", original);
    ImGui::DockBuilderDockWindow("###Result", result);
    ImGui::DockBuilderDockWindow("###NodeEditor", editor);
    if (inspector) ImGui::DockBuilderDockWindow("###Inspector", inspector);
    ImGui::DockBuilderDockWindow("###Library", filmstrip);
    for (size_t i = 1; i < viewers_.size(); ++i)
        ImGui::DockBuilderDockWindow(("###Viewer" + std::to_string(viewers_[i]->id)).c_str(), result);
    ImGui::DockBuilderFinish(dockId);
    // Where Original shares Result's node, Result is the tab in front.
    if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(result)) n->SelectedTabId = ImHashStr("###Result");
    showOriginal_ = showEditor_ = showInspector_ = showResult_ = showLibrary_ = true;
}

static constexpr ImGuiWindowFlags kCanvasFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

void App::drawOriginalWindow() {
    if (ImGui::Begin("Original###Original", &showOriginal_, kCanvasFlags)) {
        PickRequest pick{leftShown_.get()};
        originalDrawn_ = true;
        drawImageView("##leftview", leftTex_, view_, "Drop an image here or use File > Import Image",
                      eyedropper().active() ? &pick : nullptr, nullptr,
                      left_.detailTex.valid() ? &left_.detail : nullptr, &left_.info);
        finishPick(pick);
    }
    ImGui::End();
}

void App::drawEditorWindow() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    const bool visible = ImGui::Begin("Node Editor###NodeEditor", &showEditor_, kCanvasFlags);
    ImGui::PopStyleVar();
    if (visible) {
        editorShown_ = true;
        const ImVec2 wp = ImGui::GetWindowPos(), c0 = ImGui::GetWindowContentRegionMin(),
                     c1 = ImGui::GetWindowContentRegionMax();
        editorMin_ = ImVec2(wp.x + c0.x, wp.y + c0.y);
        editorMax_ = ImVec2(wp.x + c1.x, wp.y + c1.y);
        editorViewport_ = ImGui::GetWindowViewport()->ID;
        // Breadcrumb: Root > Group > ...; click a level to go back up.
        if (!groupPath_.empty()) {
            if (ImGui::SmallButton("Root")) setGroupPath({});
            Graph* g = &graph_;
            for (size_t i = 0; i < groupPath_.size() && g; ++i) {
                auto* grp = dynamic_cast<GroupNode*>(g->find(groupPath_[i]));
                if (!grp) break;
                ImGui::SameLine();
                ImGui::TextDisabled(">");
                ImGui::SameLine();
                ImGui::PushID(int(i));
                if (ImGui::SmallButton(grp->name.c_str()) && i + 1 < groupPath_.size())
                    setGroupPath(std::vector<int>(groupPath_.begin(), groupPath_.begin() + i + 1));
                ImGui::PopID();
                g = &grp->inner();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("  (Tab to exit group)");
        }

        Graph& g = currentGraph();
        // The editor works on ids within the current graph; map the preview path in and out.
        int preview = 0;
        if (previewPath_.size() == groupPath_.size() + 1 &&
            std::equal(groupPath_.begin(), groupPath_.end(), previewPath_.begin()))
            preview = previewPath_.back();
        const int previewBefore = preview;
        // Timings are for the top-level graph; ids inside a group mean different nodes.
        editor_.setTimings(groupPath_.empty() ? nodeMs_ : std::unordered_map<int, double>{},
                           groupPath_.empty() ? nodeGpu_ : std::unordered_map<int, bool>{});
        editor_.insideGroup = !groupPath_.empty();
        NodeEditor::Result r = editor_.draw(g, selected_, preview, previewPin_);
        // Value Input / Output nodes added, renamed or deleted inside the group change its sockets.
        if (GroupNode* owner = currentGroupOwner()) {
            Graph* parent = resolveGroupPath(graph_, std::vector<int>(groupPath_.begin(), groupPath_.end() - 1));
            if (parent && owner->syncValueNodes(*parent)) r.evalChanged = r.docChanged = true;
        }
        if (preview != previewBefore || r.previewChanged) {
            previewPath_.clear();
            if (preview) {
                previewPath_ = groupPath_;
                previewPath_.push_back(preview);
            }
            evalDirty_ = true;
        }
        if (r.evalChanged) evalDirty_ = true;
        if (r.docChanged) {
            modified_ = true;
            historyDirty_ = true;
        }
        if (r.openViewer) {
            NodePath pin = groupPath_;
            pin.push_back(r.openViewer);
            openViewer(std::move(pin));
        }
        if (r.maskSelection >= 0) {
            Graph& g = currentGraph();
            const recipes::AddedMask m =
                recipes::maskNodes(g, std::set<int>(r.maskNodes.begin(), r.maskNodes.end()), recipes::MaskKind(r.maskSelection));
            if (m.ok()) {
                selected_ = m.mask;
                editor_.select(m.mask);
                status_ = "Added " + g.find(m.adjust)->label + ": the selected nodes apply only where " +
                          g.find(m.mask)->info().displayName + " is white";
                markChanged(true);
            } else {
                status_ = "Mask Selected Nodes needs nodes with an image output";
            }
        }
        if (r.enterGroup) enterGroup(r.enterGroup);
        else if (r.exitGroup) exitGroup();
        else if (r.findNode) {
            auto p = groupPath_;
            p.insert(p.end(), r.findPath.begin(), r.findPath.end());
            setGroupPath(p);
            editor_.select(r.findNode);
            editor_.frameSelectionNext();
        }
    }
    editorFocused_ = visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    ImGui::End();
}

void App::drawInspectorWindow() {
    if (ImGui::Begin("Inspector###Inspector", &showInspector_)) drawInspectorContents();
    ImGui::End();
}

// The Inspector as a panel floating over the Node Editor's top-right corner while a node is
// selected (Preferences > Interface > Inspector: Overlay), like the sidebar of Blender's node
// editor. It is its own top-level window rather than a child of the editor's, so the editor
// doesn't take its mouse wheel or keys (X deletes nodes there).
void App::drawInspectorOverlay() {
    if (!editorShown_ || !showInspector_ || !selected_ || !currentGraph().find(selected_)) return;
    const float margin = 8.0f * ImGui::GetFontSize() / 17.0f;
    const ImVec2 avail(editorMax_.x - editorMin_.x, editorMax_.y - editorMin_.y);
    const float w = std::min(ImGui::GetFontSize() * 22.0f, avail.x * 0.55f);
    const float maxH = avail.y - margin * 2;
    if (w < 120.0f || maxH < 80.0f) return;
    ImGui::SetNextWindowViewport(editorViewport_);
    ImGui::SetNextWindowPos(ImVec2(editorMax_.x - margin, editorMin_.y + margin), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, maxH));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    if (ImGui::Begin("Inspector##overlay", nullptr, flags)) {
        // Clicking the editor raises it over everything docked; keep the overlay on top. Not
        // while a popup is open, though: raising the overlay would cover its own dropdown lists
        // (a combo's items opened behind it and couldn't be clicked).
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        drawInspectorContents();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void App::drawInspectorContents() {
    Graph& g = currentGraph();
    Graph* parent = groupPath_.empty()
                        ? nullptr
                        : resolveGroupPath(graph_, std::vector<int>(groupPath_.begin(), groupPath_.end() - 1));
    if (drawInspector(g, selected_, currentGroupOwner(), parent)) markChanged(true);
    if (autoToneRequest) applyAutoTone(std::exchange(autoToneRequest, 0));
    // A mask driving a Basic's Factor (as Add Mask builds) shows that adjustment's sliders
    // too, the way Lightroom shows a mask's settings and its adjustments together.
    for (const Link& l : g.links())
        if (l.fromNode == selected_ && l.toPin == 1)
            if (const Node* adj = g.find(l.toNode); adj && adj->info().type == "color.basic") {
                ImGui::Spacing();
                ImGui::SeparatorText(adj->title().c_str());
                ImGui::PushID("##adjustment");
                if (drawInspector(g, adj->id, currentGroupOwner(), parent)) markChanged(true);
                if (autoToneRequest) applyAutoTone(std::exchange(autoToneRequest, 0));
                ImGui::PopID();
                break;
            }
}

void App::drawViewerWindow(Viewer& v, bool isMain) {
    std::string title;
    if (isMain) title = "Result###Result";
    else title = "Viewer " + std::to_string(v.id) + "###Viewer" + std::to_string(v.id);
    bool* open = isMain ? &showResult_ : &v.open;
    if (!isMain) {
        // New viewers float in the middle of the window (cascaded) until docked somewhere.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float step = 30.0f * float((v.id - 1) % 6);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f + step, vp->WorkPos.y + vp->WorkSize.y * 0.45f + step),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    }
    if (ImGui::Begin(title.c_str(), open, kCanvasFlags)) {
        // Toolbar: what is shown, and pin controls for extra viewers.
        NodePath shown = isMain || v.pin.empty() ? resultTarget() : v.pin;
        std::string label = shown.empty() ? "(nothing)" : pathLabel(shown);
        if (isMain && !previewPath_.empty()) label = "Preview: " + label + "  (Ctrl+click it again to clear)";
        if (!isMain) {
            // Any node of the graph being edited, left to right, so a chain a -> b -> c reads in order.
            ImGui::SetNextItemWidth(std::min(240.0f, ImGui::GetContentRegionAvail().x * 0.5f));
            const std::string current = v.pin.empty() ? "Follow Result" : pathLabel(v.pin);
            if (ImGui::BeginCombo("##node", current.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("Follow Result", v.pin.empty())) {
                    v.pin.clear();
                    evalDirty_ = true;
                }
                ImGui::Separator();
                std::vector<const Node*> nodes;
                for (const auto& [id, n] : currentGraph().nodes()) nodes.push_back(n.get());
                std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) {
                    return a->x != b->x ? a->x < b->x : a->y < b->y;
                });
                for (const Node* n : nodes) {
                    NodePath p = groupPath_;
                    p.push_back(n->id);
                    ImGui::PushID(n->id);
                    if (ImGui::Selectable(n->title().c_str(), v.pin == p)) {
                        v.pin = std::move(p);
                        evalDirty_ = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Node shown in this viewer");
            ImGui::SameLine();
            if (ImGui::SmallButton("Pin Selected") && selected_) {
                v.pin = groupPath_;
                v.pin.push_back(selected_);
                evalDirty_ = true;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Sync view", &v.sync);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom and pan together with Original and Result");
        }
        Node* ov = isMain ? overlayNode() : nullptr;
        if (isMain) {
            drawResultToolbar(ov);
            if (!previewPath_.empty()) {
                // Only worth mentioning when showing something other than the Output node.
                ImGui::SameLine();
                ImGui::TextDisabled("%s", label.c_str());
            }
        }
        const char* emptyMsg = !v.error.empty() ? v.error.c_str()
                               : shown.empty() ? "Add an Output node (right-click the canvas)"
                                               : "No output yet - connect this node's inputs";
        if (eyedropper().active() && !v.shown && !v.shownGpu.empty()) v.shown = DisplayWorker::download(v.shownGpu);
        PickRequest pick{v.shown.get()};
        overlay_.set(ov, maskWanted_ && maskTex_.valid() ? &maskTex_ : nullptr);
        const ImVec2 viewMin = ImGui::GetCursorScreenPos();
        SplitView split;
        if (isMain && (splitView_ || beforeFull_)) {
            split.before = &leftTex_;
            split.beforeDetail = left_.detailTex.valid() ? &left_.detail : nullptr;
            // The Original window asks for its own detail when it's showing.
            split.beforeInfo = originalDrawn_ ? nullptr : &left_.info;
            split.pos = &splitPos_;
            split.dragging = &splitDrag_;
            split.full = beforeFull_;
        }
        ImageOverlay* controls = ov ? &overlay_ : nullptr;
        if (isMain && loupe_.any()) {
            loupe_.inner = controls;
            controls = &loupe_;
        }
        drawImageView(isMain ? "##result" : "##viewer", v.tex, isMain || v.sync ? view_ : v.view, emptyMsg,
                      eyedropper().active() ? &pick : nullptr, controls,
                      v.detailTex.valid() ? &v.detail : nullptr, &v.info, split.before ? &split : nullptr);
        finishPick(pick);
        if (ov && overlay_.takeChanged()) markChanged(true);
        if (auto* sr = dynamic_cast<SpotRemovalNode*>(ov); sr && sr->findSource >= 0) findSpotSource(*sr);
        if (isMain && showHistogram_ && histogram_.valid) {
            // Top-right corner of the view, like Lightroom's histogram panel.
            const ImVec2 viewMax = ImGui::GetItemRectMax();
            const ImVec2 size(std::min(256.0f, viewMax.x - viewMin.x - 16.0f), 110.0f);
            if (size.x > 60.0f && viewMax.y - viewMin.y > size.y + 16.0f &&
                drawHistogram(ImGui::GetWindowDrawList(), ImVec2(viewMax.x - size.x - 8.0f, viewMin.y + 8.0f), size,
                              histogram_, clipping_)) {
                clipping_ = !clipping_;
                refreshDisplay(v, true);
                refreshDetail(v, true);
            }
        }
    }
    ImGui::End();
}

Node* App::overlayNode() {
    Node* n = currentGraph().find(selected_);
    return n && NodeOverlay::supports(*n) ? n : nullptr;
}

// SameLine when an item `w` wide still fits the window's width, otherwise a new row, so a toolbar
// wraps in a narrow panel instead of hiding its last controls past the edge.
static void sameLineIfFits(float w) {
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w <= right) ImGui::SameLine();
}

static float checkboxWidth(const char* label) {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x;
}

void App::drawResultToolbar(Node* ov) {
    Viewer& v = *viewers_[0];
    // Hotkeys while the pointer is over the Result viewer (J and O as in Lightroom).
    const bool hover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::GetIO().WantTextInput &&
                       !ImGui::GetIO().KeyCtrl;
    bool clipToggled = false;
    if (hover && ImGui::IsKeyPressed(ImGuiKey_J, false)) clipping_ = !clipping_, clipToggled = true;
    // O: with Crop selected it cycles the crop guide overlay (Shift+O turns it), as in
    // Lightroom's crop tool; otherwise it shows or hides the mask overlay.
    if (hover && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        if (ov && ov->info().type == crop::kType) {
            overlay_.cycleCropGuide(ImGui::GetIO().KeyShift);
            status_ = std::string("Crop overlay: ") + NodeOverlay::cropGuideName(overlay_.cropGuide);
        } else {
            maskOverlay_ = !maskOverlay_;
        }
    }
    if (hover && ImGui::IsKeyPressed(ImGuiKey_H, false)) showHistogram_ = !showHistogram_;
    // Lightroom: Y splits Before / After, \ shows the before image alone while toggled.
    if (hover && ImGui::IsKeyPressed(ImGuiKey_Y, false)) splitView_ = !splitView_, beforeFull_ = false;
    if (hover && ImGui::IsKeyPressed(ImGuiKey_Backslash, false)) beforeFull_ = !beforeFull_;
    // Lightroom's mask shortcuts: Shift+M opens the menu, M linear, Shift+R radial, K brush.
    const bool shift = ImGui::GetIO().KeyShift;
    bool openMaskMenu = false;
    if (hover && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        if (shift) openMaskMenu = true;
        else addMask(int(recipes::MaskKind::Linear));
    }
    if (hover && shift && ImGui::IsKeyPressed(ImGuiKey_R, false)) addMask(int(recipes::MaskKind::Radial));
    if (hover && !shift && ImGui::IsKeyPressed(ImGuiKey_K, false)) addMask(int(recipes::MaskKind::Brush));

    if (ImGui::SmallButton("Add Mask") || openMaskMenu) ImGui::OpenPopup("##addmask");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adjust part of the image: adds a Basic before the Output, driven by a new mask (Shift+M)");
    if (ImGui::BeginPopup("##addmask")) {
        if (ImGui::MenuItem("Linear Gradient", "M")) addMask(int(recipes::MaskKind::Linear));
        if (ImGui::MenuItem("Radial Gradient", "Shift+R")) addMask(int(recipes::MaskKind::Radial));
        if (ImGui::MenuItem("Brush", "K")) addMask(int(recipes::MaskKind::Brush));
        if (ImGui::MenuItem("Luminance Range")) addMask(int(recipes::MaskKind::Range));
        // Lightroom's AI masks; the Inspector offers the model's download the first time.
        ImGui::Separator();
        if (ImGui::MenuItem("Subject")) addMask(int(recipes::MaskKind::Subject));
        if (ImGui::MenuItem("Sky")) addMask(int(recipes::MaskKind::Sky));
        if (ImGui::MenuItem("Background")) addMask(int(recipes::MaskKind::Background));
        // Keyboard navigation is off (Tab enters groups), so Escape doesn't close popups by itself.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    sameLineIfFits(checkboxWidth("Histogram"));
    ImGui::Checkbox("Histogram", &showHistogram_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the histogram (H)");
    sameLineIfFits(checkboxWidth("Clipping"));
    clipToggled |= ImGui::Checkbox("Clipping", &clipping_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show clipped highlights in red and crushed shadows in blue (J)");
    sameLineIfFits(checkboxWidth("Before / After"));
    if (ImGui::Checkbox("Before / After", &splitView_)) beforeFull_ = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Split the view: the original image left of the divider, the result right of it (Y).\n"
                          "Drag the divider to move it; \\ shows the whole original");
    if (clipToggled) {
        refreshDisplay(v, true);
        refreshDetail(v, true);
    }
    if (ov && NodeOverlay::isMask(*ov)) {
        sameLineIfFits(checkboxWidth("Mask Overlay"));
        ImGui::Checkbox("Mask Overlay", &maskOverlay_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tint the selected mask over the image (O)");
    }
    if (ov) {
        const char* hint = ov->info().type == crop::kType
                               ? "Drag the frame or its handles; drag outside to straighten; O cycles the overlay"
                           : ov->info().type == perspective::kType
                               ? (ov->paramI(perspective::Upright) == perspective::UprightGuided
                                      ? "Drag along a line that should be straight to add a guide (up to 4); Alt+click removes one"
                                      : "Set Upright to Guided to draw guides")
                           : dynamic_cast<BrushMaskNode*>(ov) ? "Paint to add, Alt+paint to erase, [ ] brush size"
                           : dynamic_cast<SpotRemovalNode*>(ov) ? "Click to add a spot, drag to move, Alt+click removes, [ ] size"
                                                              : "Drag the handles to shape the mask";
        sameLineIfFits(ImGui::CalcTextSize(hint).x);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", hint);
        ImGui::PopTextWrapPos();
    }
}

void App::applyAutoTone(int nodeId) {
    Graph& g = currentGraph();
    Node* basic = g.find(nodeId);
    const Link* in = nullptr;
    for (const Link& l : g.links())
        if (l.toNode == nodeId && l.toPin == 0) in = &l;
    if (!basic || !in) {
        status_ = "Auto needs an image connected to the Basic node";
        return;
    }
    // The image arriving at the node, at a small preview size: statistics don't need more, and
    // this runs on the UI thread. The decoded proxy comes from the shared image cache.
    ImagePtr img;
    try {
        Evaluator ev;
        EvalContext ctx;
        ctx.cache = &cache_;
        ctx.proxyEdge = 512;
        ctx.interactive = true;  // on the UI thread: never wait for an AI model
        initContextSize(graph_, ctx);
        NodePath path = groupPath_;
        path.push_back(in->fromNode);
        img = ev.evaluateDisplayPath(graph_, path, ctx, in->fromPin);
    } catch (const std::exception& e) {
        status_ = std::string("Auto failed: ") + e.what();
        return;
    }
    if (!img || img->empty()) {
        status_ = "Auto needs an image connected to the Basic node";
        return;
    }
    const autotone::Settings s = autotone::compute(*img, graph_.colorManagement.linear);
    const float v[6] = {s.exposure, s.contrast, s.highlights, s.shadows, s.whites, s.blacks};
    for (int i = 0; i < 6; ++i) basic->params[autotone::kBasicParams[i]] = v[i];
    status_ = "Auto tone set on " + basic->title();
    markChanged(true);
}

void App::findSpotSource(SpotRemovalNode& node) {
    const int index = std::exchange(node.findSource, -1);
    const Link* in = currentGraph().inputLink(node.id, 0);
    if (!in || index >= int(node.spots.size())) return;
    // As Auto tone does: the image arriving at the node, small (the search compares rings of a
    // few dozen samples), on the UI thread.
    ImagePtr img;
    try {
        Evaluator ev;
        EvalContext ctx;
        ctx.cache = &cache_;
        ctx.proxyEdge = 768;
        ctx.interactive = true;
        initContextSize(graph_, ctx);
        NodePath path = groupPath_;
        path.push_back(in->fromNode);
        img = ev.evaluateDisplayPath(graph_, path, ctx, in->fromPin);
    } catch (const std::exception& e) {
        status_ = std::string("Finding a source failed: ") + e.what();
        return;
    }
    if (!img || img->empty()) return;
    if (::findSpotSource(*img, node.spots, index, node.findAvoidCurrent)) markChanged(true);
    else status_ = "No room for a source around this spot";
}

void App::addMask(int kind) {
    Graph& g = currentGraph();
    const recipes::AddedMask m = recipes::addMask(g, recipes::MaskKind(kind));
    if (!m.ok()) {
        status_ = "Add Mask needs an Output node with an image connected";
        return;
    }
    // Select the mask so its handles (or the brush) are ready on the Result.
    selected_ = m.mask;
    editor_.select(m.mask);
    status_ = "Added " + g.find(m.adjust)->label + ": " + g.find(m.mask)->info().displayName + " driving a Basic";
    markChanged(true);
}

void App::drawStatusBar() {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            const Viewer& main = *viewers_[0];
            std::string st;
            if (main.tex.valid())
                st = std::to_string(main.tex.width()) + " x " + std::to_string(main.tex.height()) + " preview  |  " +
                     std::to_string(int(evalMs_)) + " ms";
            if (eval_->busy()) st += "  |  evaluating...";
            if (gpuFallbacks_)
                st += "  |  " + std::to_string(gpuFallbacks_) + (gpuFallbacks_ == 1 ? " node" : " nodes") +
                      " ran on the CPU (" + gpuError_ + ")";
            if (exporter_.busy()) {
                const Exporter::Progress pr = exporter_.progress();
                st += "  |  Export " + std::to_string(pr.done) + "/" + std::to_string(pr.total) + ": " + pr.stage;
            }
            if (!main.error.empty()) st += "  |  " + main.error;
            if (!status_.empty()) st += "  |  " + status_;
            if (eyedropper().active()) {
                const Node* n = currentGraph().find(eyedropper().node);
                st = "Eyedropper: click a pixel or drag a rectangle on an image";
                if (n && eyedropper().param < int(n->info().params.size()))
                    st += " for " + n->title() + " > " + n->info().params[eyedropper().param].name;
                st += "   (right-click or Esc cancels)";
            }
            ImGui::TextDisabled("%s", st.c_str());
            drawStatusRight();
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

namespace {
std::string gigabytes(uint64_t b) {
    char s[32];
    std::snprintf(s, sizeof s, b >= (uint64_t(1) << 30) ? "%.1f GB" : "%.0f MB",
                  b >= (uint64_t(1) << 30) ? b / double(1 << 30) : b / double(1 << 20));
    return s;
}
}  // namespace

// Right of the status bar: an AI model running in the background, and the computer's load.
void App::drawStatusRight() {
    stats_.update();
    const AutoMaskNode::Progress pr = AutoMaskNode::progress();
    std::string ai;
    float fraction = 0;
    if (pr.running) {
        const char* what = pr.model.rfind("subject", 0) == 0 ? "Selecting subject" : pr.model == "sky" ? "Selecting sky" : "AI mask";
        char s[96];
        if (pr.loading)
            std::snprintf(s, sizeof s, "%s: loading the model, %.0f s", what, pr.seconds);
        else
            std::snprintf(s, sizeof s, "%s: %.0f s of about %.0f s", what, pr.seconds, pr.expected);
        ai = s;
        // Time, as the runtime reports no progress: held short of the end when it runs long.
        fraction = float(std::min(pr.seconds / std::max(pr.expected, 1.0), 0.95));
    }
    const std::string mem = "NodeLab " + gigabytes(stats_.privateBytes);
    char load[96];
    std::snprintf(load, sizeof load, "RAM %s / %s   CPU %.0f%%", gigabytes(stats_.ramUsed).c_str(),
                  gigabytes(stats_.ramTotal).c_str(), stats_.systemCpu * 100.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float barW = 120.0f;
    float w = ImGui::CalcTextSize(mem.c_str()).x + ImGui::CalcTextSize(load).x + style.ItemSpacing.x * 3 + 8.0f;
    if (pr.running) w += ImGui::CalcTextSize(ai.c_str()).x + barW + style.ItemSpacing.x * 2;
    const float x = ImGui::GetWindowContentRegionMax().x - w;
    if (x < ImGui::GetCursorPosX()) return;  // no room
    ImGui::SetCursorPosX(x);
    if (pr.running) {
        ImGui::TextUnformatted(ai.c_str());
        if (pr.queued && ImGui::IsItemHovered())
            ImGui::SetTooltip("%d more waiting", pr.queued);
        ImGui::ProgressBar(fraction, ImVec2(barW, ImGui::GetTextLineHeight() * 0.6f), "");
        ImGui::SameLine(0, style.ItemSpacing.x * 2);
    }
    ImGui::TextDisabled("%s", mem.c_str());
    const bool low = stats_.ramTotal && stats_.ramUsed > stats_.ramTotal / 10 * 9;
    if (low)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", load);
    else
        ImGui::TextDisabled("%s", load);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("NodeLab: %s committed, %s in RAM (peak %s), CPU %.0f%% of %d threads",
                    gigabytes(stats_.privateBytes).c_str(), gigabytes(stats_.workingSet).c_str(),
                    gigabytes(stats_.peakWorkingSet).c_str(), stats_.cpu * 100.0f, stats_.cores);
        ImGui::Text("System: %s of %s RAM in use, CPU %.0f%%", gigabytes(stats_.ramUsed).c_str(),
                    gigabytes(stats_.ramTotal).c_str(), stats_.systemCpu * 100.0f);
        if (gpu::available())
            ImGui::Text("GPU textures: %s in use, %s pooled", gigabytes(gpu::bytesInUse()).c_str(),
                        gigabytes(gpu::bytesPooled()).c_str());
        if (low) ImGui::TextUnformatted("Memory is nearly full: Windows swaps to disk, which slows everything down.");
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------- menus & shortcuts

// Blender's Render Properties > Color Management. The view settings only change how the viewers
// and exports show the result, so they don't re-evaluate the graph.
void App::drawColorMenu() {
    if (!ImGui::BeginMenu("Color")) return;
    ColorManagement& cm = graph_.colorManagement;
    ImGui::TextDisabled(cm.linear ? "Working space: Scene-Linear (Rec.709)" : "Working space: Legacy (sRGB-encoded)");
    ImGui::Separator();
    ImGui::BeginDisabled(!cm.linear);
    bool changed = false;
    ImGui::TextUnformatted("View Transform");
    for (int i = 0; i < 3; ++i)
        if (ImGui::RadioButton(colormgmt::kViewNames[i], &cm.view, i)) changed = true;
    ImGui::BeginDisabled(cm.view != ColorManagement::AgX);
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::Combo("Look", &cm.look, colormgmt::kLookNames, 3);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::DragFloat("Exposure", &cm.exposure, 0.01f, -10.0f, 10.0f, "%.2f");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) cm.exposure = 0.0f, changed = true;
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::DragFloat("Gamma", &cm.gamma, 0.005f, 0.01f, 5.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) cm.gamma = 1.0f, changed = true;
    ImGui::EndDisabled();
    if (changed) markChanged(false);
    ImGui::Separator();
    if (ImGui::MenuItem("Convert Project to Scene-Linear...", nullptr, false, !cm.linear))
        convertPrompt_ = true;
    ImGui::EndMenu();
}

void App::drawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) requestAction(Pending::New);
        if (ImGui::MenuItem("Open Project...", "Ctrl+O")) requestAction(Pending::Open);
        if (ImGui::MenuItem("Open Folder...", "Ctrl+Shift+O")) requestAction(Pending::OpenFolder);
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveProject(false);
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) saveProject(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Import Image...", "Ctrl+I"))
            if (auto p = openFileDialog("Import image", kImageFileFilter)) importImage(*p);
        if (ImGui::MenuItem("Export...", "Ctrl+E")) openExportWindow();
        if (ImGui::MenuItem("Write File Outputs", nullptr, false, !exporter_.busy())) startExport({{"", ""}}, 0);
        if (library_.active()) {
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Edit", "Ctrl+Shift+C", false, libraryPhotoOpen())) copyEdit();
            if (ImGui::MenuItem("Paste Edit", "Ctrl+Shift+V", false, !copiedEdit_.is_null())) pasteEdit();
            if (ImGui::MenuItem("Export Selected Photos...", nullptr, false, !exporter_.busy())) exportSelected();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) requestAction(Pending::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        Graph& g = currentGraph();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editor_.hasSelection()) && editor_.duplicateSelection(g))
            markChanged(true);
        int preview = 0;
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, editor_.hasSelection())) editor_.copySelection(g);
        if (ImGui::MenuItem("Paste", "Ctrl+V") && editor_.paste(g)) markChanged(true);
        if (ImGui::MenuItem("Delete (reconnect)", "Del / X", false, editor_.hasSelection()) && editor_.deleteSelection(g, preview, true))
            markChanged(true);
        if (ImGui::MenuItem("Delete", "Alt+Del", false, editor_.hasSelection()) && editor_.deleteSelection(g, preview, false))
            markChanged(true);
        if (ImGui::MenuItem("Mute", "M", false, editor_.hasSelection()) && editor_.toggleMute(g)) markChanged(true);
        if (ImGui::MenuItem("Collapse", "H", false, editor_.hasSelection()) && editor_.toggleCollapse(g)) markChanged(false);
        if (ImGui::MenuItem("Make Links", "F", false, editor_.hasSelection()) && editor_.makeLinks(g)) markChanged(true);
        if (ImGui::MenuItem("Reset to Defaults", nullptr, false, editor_.hasSelection()) && editor_.resetSelection(g))
            markChanged(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Group Selected", "Ctrl+G", false, editor_.hasSelection()) && editor_.groupSelection(g))
            markChanged(true);
        if (ImGui::MenuItem("Ungroup", "Ctrl+Alt+G", false, editor_.selectedGroup(g) != 0) && editor_.ungroupSelection(g))
            markChanged(true);
        if (ImGui::MenuItem("Edit Group", "Tab", false, editor_.selectedGroup(g) != 0)) enterGroup(editor_.selectedGroup(g));
        if (ImGui::MenuItem("Exit Group", "Tab", false, !groupPath_.empty())) exitGroup();
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", "Ctrl+J") && editor_.frameSelection(g)) markChanged(false);
        if (ImGui::MenuItem("Remove from Frame", "Alt+P") && editor_.moveSelectionToFrame(g, 0)) markChanged(false);
        if (ImGui::MenuItem("Arrange Nodes", "Shift+P") && editor_.arrange(g)) markChanged(false);
        ImGui::Separator();
        if (ImGui::MenuItem("Preferences...")) showPreferences_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Original", nullptr, &showOriginal_);
        ImGui::MenuItem("Result", nullptr, &showResult_);
        ImGui::MenuItem("Node Editor", nullptr, &showEditor_);
        ImGui::MenuItem("Inspector", nullptr, &showInspector_);
        ImGui::MenuItem("Library", nullptr, &showLibrary_, library_.active());
        ImGui::MenuItem("Library Grid", "G", &library_.grid, library_.active());
        ImGui::MenuItem("Snapshots", nullptr, &showSnapshots_);
        // Lightroom's View > Loupe Overlay.
        if (ImGui::BeginMenu("Loupe Overlay")) {
            ImGui::MenuItem("Grid", nullptr, &loupe_.grid);
            ImGui::MenuItem("Guides", nullptr, &loupe_.guides);
            ImGui::SetNextItemWidth(160);
            ImGui::SliderFloat("Grid Size", &loupe_.gridSize, 8.0f, 400.0f, "%.0f px", ImGuiSliderFlags_Logarithmic);
            if (ImGui::MenuItem("Center Guides", nullptr, false, loupe_.guides)) loupe_.guideX = loupe_.guideY = 0.5f;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Crop Guide Overlay")) {
            for (int g = 0; g < NodeOverlay::kCropGuideCount; ++g)
                if (ImGui::MenuItem(NodeOverlay::cropGuideName(g), g == overlay_.cropGuide ? "O" : nullptr, g == overlay_.cropGuide))
                    overlay_.cropGuide = g;
            ImGui::Separator();
            if (ImGui::MenuItem("Cycle Orientation", "Shift+O")) overlay_.cycleCropGuide(true);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("New Viewer")) {
            NodePath pin;
            if (selected_) {
                pin = groupPath_;
                pin.push_back(selected_);
            }
            openViewer(std::move(pin));
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Layout")) {
            for (int i = 0; i < kLayouts; ++i)
                if (ImGui::MenuItem(kLayoutNames[i], nullptr, layoutPreset_ == i)) {
                    layoutPreset_ = i;
                    resetLayout_ = true;
                    savePreferences();
                }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset Layout")) resetLayout_ = true;
        if (ImGui::MenuItem("Inspector Overlay", nullptr, inspectorOverlay_)) {
            inspectorOverlay_ = !inspectorOverlay_;
            resetLayout_ = true;
            savePreferences();
        }
        ImGui::Separator();
        ImGui::MenuItem("Node Timings", nullptr, &editor_.showTimings);
        if (ImGui::BeginMenu("Compositor")) {
            if (drawCompositorSettings()) {
                savePreferences();
                evalDirty_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Frame All Nodes", "Home")) editor_.frameAll();
        if (ImGui::MenuItem("Find Node...", "Ctrl+F")) editor_.openFind();
        if (ImGui::MenuItem("Reset Image Zoom", "double-click image")) view_.reset();
        if (ImGui::MenuItem("Clear Node Preview", nullptr, false, !previewPath_.empty())) {
            previewPath_.clear();
            evalDirty_ = true;
        }
        ImGui::EndMenu();
    }
    drawColorMenu();
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("NodeLab %s - node-based image manipulation", versionString().c_str());
        ImGui::TextDisabled("%s", gpu::available() ? ("GPU: " + gpu::description()).c_str() : gpuError_.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Guide", "F1")) openGuide();
        if (const Node* n = currentGraph().find(selected_))
            if (ImGui::MenuItem(("Guide: " + n->info().displayName).c_str())) openGuide(n->info().displayName);
        ImGui::Separator();
        ImGui::TextUnformatted("Right-click canvas or Shift+A: add node   Drag pin to empty space: add connected node");
        ImGui::TextUnformatted("Drag empty space: pan   Wheel: zoom   Shift+drag: box select   Home / . : frame all / selected");
        ImGui::TextUnformatted("Ctrl+click node: preview (Ctrl+Shift+click: next output)   Del / X: delete and reconnect");
        ImGui::TextUnformatted("Ctrl+C / Ctrl+V   Ctrl+D / Shift+D: duplicate (and move)   G: move   H: collapse   M: mute");
        ImGui::TextUnformatted("F: make links   L / Shift+L: select upstream / downstream   F2: rename   Alt+drag: pull out");
        ImGui::TextUnformatted("Ctrl+right-drag: cut wires   Shift+right-drag: add reroutes");
        ImGui::TextUnformatted("Ctrl+G: group   Ctrl+Alt+G: ungroup   Tab: enter / exit group   Ctrl+J: frame   Alt+P: remove from frame");
        ImGui::TextUnformatted("Panels: drag a tab to dock it anywhere or pull it out into its own window");
        ImGui::Separator();
        ImGui::TextUnformatted("Wires: amber = Image, gray = Channel, blue = Number");
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) requestAction(Pending::New);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) requestAction(Pending::Open);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O)) requestAction(Pending::OpenFolder);
    if (library_.active() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) copyEdit();
    if (library_.active() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V)) pasteEdit();
    // Lightroom's Create Virtual Copy (Ctrl+').
    if (library_.active() && libraryPhotoOpen() && !io.WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Apostrophe))
        createVirtualCopy(library_.current());
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) saveProject(false);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) saveProject(true);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_I))
        if (auto p = openFileDialog("Import image", kImageFileFilter)) importImage(*p);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E)) openExportWindow();
    // G: the Library's grid, as in Lightroom (the Node Editor keeps G for grabbing nodes).
    if (library_.active() && !library_.grid && !editorFocused_ && !eyedropper().active() && !io.KeyCtrl && !io.KeyAlt &&
        !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G, false) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        library_.grid = true;
    if (eyedropper().active() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) eyedropper().cancel();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        // F1 opens the guide at the selected node's entry, like context help.
        const Node* n = currentGraph().find(selected_);
        openGuide(n ? n->info().displayName : std::string());
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        redo();
}

void App::handleDrops() {
    auto drops = std::move(drops_);
    drops_.clear();
    for (const auto& p : drops) {
        // With the Batch tab open, dropped images and folders become batch sources instead.
        if (showExport_ && exportTab_ == 1 && !exporter_.busy()) {
            std::vector<std::string> found;
            std::error_code ec;
            if (std::filesystem::is_directory(u8ToPath(p), ec)) {
                for (const auto& e : std::filesystem::directory_iterator(u8ToPath(p), ec))
                    if (e.is_regular_file() && isImageFile(e.path())) found.push_back(pathToU8(e.path()));
                std::sort(found.begin(), found.end());
            } else if (isImageFile(u8ToPath(p))) {
                found.push_back(p);
            }
            for (std::string& f : found)
                if (std::find(batchSources_.begin(), batchSources_.end(), f) == batchSources_.end())
                    batchSources_.push_back(std::move(f));
            if (!found.empty()) continue;
        }
        std::error_code ec;
        if (std::filesystem::is_directory(u8ToPath(p), ec)) {
            if (!modified_ || libraryPhotoOpen()) {
                saveLibraryPhoto();
                openFolder(p);
            } else {
                status_ = "Save or discard changes before opening a dropped folder";
            }
            continue;
        }
        std::string ext = pathToU8(u8ToPath(p).extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".nlproj") {
            if (!modified_) openProject(p);
            else status_ = "Save or discard changes before opening a dropped project";
        } else {
            importImage(p);
        }
    }
}

// ---------------------------------------------------------------- groups

Graph& App::currentGraph() {
    if (Graph* g = resolveGroupPath(graph_, groupPath_)) return *g;
    groupPath_.clear();
    return graph_;
}

GroupNode* App::currentGroupOwner() {
    if (groupPath_.empty()) return nullptr;
    Graph* parent = resolveGroupPath(graph_, std::vector<int>(groupPath_.begin(), groupPath_.end() - 1));
    return parent ? dynamic_cast<GroupNode*>(parent->find(groupPath_.back())) : nullptr;
}

void App::setGroupPath(std::vector<int> path) {
    groupViews_[groupPath_] = editor_.viewState();
    const int leaving = groupPath_.size() > path.size() ? groupPath_[path.size()] : 0;
    groupPath_ = std::move(path);
    auto it = groupViews_.find(groupPath_);
    eyedropper().cancel();  // its node id may mean something else now
    editor_.onGraphReplaced(it == groupViews_.end());
    if (it != groupViews_.end()) editor_.setViewState(it->second);
    if (leaving) editor_.select(leaving);  // going up: highlight the group we came out of
    selected_ = 0;
}

void App::enterGroup(int nodeId) {
    if (!dynamic_cast<GroupNode*>(currentGraph().find(nodeId))) return;
    auto p = groupPath_;
    p.push_back(nodeId);
    setGroupPath(p);
}

void App::exitGroup() {
    if (groupPath_.empty()) return;
    setGroupPath(std::vector<int>(groupPath_.begin(), groupPath_.end() - 1));
}

void App::finishPick(const PickRequest& pick) {
    Eyedropper& e = eyedropper();
    if (pick.cancelled) {
        e.cancel();
        return;
    }
    if (!pick.done) return;
    Node* n = currentGraph().find(e.node);
    if (n && e.param >= 0 && e.param < int(n->info().params.size())) {
        const ParamDesc& d = n->info().params[e.param];
        float c[3];
        for (int k = 0; k < 3; ++k) c[k] = std::clamp(pick.rgb[k], d.hardMin, d.hardMax);
        n->params[e.param] = nlohmann::json::array({c[0], c[1], c[2]});
        markChanged(true);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Picked %.3f %.3f %.3f for %s", c[0], c[1], c[2], d.name.c_str());
        status_ = buf;
    }
    e.cancel();
}

void App::openViewer(NodePath pin) {
    auto v = std::make_unique<Viewer>();
    v->id = nextViewerId_++;
    v->pin = std::move(pin);
    viewers_.push_back(std::move(v));
    evalDirty_ = true;
}

std::string App::pathLabel(const NodePath& p) {
    std::string label;
    Graph* g = &graph_;
    for (size_t i = 0; i < p.size() && g; ++i) {
        Node* n = g->find(p[i]);
        if (!n) return "(missing)";
        if (!label.empty()) label += " > ";
        label += n->title();
        auto* grp = dynamic_cast<GroupNode*>(n);
        g = grp ? &grp->inner() : nullptr;
    }
    return label;
}

// ---------------------------------------------------------------- undo / redo (whole-graph snapshots)

void App::resetHistory() {
    undo_.clear();
    redo_.clear();
    committed_ = graph_.toJson();
    historyDirty_ = false;
}

bool App::commitHistory() {
    nlohmann::json cur = graph_.toJson();
    if (cur == committed_) return false;
    undo_.push_back(std::move(committed_));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    committed_ = std::move(cur);
    redo_.clear();
    return true;
}

bool App::canUndo() const { return !undo_.empty() || historyDirty_; }

void App::restoreSnapshot(const nlohmann::json& j) {
    try {
        graph_.fromJson(j);
    } catch (const std::exception& e) {
        status_ = std::string("Undo failed: ") + e.what();
        return;
    }
    // Stay inside the current group if it still exists, otherwise back out to where it does.
    while (!groupPath_.empty() && !resolveGroupPath(graph_, groupPath_)) groupPath_.pop_back();
    eyedropper().cancel();  // its node id may mean something else now
    editor_.onGraphReplaced(false);
    if (!pathValid(previewPath_)) previewPath_.clear();
    selected_ = 0;
    modified_ = true;
    evalDirty_ = true;
    historyDirty_ = false;
}

void App::undo() {
    commitHistory();  // include any not-yet-snapshotted change so it is what gets undone
    if (undo_.empty()) return;
    redo_.push_back(std::move(committed_));
    committed_ = std::move(undo_.back());
    undo_.pop_back();
    restoreSnapshot(committed_);
    status_ = "Undo";
}

void App::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(committed_));
    committed_ = std::move(redo_.back());
    redo_.pop_back();
    restoreSnapshot(committed_);
    status_ = "Redo";
}

// ---------------------------------------------------------------- unsaved-changes flow

void App::requestAction(Pending action) {
    // A library photo saves itself, as in Lightroom: no question when leaving it.
    if (modified_ && libraryPhotoOpen()) saveLibraryPhoto();
    if (modified_) {
        pending_ = action;
        openUnsavedModal_ = true;
    } else {
        performAction(action);
    }
}

void App::performAction(Pending action) {
    switch (action) {
        case Pending::New: newProject(); break;
        case Pending::Open:
            if (auto p = openFileDialog("Open project", kProjectFilter)) openProject(*p);
            break;
        case Pending::OpenFolder:
            if (auto d = folderDialog("Open folder")) openFolder(*d);
            break;
        case Pending::OpenPhoto: loadLibraryPhoto(pendingPhoto_); break;
        case Pending::Quit: quit_ = true; break;
        case Pending::None: break;
    }
}

void App::drawUnsavedModal() {
    if (openUnsavedModal_) {
        ImGui::OpenPopup("Unsaved changes");
        openUnsavedModal_ = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("This project has unsaved changes.");
    ImGui::Spacing();
    if (ImGui::Button("Save")) {
        ImGui::CloseCurrentPopup();
        if (saveProject(false)) performAction(pending_);
        pending_ = Pending::None;
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        ImGui::CloseCurrentPopup();
        modified_ = false;
        performAction(pending_);
        pending_ = Pending::None;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        pending_ = Pending::None;
    }
    ImGui::EndPopup();
}

void App::drawConvertModal() {
    // Opened here, not inside the Color menu, so the ID stack matches BeginPopupModal's.
    if (convertPrompt_) {
        ImGui::OpenPopup("Convert to Scene-Linear");
        convertPrompt_ = false;
    }
    if (!ImGui::BeginPopupModal("Convert to Scene-Linear", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Images will load as linear light and nodes will work on linear values,");
    ImGui::TextUnformatted("with the view transform applied only for display and export.");
    ImGui::TextUnformatted("Nothing is added to the graph, so the result will look different:");
    ImGui::TextUnformatted("curves, levels and blends tuned on sRGB values may need adjusting.");
    ImGui::TextUnformatted("Ctrl+Z undoes the conversion.");
    ImGui::Spacing();
    if (ImGui::Button("Convert")) {
        ImGui::CloseCurrentPopup();
        graph_.colorManagement.linear = true;
        markChanged(true);
        status_ = "Converted to scene-linear";
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- project lifecycle

void App::newProject() {
    AutoMaskNode::cancelRuns();
    graph_.clear();
    graph_.colorManagement = newProjectColor();
    Node* in = graph_.addNode(ImageInputNode::staticInfo().type, 40, 80);
    Node* out = graph_.addNode(OutputNode::staticInfo().type, 460, 80);
    graph_.connect(in->id, 0, out->id, 0);
    groupPath_.clear();
    groupViews_.clear();
    eyedropper().cancel();  // its node id may mean something else now
    editor_.onGraphReplaced(true);
    editor_.select(in->id);
    resetHistory();
    projectPath_.clear();
    previewPath_.clear();
    snapshots_.clear();
    selected_ = 0;
    view_.reset();
    modified_ = false;
    evalDirty_ = true;
    status_ = "New project";
    library_.setCurrentProject(projectPath_);
}

bool App::openProject(const std::string& path) {
    AutoMaskNode::cancelRuns();  // the last project's AI masks are no use now
    Graph g;
    nlohmann::json ui;
    std::string err;
    if (!loadProject(path, g, ui, err)) {
        status_ = "Open failed: " + err;
        return false;
    }
    graph_ = std::move(g);
    groupPath_.clear();
    groupViews_.clear();
    eyedropper().cancel();  // its node id may mean something else now
    editor_.onGraphReplaced(true);
    projectPath_ = path;
    selected_ = 0;
    applyUiState(ui);
    resetHistory();
    modified_ = false;
    evalDirty_ = true;
    status_ = "Opened " + pathToU8(u8ToPath(path).filename());
    library_.setCurrentProject(projectPath_);
    return true;
}

bool App::saveProject(bool saveAs) {
    if (!saveAs && libraryPhotoOpen()) return saveLibraryPhoto(true);
    std::string path = projectPath_;
    if (saveAs || path.empty()) {
        auto p = saveFileDialog("Save project", kProjectFilter, "nlproj");
        if (!p) return false;
        path = *p;
    }
    std::string err;
    if (!::saveProject(path, graph_, uiState(), err)) {
        status_ = "Save failed: " + err;
        return false;
    }
    projectPath_ = path;
    modified_ = false;
    status_ = "Saved " + pathToU8(u8ToPath(path).filename());
    library_.setCurrentProject(projectPath_);
    return true;
}

// ---------------------------------------------------------------- library

void App::openFolder(const std::string& dirU8) {
    if (!library_.open(dirU8)) {
        status_ = "No photos in " + dirU8;
        return;
    }
    showLibrary_ = true;
    library_.setCurrentProject(projectPath_);
    status_ = "Library: " + std::to_string(library_.size()) + " photos in " + pathToU8(u8ToPath(dirU8).filename());
    // Start on the first photo, unless a project with unsaved changes is open.
    if (!modified_ && library_.current() < 0) loadLibraryPhoto(0);
}

bool App::libraryPhotoOpen() const {
    const int i = library_.current();
    return i >= 0 && projectPath_ == library_.sidecar(i);
}

void App::openLibraryPhoto(int index) {
    if (index < 0 || index >= library_.size() || (libraryPhotoOpen() && index == library_.current())) return;
    pendingPhoto_ = index;
    requestAction(Pending::OpenPhoto);
}

void App::loadLibraryPhoto(int index) {
    if (index < 0 || index >= library_.size()) return;
    const std::string photo = library_.photo(index);
    AutoMaskNode::cancelRuns();  // an AI mask of the last photo is no use now
    // Decoded images are kept per file; a browsing session would otherwise keep every photo's.
    // The last few photos' previews stay (about 18 MB each), so stepping back is instant, and
    // the next one in the direction of browsing is decoded ahead (tickPrefetch).
    const int from = library_.current();
    std::erase(recentPhotos_, photo);
    recentPhotos_.insert(recentPhotos_.begin(), photo);
    if (recentPhotos_.size() > kKeptPhotos) recentPhotos_.resize(kKeptPhotos);
    const int next = index + (from >= 0 && from > index ? -1 : 1);
    prefetchPhoto_ = next >= 0 && next < library_.size() ? library_.photo(next) : std::string();
    std::vector<std::string> keep = recentPhotos_;
    if (!prefetchPhoto_.empty()) keep.push_back(prefetchPhoto_);
    cache_.retain(keep);
    const int copy = library_.copyOf(index);
    if (library::hasSidecar(photo, copy)) {
        if (!openProject(library::sidecarPath(photo, copy))) return;
    } else {
        newProject();
        library::defaultGraph(graph_, photo);
        editor_.onGraphReplaced(true);
        resetHistory();
        projectPath_ = library::sidecarPath(photo, copy);
        library_.setCurrentProject(projectPath_);
    }
    status_ = pathToU8(u8ToPath(photo).filename()) + (copy ? " (Copy " + std::to_string(copy) + ")" : "");
}

// Decodes the next photo's preview while the user looks at this one, once the evaluator is idle
// (so it never slows the current photo down), with the decode options its edit will ask for.
void App::tickPrefetch() {
    if (prefetch_.joinable() && prefetchDone_) prefetch_.join();
    if (prefetchPhoto_.empty() || prefetch_.joinable() || eval_->busy() || evalDirty_) return;
    std::string err;
    const nlohmann::json gj = library::graphFor(prefetchPhoto_, err);
    const std::string photo = std::move(prefetchPhoto_);
    prefetchPhoto_.clear();
    if (gj.is_null()) return;
    Graph g;
    try {
        g.fromJson(gj);
    } catch (const std::exception&) {
        return;
    }
    const int inId = g.firstOfType(ImageInputNode::staticInfo().type);
    if (!inId) return;
    const auto& input = static_cast<const ImageInputNode&>(*g.find(inId));
    if (input.paramS(0) != photo) return;  // an edit of another file: nothing to guess
    const ImageCache::Decode decode = input.decode(g.colorManagement.linear);
    const int edge = proxyEdge_;
    prefetchDone_ = false;
    prefetch_ = std::thread([this, photo, decode, edge] {
        parallel::lowerThreadPriority();
        try {
            cache_.get(photo, true, nullptr, decode, edge);
        } catch (const std::exception&) {
            // Only a head start: the evaluation reports any error when the photo opens.
        }
        prefetchDone_ = true;
    });
}

bool App::saveLibraryPhoto(bool force) {
    if (!libraryPhotoOpen() || (!modified_ && !force)) return true;
    const int i = library_.current();
    if (modified_) library_.meta(i).edited = true;
    std::string err;
    if (!::saveProject(projectPath_, graph_, uiState(), err)) {
        status_ = "Save failed: " + err;
        return false;
    }
    modified_ = false;
    library_.refresh(i, true);  // its thumbnail shows the edit
    status_ = "Saved " + pathToU8(u8ToPath(projectPath_).filename());
    return true;
}

void App::copyEdit() {
    if (!libraryPhotoOpen()) return;
    copiedEdit_ = graph_.toJson();
    copiedFrom_ = library_.photo(library_.current());
    copiedSidecar_ = library_.sidecar(library_.current());
    status_ = "Copied the edit of " + pathToU8(u8ToPath(copiedFrom_).filename());
}

void App::pasteEdit() {
    if (copiedEdit_.is_null()) return;
    saveLibraryPhoto();
    int pasted = 0, failed = 0;
    std::string err;
    for (int i : library_.selection()) {
        const std::string& photo = library_.photo(i);
        if (library_.sidecar(i) == copiedSidecar_) continue;  // its own edit
        if (i == library_.current() && libraryPhotoOpen()) {
            // The photo being edited takes it in memory, so Ctrl+Z undoes the paste.
            Graph g;
            try {
                g.fromJson(copiedEdit_);
            } catch (const std::exception& e) {
                err = e.what();
                ++failed;
                continue;
            }
            library::retargetEdit(g, copiedFrom_, photo);
            graph_ = std::move(g);
            groupPath_.clear();
            editor_.onGraphReplaced(false);
            selected_ = 0;
            markChanged(true);
            ++pasted;
        } else if (library::pasteEdit(copiedEdit_, copiedFrom_, photo, err, library_.copyOf(i))) {
            library_.refresh(i, true);
            ++pasted;
        } else {
            ++failed;
        }
    }
    status_ = "Pasted the edit onto " + std::to_string(pasted) + (pasted == 1 ? " photo" : " photos");
    if (failed) status_ += " (" + std::to_string(failed) + " failed: " + err + ")";
}

void App::exportSelected() {
    const std::vector<int> sel = library_.selection();
    if (sel.empty() || exporter_.busy()) return;
    if (!batchDir_[0]) {
        auto d = folderDialog("Export selected photos to");
        if (!d) return;
        std::snprintf(batchDir_, sizeof(batchDir_), "%s", d->c_str());
    }
    saveLibraryPhoto();
    exportSettings_.nameTemplate = nameTemplate_;
    std::vector<NameSource> names;
    for (int i : sel) names.push_back({library_.photo(i), 0, library_.copyOf(i)});
    const std::vector<std::string> outputs = batchOutputPaths(names, batchDir_, exportSettings_);
    std::vector<ExportItem> items;
    std::string err;
    for (size_t k = 0; k < sel.size(); ++k) {
        const int i = sel[k];
        ExportItem it;
        it.source = library_.photo(i);
        it.output = outputs[k];
        it.graph = i == library_.current() && libraryPhotoOpen() ? graph_.toJson() : library::graphFor(it.source, err, library_.copyOf(i));
        if (it.graph.is_null()) {
            status_ = "Can't export " + it.source + ": " + err;
            continue;
        }
        items.push_back(std::move(it));
    }
    if (items.empty()) return;
    exportLog_.clear();
    exporter_.start(nullptr, std::move(items), 0, exportSettings_, gpuDevice_ && gpu::available());
    showExport_ = true;
}

void App::drawLibraryWindow() {
    // A layout saved before the Library existed has no place for it: rebuild the default one
    // (once), rather than leaving the filmstrip floating over the panels.
    if (!libraryLayoutChecked_) {
        libraryLayoutChecked_ = true;
        if (!ImGui::FindWindowSettingsByID(ImHashStr("###Library"))) resetLayout_ = true;
    }
    if (ImGui::Begin("Library###Library", &showLibrary_, kCanvasFlags)) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool keys = !io.WantTextInput && !editorFocused_ && !eyedropper().active() &&
                          !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        library_.canPaste = !copiedEdit_.is_null();
        handleLibraryActions(library_.draw(keys && !library_.grid));
    }
    ImGui::End();
}

void App::createVirtualCopy(int i) {
    if (i < 0 || i >= library_.size()) return;
    // The copy starts from the edit as it is now, unsaved changes included.
    if (i == library_.current()) saveLibraryPhoto();
    std::string err;
    const int n = library::createVirtualCopy(library_.photo(i), library_.copyOf(i), err);
    if (n < 0) {
        status_ = "Could not create a virtual copy: " + err;
        return;
    }
    const int at = library_.insertCopy(i, n);
    library_.refresh(at, false);
    openLibraryPhoto(at);
    status_ = "Created Copy " + std::to_string(n) + " of " + pathToU8(u8ToPath(library_.photo(i)).filename());
}

void App::removeVirtualCopy(int i) {
    if (i < 0 || i >= library_.size() || library_.copyOf(i) <= 0) return;
    const std::string photo = library_.photo(i);
    const int copy = library_.copyOf(i);
    const bool open = i == library_.current() && libraryPhotoOpen();
    std::string err;
    if (!library::removeVirtualCopy(photo, copy, err)) {
        status_ = "Could not remove the virtual copy: " + err;
        return;
    }
    library_.removeEntry(i);
    if (open) {
        // Its edit is gone: show the photo itself, without asking to save the removed copy.
        modified_ = false;
        for (int k = 0; k < library_.size(); ++k)
            if (library_.photo(k) == photo && library_.copyOf(k) == 0) loadLibraryPhoto(k);
    }
    status_ = "Removed Copy " + std::to_string(copy) + " of " + pathToU8(u8ToPath(photo).filename()) + " (to the Recycle Bin)";
}

void App::handleLibraryActions(const LibraryPanel::Actions& a) {
    if (a.createCopy >= 0) createVirtualCopy(a.createCopy);
    if (a.removeCopy >= 0) {
        removeCopyIndex_ = a.removeCopy;
        ImGui::OpenPopup("Remove Virtual Copy?");
    }
    if (ImGui::BeginPopupModal("Remove Virtual Copy?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool valid = removeCopyIndex_ >= 0 && removeCopyIndex_ < library_.size() && library_.copyOf(removeCopyIndex_) > 0;
        if (valid)
            ImGui::Text("Remove Copy %d of %s?\nIts edit goes to the Recycle Bin; the photo stays.", library_.copyOf(removeCopyIndex_),
                        pathToU8(u8ToPath(library_.photo(removeCopyIndex_)).filename()).c_str());
        if (ImGui::Button("Remove", ImVec2(120, 0)) && valid) {
            removeVirtualCopy(removeCopyIndex_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || !valid) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (a.copy) copyEdit();
    if (a.paste) pasteEdit();
    if (a.exportSelected) exportSelected();
    if (a.open >= 0) openLibraryPhoto(a.open);
}

void App::drawLibraryGrid() {
    // A plain window over the docked panels (not docked itself, so the layout stays as it is).
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    if (!gridShown_) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("Library Grid###LibraryGrid", nullptr, flags)) {
        const bool keys = !ImGui::GetIO().WantTextInput && !eyedropper().active() &&
                          ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                          !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        library_.canPaste = !copiedEdit_.is_null();
        handleLibraryActions(library_.drawGrid(keys));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void App::applyRawLook() {
    if (!takeRawChosen()) return;
    // Only a fresh scene-linear project whose one image is this RAW: an edited look stays put.
    ColorManagement& cm = graph_.colorManagement;
    int images = 0;
    for (const auto& [id, n] : graph_.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type && !n->paramS(0).empty()) ++images;
    if (!cm.linear || images != 1 || !(cm == newProjectColor()) || cm.view == ColorManagement::AgX) return;
    // AgX rolls off the highlights a RAW keeps above 1, which Standard would clip.
    cm.view = ColorManagement::AgX;
    status_ = "View transform set to AgX for the RAW";
    markChanged(false);
}

void App::importImage(const std::string& path) {
    std::string err;
    // Decode as the new Image Input will (its default params), so the cache entry is reused.
    // (A bare node has no params until initParams.)
    ImageInputNode probe;
    probe.initParams();
    if (!cache_.get(path, true, &err, probe.decode(graph_.colorManagement.linear), proxyEdge_)) {
        status_ = "Import failed: " + err;
        return;
    }
    // Reuse an empty Image Input if there is one, otherwise add a new node.
    Graph& g = currentGraph();
    Node* target = nullptr;
    for (const auto& [id, n] : g.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type && n->paramS(0).empty()) {
            target = n.get();
            break;
        }
    if (!target) {
        target = g.addNode(ImageInputNode::staticInfo().type);
        editor_.placeAtScreen(*target, editor_.canvasCenter());
    }
    chooseImageFile(*target, 0, path);
    editor_.select(target->id);
    status_ = "Imported " + pathToU8(u8ToPath(path).filename());
    markChanged(true);
}

void App::openExportWindow() {
    showExport_ = true;
    focusExport_ = true;
    // Suggest a file next to the project the first time.
    if (!exportPath_[0]) {
        auto base = projectPath_.empty() ? std::filesystem::path("export") : u8ToPath(projectPath_).replace_extension();
        std::snprintf(exportPath_, sizeof(exportPath_), "%s", pathToU8(base.string() + std::string(exportSettings_.extension())).c_str());
    }
}

void App::startExport(std::vector<ExportItem> items, int inputNode) {
    if (exporter_.busy() || items.empty()) return;
    exportSettings_.nameTemplate = nameTemplate_;
    exportLog_.clear();
    // The root graph, whatever group is open: exports always render the whole project.
    exporter_.start(graph_.toJson(), std::move(items), inputNode, exportSettings_, gpuDevice_ && gpu::available());
}

void App::pollExport() {
    for (std::string& line : exporter_.takeLog()) {
        status_ = line;
        exportLog_.push_back(std::move(line));
    }
}

// As Blender's Render Properties > Performance > Compositor (View > Compositor, Preferences).
bool App::drawCompositorSettings() {
    bool changed = false;
    ImGui::TextDisabled("Device");
    if (ImGui::RadioButton("CPU", !gpuDevice_ || !gpu::available())) changed = gpuDevice_, gpuDevice_ = false;
    ImGui::BeginDisabled(!gpu::available());
    if (ImGui::RadioButton("GPU", gpuDevice_ && gpu::available())) changed = !gpuDevice_, gpuDevice_ = true;
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", gpu::available() ? gpu::description().c_str() : gpuError_.c_str());
    ImGui::Separator();
    ImGui::TextDisabled("Precision");
    ImGui::BeginDisabled(!gpuDevice_ || !gpu::available());
    if (ImGui::RadioButton("Auto", !gpuFull_)) changed |= gpuFull_, gpuFull_ = false;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Half floats on the GPU: half the memory traffic. Exports always use full precision.");
    if (ImGui::RadioButton("Full", gpuFull_)) changed |= !gpuFull_, gpuFull_ = true;
    ImGui::EndDisabled();
    return changed;
}

// Select Subject and Select Sky: where their models run, and the downloaded models.
bool App::drawAiSettings() {
    bool changed = false;
    ImGui::TextDisabled("Device");
    if (ImGui::RadioButton("CPU##ai", !ml::useGpu())) changed = ml::useGpu(), ml::setUseGpu(false);
    if (ImGui::RadioButton("GPU (DirectML)##ai", ml::useGpu())) changed = !ml::useGpu(), ml::setUseGpu(true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Much faster on a dedicated graphics card. Integrated GPUs can take minutes to load a\n"
                          "model or run out of memory; a model that fails there runs on the CPU instead.");
    ImGui::TextDisabled("Models (in %s)", ml::folder().c_str());
    const bool installing = ml::installState().running;
    for (const ml::ModelSpec& m : ml::catalogue()) {
        ImGui::PushID(m.id);
        const bool have = ml::installed(m.id);
        uint64_t size = 0;
        for (const ml::FileSpec& f : m.files) size += f.size;
        ImGui::Text("%s", m.title);
        ImGui::SameLine(ImGui::GetFontSize() * 9);
        ImGui::TextDisabled("%s", have ? "installed" : "not downloaded");
        if (have) {
            ImGui::SameLine(ImGui::GetFontSize() * 16);
            ImGui::BeginDisabled(installing);
            char label[48];
            std::snprintf(label, sizeof label, "Remove (%.0f MB)", size / 1e6);
            if (ImGui::SmallButton(label)) ml::remove(m.id);
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("Select Subject and Select Sky offer the download when they need it.");
    return changed;
}

// Edit > Preferences, as Blender's: sections on the left, their settings on the right. Every
// change is kept in preferences.json straight away.
void App::drawPreferencesWindow() {
    if (!showPreferences_) return;
    const float fs = ImGui::GetFontSize();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(fs * 38, fs * 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Preferences###Preferences", &showPreferences_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    static constexpr const char* kSections[] = {"Interface", "Themes", "Viewer", "Compositor", "New Projects",
                                                "Save & Load"};
    ImGui::BeginChild("##sections", ImVec2(fs * 7.5f, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < int(std::size(kSections)); ++i)
        if (ImGui::Selectable(kSections[i], prefsSection_ == i)) prefsSection_ = i;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##settings");
    bool changed = false;
    theme::Theme& th = theme::current();
    // Editing a built-in theme makes it "Custom" (the preset itself stays as it was).
    auto themeEdited = [&] {
        for (const theme::Theme& p : theme::presets())
            if (p.name == th.name) th.name = "Custom";
        theme::apply();
        changed = true;
    };
    auto colorRow = [&](int c) {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(th.col[c]);
        if (ImGui::ColorEdit4(theme::colName(c), &v.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf)) {
            th.col[c] = ImGui::ColorConvertFloat4ToU32(v);
            themeEdited();
        }
    };
    auto colorTable = [&](const char* id, int from, int to) {
        if (!ImGui::BeginTable(id, 2)) return;
        for (int c = from; c < to; ++c) {
            ImGui::TableNextColumn();
            colorRow(c);
        }
        ImGui::EndTable();
    };
    const float combo = fs * 12;
    switch (prefsSection_) {
        case 0: {  // Interface
            ImGui::SeparatorText("Layout");
            ImGui::SetNextItemWidth(combo);
            if (ImGui::Combo("Preset", &layoutPreset_, kLayoutNames, kLayouts)) {
                resetLayout_ = true;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Layout")) resetLayout_ = true;
            ImGui::TextDisabled("Default: the images either side of the graph.\n"
                                "Compositing: the result over a wide graph.\n"
                                "Photo: the photo in the middle, its settings on the right.\n"
                                "Side by Side: before and after over the graph.\n"
                                "Node Focus: a big graph, the images on the right.");
            ImGui::SeparatorText("Inspector");
            int mode = inspectorOverlay_ ? 0 : 1;
            bool modeChanged = ImGui::RadioButton("Overlay", &mode, 0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The selected node's settings float in the Node Editor's top-right corner.");
            ImGui::SameLine();
            modeChanged |= ImGui::RadioButton("Panel", &mode, 1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The Inspector is a panel of its own in the layout.");
            if (modeChanged) {
                inspectorOverlay_ = mode == 0;
                resetLayout_ = true;  // make room for the panel, or take it back
                changed = true;
            }
            ImGui::TextDisabled("Changing this rebuilds the layout.");
            ImGui::SeparatorText("Node Editor");
            changed |= ImGui::Checkbox("Node Timings", &editor_.showTimings);
            break;
        }
        case 1: {  // Themes
            ImGui::SetNextItemWidth(combo);
            if (ImGui::BeginCombo("Theme", th.name.c_str())) {
                for (const theme::Theme& p : theme::presets())
                    if (ImGui::Selectable(p.name.c_str(), p.name == th.name)) {
                        th = p;
                        theme::apply();
                        changed = true;
                    }
                if (!customThemes_.empty()) ImGui::Separator();
                for (size_t i = 0; i < customThemes_.size(); ++i) {
                    ImGui::PushID(int(i));
                    if (ImGui::Selectable(customThemes_[i].name.c_str(), customThemes_[i].name == th.name)) {
                        th = customThemes_[i];
                        theme::apply();
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            auto custom = std::find_if(customThemes_.begin(), customThemes_.end(),
                                       [&](const theme::Theme& c) { return c.name == th.name; });
            const bool isCustom = custom != customThemes_.end();
            ImGui::SameLine();
            ImGui::BeginDisabled(!isCustom);
            if (ImGui::Button("Delete") && isCustom) {
                customThemes_.erase(custom);
                th.name = "Custom";
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(combo);
            ImGui::InputTextWithHint("##themeName", "New theme name", themeName_, sizeof(themeName_));
            ImGui::SameLine();
            // Save under the typed name, or over the current custom theme.
            std::string name = themeName_;
            if (name.empty() && isCustom) name = th.name;
            bool builtIn = false;
            for (const theme::Theme& p : theme::presets()) builtIn |= p.name == name;
            ImGui::BeginDisabled(name.empty() || builtIn);
            if (ImGui::Button("Save Theme")) {
                th.name = name;
                auto same = std::find_if(customThemes_.begin(), customThemes_.end(),
                                         [&](const theme::Theme& c) { return c.name == name; });
                if (same != customThemes_.end()) *same = th;
                else customThemes_.push_back(th);
                themeName_[0] = 0;
                changed = true;
            }
            ImGui::EndDisabled();

            ImGui::SeparatorText("Interface");
            int base = th.light ? 1 : 0;
            bool baseChanged = ImGui::RadioButton("Dark", &base, 0);
            ImGui::SameLine();
            baseChanged |= ImGui::RadioButton("Light", &base, 1);
            if (baseChanged) {
                th.light = base == 1;
                themeEdited();
            }
            if (ImGui::BeginTable("##ui", 2)) {
                for (int k = 0; k < theme::kUiKeys; ++k) {
                    ImGui::TableNextColumn();
                    ImVec4 v = theme::uiValue(th, k);
                    if (ImGui::ColorEdit3(theme::uiKeyName(k), &v.x, ImGuiColorEditFlags_NoInputs)) {
                        th.uiSet[k] = true;
                        th.ui[k] = v;
                        themeEdited();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::SeparatorText("Node Editor");
            colorTable("##editor", theme::Canvas, theme::WireImage);
            ImGui::SeparatorText("Sockets and Wires");
            colorTable("##wires", theme::WireImage, theme::CatInputOutput);
            ImGui::SeparatorText("Node Headers");
            colorTable("##cats", theme::CatInputOutput, theme::ImageBackground);
            ImGui::SeparatorText("Viewer");
            colorTable("##viewer", theme::ImageBackground, theme::kCols);
            break;
        }
        case 2:  // Viewer
            ImGui::SeparatorText("Theme");
            colorRow(theme::ImageBackground);
            ImGui::TextDisabled("The view transform, histogram and clipping warnings are saved\n"
                                "with each project (Color menu, and the Result viewer's toolbar).");
            break;
        case 3:  // Compositor
            ImGui::SeparatorText("Performance");
            if (drawCompositorSettings()) {
                evalDirty_ = true;
                changed = true;
            }
            ImGui::SeparatorText("AI Masks");
            if (drawAiSettings()) {
                evalDirty_ = true;
                changed = true;
            }
            break;
        case 4: {  // New Projects
            ImGui::SeparatorText("Color Management");
            ImGui::TextDisabled("New projects and library photos without an edit start with this view.\n"
                                "RAW photos always start with AgX, which keeps their highlights.");
            int view = newView_ == ColorManagement::AgX ? 1 : 0;
            static constexpr const char* kViews[] = {"Standard", "AgX"};
            ImGui::SetNextItemWidth(combo);
            if (ImGui::Combo("View Transform", &view, kViews, 2)) {
                newView_ = view == 1 ? ColorManagement::AgX : ColorManagement::Standard;
                changed = true;
            }
            ImGui::BeginDisabled(newView_ != ColorManagement::AgX);
            ImGui::SetNextItemWidth(combo);
            changed |= ImGui::Combo("Look", &newLook_, colormgmt::kLookNames, 3);
            ImGui::EndDisabled();
            if (changed) library::setDefaultView(newView_, newView_ == ColorManagement::AgX ? newLook_ : 0);
            break;
        }
        case 5: {  // Save & Load
            ImGui::SeparatorText("Auto Save");
            changed |= ImGui::Checkbox("Auto Save", &autosave_);
            ImGui::BeginDisabled(!autosave_);
            ImGui::SetNextItemWidth(combo);
            changed |= ImGui::SliderInt("Timer (Minutes)", &autosaveMinutes_, 1, 60, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Unsaved changes are saved this long after the first one.\n"
                                "Only a project saved once (or a library photo's edit) is auto saved,\n"
                                "in place; an untitled project waits for its first Save.");
            break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
    if (changed) prefsDirty_ = true;
    if (prefsDirty_ && !ImGui::IsAnyItemActive()) {
        savePreferences();
        prefsDirty_ = false;
    }
}

// Lightroom's Preset list in the Export dialog: built-in presets, then the user's.
void App::exportPresetRow() {
    ExportSettings& es = exportSettings_;
    es.nameTemplate = nameTemplate_;
    std::string current = "Custom";
    for (const ExportPreset& p : builtInExportPresets())
        if (es.sameOutput(p.settings)) current = p.name;
    int userIndex = -1;
    for (int i = 0; i < int(exportPresets_.size()); ++i)
        if (es.sameOutput(exportPresets_[size_t(i)].settings)) current = exportPresets_[size_t(i)].name, userIndex = i;
    auto apply = [&](const ExportPreset& p) {
        const bool fileOutputs = es.fileOutputs;
        es = p.settings;
        es.fileOutputs = fileOutputs;
        std::snprintf(nameTemplate_, sizeof(nameTemplate_), "%s", es.nameTemplate.c_str());
        // The single export's file follows the preset's format.
        if (exportPath_[0]) {
            auto path = u8ToPath(exportPath_);
            path.replace_extension(es.extension());
            std::snprintf(exportPath_, sizeof(exportPath_), "%s", pathToU8(path).c_str());
        }
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    const float buttons = ImGui::CalcTextSize("Save...Delete").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::SetNextItemWidth(-buttons);
    if (ImGui::BeginCombo("##exportPreset", current.c_str())) {
        for (const ExportPreset& p : builtInExportPresets())
            if (ImGui::Selectable(p.name.c_str(), current == p.name && userIndex < 0)) apply(p);
        if (!exportPresets_.empty()) ImGui::SeparatorText("User Presets");
        for (int i = 0; i < int(exportPresets_.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(exportPresets_[size_t(i)].name.c_str(), userIndex == i)) apply(exportPresets_[size_t(i)]);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save...")) {
        std::snprintf(presetName_, sizeof(presetName_), "%s", userIndex >= 0 ? exportPresets_[size_t(userIndex)].name.c_str() : "");
        ImGui::OpenPopup("Save Export Preset");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save these settings as a preset (a preset of the same name is replaced)");
    ImGui::SameLine();
    ImGui::BeginDisabled(userIndex < 0);
    if (ImGui::Button("Delete")) {
        exportPresets_.erase(exportPresets_.begin() + userIndex);
        savePreferences();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Delete the selected user preset");
    if (ImGui::BeginPopup("Save Export Preset")) {
        ImGui::TextUnformatted("Preset name");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##presetName", presetName_, sizeof(presetName_), ImGuiInputTextFlags_EnterReturnsTrue);
        std::string name = presetName_;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        ImGui::BeginDisabled(name.empty());
        if ((ImGui::Button("Save") || enter) && !name.empty()) {
            ExportPreset p{name, es};
            auto same = std::find_if(exportPresets_.begin(), exportPresets_.end(), [&](const ExportPreset& q) { return q.name == name; });
            if (same != exportPresets_.end()) *same = std::move(p);
            else exportPresets_.push_back(std::move(p));
            savePreferences();
            status_ = "Saved the export preset " + name;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Lightroom's File Naming: a template with tokens, a menu to insert them and an example name.
void App::fileNaming(const std::string& exampleSource) {
    ImGui::TextUnformatted("File naming");
    const float insertW = ImGui::CalcTextSize("Insert").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-insertW - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##nameTemplate", nameTemplate_, sizeof(nameTemplate_));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A filename template: text, and tokens such as {name}, {seq:3} or {date}");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##insertToken", "Insert", ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < kNameTokenCount; ++i)
            if (ImGui::Selectable(kNameTokens[i].token)) {
                const size_t len = std::strlen(nameTemplate_), add = std::strlen(kNameTokens[i].token);
                if (len + add < sizeof(nameTemplate_)) std::memcpy(nameTemplate_ + len, kNameTokens[i].token, add + 1);
            } else if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", kNameTokens[i].help);
            }
        ImGui::EndCombo();
    }
    exportSettings_.nameTemplate = nameTemplate_;
    if (!exampleSource.empty()) {
        // Reading EXIF for every frame would be wasteful: keep the example until the inputs change.
        const std::string key = exampleSource + '\n' + nameTemplate_ + exportSettings_.extension();
        if (key != namingExampleKey_) {
            namingExampleKey_ = key;
            namingExample_ = pathToU8(u8ToPath(batchOutputPath(exampleSource, batchDir_, exportSettings_)).filename());
        }
        ImGui::TextDisabled("e.g. %s", namingExample_.c_str());
    }
}

void App::drawExportWindow() {
    if (!showExport_) return;
    ImGui::SetNextWindowSize(ImVec2(520, 660), ImGuiCond_FirstUseEver);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (focusExport_) ImGui::SetNextWindowFocus();
    focusExport_ = false;
    if (!ImGui::Begin("Export", &showExport_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    const bool busy = exporter_.busy();
    ExportSettings& es = exportSettings_;
    const float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
    auto pathField = [&](const char* id, char* buf, size_t size) {
        ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText(id, buf, size);
        ImGui::SameLine();
        ImGui::PushID(id);
        const bool clicked = ImGui::Button("Browse...");
        ImGui::PopID();
        return clicked;
    };
    auto withExt = [&](std::string path) {
        auto p = u8ToPath(path);
        p.replace_extension(es.extension());
        return pathToU8(p);
    };

    ImGui::BeginDisabled(busy);
    exportPresetRow();
    int tab = -1;
    if (ImGui::BeginTabBar("##exportTabs")) {
        if (ImGui::BeginTabItem("Single")) {
            tab = 0;
            ImGui::TextUnformatted("Renders the Output node at full resolution.");
            ImGui::TextUnformatted("File");
            if (pathField("##exportPath", exportPath_, sizeof(exportPath_)))
                if (auto p = saveFileDialog("Export result", kSaveImageFilter, es.extension() + 1)) {
                    es.format = int(formatFromPath(*p));
                    std::snprintf(exportPath_, sizeof(exportPath_), "%s", p->c_str());
                }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Batch")) {
            tab = 1;
            ImGui::TextWrapped("Runs every source image through this node tree and saves each result to the output folder.");
            // Which Image Input receives each source.
            std::vector<const Node*> inputs;
            for (const auto& [id, n] : graph_.nodes())
                if (n->info().type == ImageInputNode::staticInfo().type) inputs.push_back(n.get());
            if (!graph_.find(batchInput_) && !inputs.empty()) batchInput_ = inputs.front()->id;
            auto inputName = [](const Node* n) {
                std::string f = n->paramS(0).empty() ? "no file" : pathToU8(u8ToPath(n->paramS(0)).filename());
                return n->title() + " (" + f + ")";
            };
            const Node* cur = graph_.find(batchInput_);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##batchInput", cur ? inputName(cur).c_str() : "No Image Input node")) {
                for (const Node* n : inputs) {
                    ImGui::PushID(n->id);
                    if (ImGui::Selectable(inputName(n).c_str(), n->id == batchInput_)) batchInput_ = n->id;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::Text("Sources (%d)", int(batchSources_.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Add Files..."))
                for (std::string& f : openFilesDialog("Add source images", kImageFileFilter))
                    if (std::find(batchSources_.begin(), batchSources_.end(), f) == batchSources_.end())
                        batchSources_.push_back(std::move(f));
            ImGui::SameLine();
            if (ImGui::SmallButton("Add Folder..."))
                if (auto dir = folderDialog("Add every image in a folder")) {
                    std::vector<std::string> found;
                    std::error_code ec;
                    for (const auto& e : std::filesystem::directory_iterator(u8ToPath(*dir), ec))
                        if (e.is_regular_file() && isImageFile(e.path())) found.push_back(pathToU8(e.path()));
                    std::sort(found.begin(), found.end());
                    for (std::string& f : found)
                        if (std::find(batchSources_.begin(), batchSources_.end(), f) == batchSources_.end())
                            batchSources_.push_back(std::move(f));
                }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) batchSources_.clear();
            if (ImGui::BeginChild("##sources", ImVec2(0, 110), ImGuiChildFlags_Borders)) {
                int remove = -1;
                for (int i = 0; i < int(batchSources_.size()); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::SmallButton("x")) remove = i;
                    ImGui::SameLine();
                    ImGui::TextUnformatted(pathToU8(u8ToPath(batchSources_[i]).filename()).c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", batchSources_[i].c_str());
                    ImGui::PopID();
                }
                if (batchSources_.empty()) ImGui::TextDisabled("Add files, a folder, or drop images here");
                if (remove >= 0) batchSources_.erase(batchSources_.begin() + remove);
            }
            ImGui::EndChild();
            ImGui::TextUnformatted("Output folder");
            if (pathField("##batchDir", batchDir_, sizeof(batchDir_)))
                if (auto d = folderDialog("Output folder")) std::snprintf(batchDir_, sizeof(batchDir_), "%s", d->c_str());
            fileNaming(batchSources_.empty() ? std::string() : batchSources_[0]);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    exportTab_ = tab;

    ImGui::SeparatorText("Format");
    ImGui::SetNextItemWidth(160);
    // The path field follows the format, so what it shows is the file that gets written.
    if (ImGui::Combo("##format", &es.format, "PNG\0JPEG\0TIFF\0OpenEXR\0") && exportPath_[0])
        std::snprintf(exportPath_, sizeof(exportPath_), "%s", withExt(exportPath_).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PNG, JPEG and TIFF are display images (view transform applied, tagged sRGB).\n"
                          "OpenEXR keeps the scene-linear values, as Blender does.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (es.format == ExportSettings::JPEG) {
        ImGui::SliderInt("##quality", &es.jpegQuality, 1, 100, "Quality %d");
    } else {
        // Blender's Color Depth: 8/16 bits for PNG and TIFF, half or full float for OpenEXR.
        const bool exr = es.format == ExportSettings::EXR;
        int hi = formatDepth(FileFormat(es.format), es.depth) > (exr ? 16 : 8) ? 1 : 0;
        if (ImGui::Combo("##depth", &hi, exr ? "Float (Half)\0Float (Full)\0" : "8 bit\00016 bit\0"))
            es.depth = exr ? (hi ? 32 : 16) : (hi ? 16 : 8);
    }
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("##size", &es.sizeMode, "Original size\0Long edge\0Percent\0");
    if (es.sizeMode == ExportSettings::LongEdge) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("px", &es.longEdge, 64, 512);
        es.longEdge = std::clamp(es.longEdge, 16, 65536);
    } else if (es.sizeMode == ExportSettings::Percent) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##percent", &es.percent, 1, 100, "%d %%");
    }
    // Lightroom's Output Sharpening: after resizing, for where the image will be seen.
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("##sharpenFor", &es.sharpenFor, "No sharpening\0Sharpen for Screen\0Sharpen for Matte Paper\0Sharpen for Glossy Paper\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lightroom's Output Sharpening, applied after resizing.\n"
                          "Screen suits web and phone images; the paper options suit prints.");
    if (es.sharpenFor != ExportSettings::SharpenOff) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##sharpenAmount", &es.sharpenAmount, "Low\0Standard\0High\0");
    }
    if (tab == 0) ImGui::Checkbox("Also write File Output nodes", &es.fileOutputs);
    ImGui::EndDisabled();

    ImGui::Separator();
    if (busy) {
        const Exporter::Progress pr = exporter_.progress();
        // No per-node progress from the evaluator, so within an item the bar just shows the stage.
        const float frac = pr.total ? float(pr.done) / pr.total : 0.0f;
        const std::string label = std::to_string(pr.done) + " / " + std::to_string(pr.total) + "  " + pr.stage;
        ImGui::ProgressBar(frac, ImVec2(-ImGui::CalcTextSize("Cancel").x - 30, 0), label.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) exporter_.cancel();
    } else {
        std::string why;
        std::vector<ExportItem> items;
        int input = 0;
        if (tab == 0) {
            if (!exportPath_[0]) why = "Choose a file";
            else if (!graph_.firstOfType(OutputNode::staticInfo().type)) why = "Add an Output node";
            else items.push_back({"", withExt(exportPath_)});
        } else if (tab == 1) {
            input = batchInput_;
            ExportSettings tmp = es;
            tmp.nameTemplate = nameTemplate_;
            if (!graph_.find(input)) why = "The tree needs an Image Input node";
            else if (batchSources_.empty()) why = "Add source images";
            else if (!batchDir_[0]) why = "Choose an output folder";
            else if (!graph_.firstOfType(OutputNode::staticInfo().type)) why = "Add an Output node";
            else {
                std::vector<NameSource> names;
                for (const std::string& src : batchSources_) names.push_back({src});
                const std::vector<std::string> outs = batchOutputPaths(names, batchDir_, tmp);
                for (size_t k = 0; k < batchSources_.size(); ++k) items.push_back({batchSources_[k], outs[k]});
            }
        }
        ImGui::BeginDisabled(!why.empty());
        const std::string label = tab == 1 ? "Export " + std::to_string(items.size()) + " Images" : std::string("Export");
        if (ImGui::Button(label.c_str(), ImVec2(160, 0))) {
            if (tab == 1) std::filesystem::create_directories(u8ToPath(batchDir_));
            // Keep the path's extension in step with the chosen format.
            if (tab == 0) std::snprintf(exportPath_, sizeof(exportPath_), "%s", items[0].output.c_str());
            startExport(std::move(items), input);
        }
        ImGui::EndDisabled();
        if (!why.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", why.c_str());
        }
    }
    if (ImGui::BeginChild("##exportLog", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (const std::string& line : exportLog_) ImGui::TextWrapped("%s", line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- snapshots

void App::createSnapshot() {
    // Named after the time, as Lightroom does; rename it from its menu.
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    char name[32] = "Snapshot";
    if (localtime_s(&tm, &t) == 0) std::strftime(name, sizeof name, "%Y-%m-%d %H:%M:%S", &tm);
    snapshots_.push_back({name, graph_.toJson()});
    renamingSnapshot_ = int(snapshots_.size()) - 1;
    std::snprintf(snapshotName_, sizeof(snapshotName_), "%s", name);
    modified_ = true;  // kept in the project
    status_ = std::string("Created the snapshot ") + name;
}

void App::restoreSnapshot(int i) {
    if (i < 0 || i >= int(snapshots_.size())) return;
    Graph g;
    try {
        g.fromJson(snapshots_[size_t(i)].graph);
    } catch (const std::exception& e) {
        status_ = std::string("Snapshot can't be restored: ") + e.what();
        return;
    }
    // The whole edit is replaced, as an ordinary change: Ctrl+Z brings the previous one back.
    graph_ = std::move(g);
    groupPath_.clear();
    eyedropper().cancel();
    editor_.onGraphReplaced(false);
    selected_ = 0;
    if (!pathValid(previewPath_)) previewPath_.clear();
    markChanged(true);
    status_ = "Restored the snapshot " + snapshots_[size_t(i)].name;
}

void App::drawSnapshotsWindow() {
    ImGui::SetNextWindowSize(ImVec2(300, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Snapshots", &showSnapshots_)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Create Snapshot")) createSnapshot();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the edit as it is now under a name, to come back to it later");
    ImGui::Separator();
    if (snapshots_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("No snapshots. A snapshot keeps the whole edit, saved with the project.");
        ImGui::PopStyleColor();
    }
    int remove = -1;
    for (int i = 0; i < int(snapshots_.size()); ++i) {
        ImGui::PushID(i);
        if (renamingSnapshot_ == i) {
            if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool done = ImGui::InputText("##name", snapshotName_, sizeof(snapshotName_),
                                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (done || ImGui::IsItemDeactivated()) {
                if (snapshotName_[0] && snapshots_[size_t(i)].name != snapshotName_) {
                    snapshots_[size_t(i)].name = snapshotName_;
                    modified_ = true;
                }
                renamingSnapshot_ = -1;
            }
        } else {
            if (ImGui::Selectable(snapshots_[size_t(i)].name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) restoreSnapshot(i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to restore this snapshot (Ctrl+Z undoes it)");
            if (ImGui::BeginPopupContextItem("##snapMenu")) {
                if (ImGui::MenuItem("Restore")) restoreSnapshot(i);
                if (ImGui::MenuItem("Rename")) {
                    renamingSnapshot_ = i;
                    std::snprintf(snapshotName_, sizeof(snapshotName_), "%s", snapshots_[size_t(i)].name.c_str());
                }
                if (ImGui::MenuItem("Update with Current Settings")) {
                    snapshots_[size_t(i)].graph = graph_.toJson();
                    modified_ = true;
                    status_ = "Updated the snapshot " + snapshots_[size_t(i)].name;
                }
                if (ImGui::MenuItem("Delete")) remove = i;
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        snapshots_.erase(snapshots_.begin() + remove);
        renamingSnapshot_ = -1;
        modified_ = true;
    }
    ImGui::End();
}

// ---------------------------------------------------------------- helpers

void App::markChanged(bool eval) {
    modified_ = true;
    historyDirty_ = true;
    if (eval) evalDirty_ = true;
}

void App::updateTitle() {
    std::string name = projectPath_.empty() ? "Untitled" : pathToU8(u8ToPath(projectPath_).filename());
    std::string title = name + (modified_ ? " *" : "") + " - NodeLab " + kNodeLabVersion;
    if (title != lastTitle_) {
        glfwSetWindowTitle(window_, title.c_str());
        lastTitle_ = title;
    }
}

bool App::pathValid(const NodePath& p) {
    if (p.empty()) return false;
    Graph* g = resolveGroupPath(graph_, std::vector<int>(p.begin(), p.end() - 1));
    return g && g->find(p.back());
}

NodePath App::resultTarget() {
    if (pathValid(previewPath_)) return previewPath_;
    int out = graph_.firstOfType(OutputNode::staticInfo().type);
    return out ? NodePath{out} : NodePath{};
}

std::vector<App::Viewer*> App::detailViews() {
    std::vector<Viewer*> out{&left_};
    for (auto& v : viewers_) out.push_back(v.get());
    return out;
}

bool App::sameDetails(const std::vector<AsyncEvaluator::Detail>& a, const std::vector<AsyncEvaluator::Detail>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto &x = a[i], &y = b[i];
        // Within a fraction of a screen pixel: the same view.
        const float tol = 0.5f / std::max(x.screenW, 1.0f);
        if (x.tag != y.tag || x.node != y.node || x.pin != y.pin || std::fabs(x.u0 - y.u0) > tol ||
            std::fabs(x.u1 - y.u1) > tol || std::fabs(x.v0 - y.v0) > tol || std::fabs(x.v1 - y.v1) > tol ||
            std::fabs(x.screenW - y.screenW) > 0.5f)
            return false;
    }
    return true;
}

std::vector<AsyncEvaluator::Detail> App::wantedDetails() {
    std::vector<AsyncEvaluator::Detail> out;
    auto want = [&](const Viewer& v, int tag, int node, int pin, int shownW) {
        // Only views zoomed in past the preview's pixels; the evaluator decides whether the full
        // resolution has more to show.
        if (!node || shownW <= 0 || v.info.imageW <= shownW * 1.15f) return;
        AsyncEvaluator::Detail d;
        d.tag = tag;
        d.node = node;
        d.pin = pin;
        d.u0 = v.info.u0, d.v0 = v.info.v0, d.u1 = v.info.u1, d.v1 = v.info.v1;
        d.screenW = v.info.imageW;
        out.push_back(d);
    };
    if (showOriginal_ || splitView_ || beforeFull_) want(left_, -1, leftNode_, 0, leftShown_ ? leftShown_->w : 0);
    // Top-level nodes only: regions are evaluated in the root graph.
    for (size_t i = 0; i < std::min({viewers_.size(), submittedViewers_, submittedTargets_.size()}); ++i)
        if (submittedTargets_[i].size() == 1)
            want(*viewers_[i], viewers_[i]->id, submittedTargets_[i][0], i < submittedPins_.size() ? submittedPins_[i] : 0,
                 viewers_[i]->shownWidth());
    return out;
}

int App::wantedProxyEdge() const {
    // Enough pixels to fill the largest view when fitted, in steps so resizing a panel doesn't
    // re-evaluate for every pixel. Capped, because every cached node holds an image of this
    // size; zoomed-in views get full-resolution details instead.
    float edge = std::max(left_.info.panelW, left_.info.panelH);
    for (const auto& v : viewers_) edge = std::max({edge, v->info.panelW, v->info.panelH});
    if (edge <= 0) return proxyEdge_;
    return std::clamp(int(std::ceil(edge / 256.0f)) * 256, kMinProxyEdge, kMaxProxyEdge);
}

void App::requestDisplay(int slot, const ImagePtr& scene, bool clipping, bool histogram, bool tint,
                         const Value& gpuScene) {
    DisplayWorker::Request r;
    r.slot = slot;
    r.seq = ++displaySeq_[slot];
    r.scene = scene;
    r.cm = graph_.colorManagement;
    r.clipping = clipping;
    r.histogram = histogram;
    r.tint = tint;
    r.gpu = gpuDevice_ && gpu::available();
    r.keepTexture = r.gpu;
    r.gpuScene = gpuScene;
    display_.submit(std::move(r));
}

void App::refreshDetail(Viewer& v, bool main) {
    if (!v.detailShown) {
        dropDetail(v);
        return;
    }
    requestDisplay(detailSlot(v), v.detailShown, main && clipping_, false);
}

void App::dropDetail(Viewer& v) {
    v.detailTex.reset();
    v.detailShown.reset();
    dropDisplay(detailSlot(v));
}

void App::refreshDisplay(Viewer& v, bool main) {
    // The histogram follows the main Result.
    if (v.hasShown()) {
        requestDisplay(mainSlot(v), v.shown, main && clipping_, main, false, v.shownGpu);
        return;
    }
    dropDisplay(mainSlot(v));
    v.tex.reset();
    if (main) histogram_.valid = false;
}

void App::applyDisplays() {
    for (DisplayWorker::Result& r : display_.poll()) {
        if (r.seq != displaySeq_[r.slot]) continue;  // superseded while it was prepared
        auto upload = [&](GLTexture& t) {
            if (r.texture) t.showDevice(r.texture, r.w, r.h);
            else if (r.bytes.empty()) t.reset();
            else t.uploadBytes(r.bytes, r.w, r.h);
        };
        auto uploadDetail = [&](Viewer& v) {
            upload(v.detailTex);
            v.detail = v.pendingDetail;
            v.detail.tex = &v.detailTex;
        };
        if (r.slot == kSlotLeft) upload(leftTex_);
        else if (r.slot == kSlotMask) upload(maskTex_);
        else if (r.slot == kSlotLeftDetail) uploadDetail(left_);
        for (size_t i = 0; i < viewers_.size(); ++i) {
            Viewer& v = *viewers_[i];
            if (r.slot == mainSlot(v)) {
                upload(v.tex);
                if (i == 0) histogram_ = r.histogram;
            } else if (r.slot == detailSlot(v)) {
                uploadDetail(v);
            }
        }
    }
}

void App::updateTextures() {
    colordisplay::linear = graph_.colorManagement.linear;
    // Original panel: the selected Image Input (in the graph being edited), else the root's first.
    Graph& cur = currentGraph();
    Node* src = cur.find(selected_);
    if (!src || src->info().type != ImageInputNode::staticInfo().type)
        src = graph_.find(graph_.firstOfType(ImageInputNode::staticInfo().type));
    ImagePtr left;
    const ColorManagement& cm = graph_.colorManagement;
    if (src && !src->paramS(0).empty()) {
        const auto& in = static_cast<const ImageInputNode&>(*src);
        // Never decode here: the evaluator does, and the panel fills in once it has.
        ImagePtr decoded = cache_.cached(in.paramS(0), in.decode(cm.linear), proxyEdge_);
        // The Original shows the node's output (with a RAW's Baseline Exposure), scaled again
        // only when the image or the gain changes.
        const float gain = in.exposureGain();
        if (decoded != leftDecoded_ || gain != leftGain_) {
            leftDecoded_ = decoded;
            leftGain_ = gain;
            leftExposed_ = in.applyExposure(decoded);
        }
        left = leftExposed_;
    }
    leftNode_ = src && graph_.find(src->id) == src ? src->id : 0;
    // Changing the view transform only redraws; the graph's values are unaffected.
    const bool cmChanged = !(cm == shownCm_);
    shownCm_ = cm;
    if (left != leftShown_) dropDetail(left_);
    if (left != leftShown_ || cmChanged) {
        leftShown_ = left;
        if (left) {
            requestDisplay(kSlotLeft, left, false, false);
        } else {
            dropDisplay(kSlotLeft);
            leftTex_.reset();
        }
    }
    if (cmChanged) {
        for (size_t i = 0; i < viewers_.size(); ++i) {
            refreshDisplay(*viewers_[i], i == 0);
            refreshDetail(*viewers_[i], i == 0);
        }
        refreshDetail(left_, false);
    }

    auto res = eval_->poll();
    if (res && !res->images.empty()) {
        evalMs_ = res->ms;
        nodeMs_ = res->nodeMs;
        nodeGpu_ = res->nodeGpu;
        gpuFallbacks_ = res->gpuFallbacks;
        if (res->gpuFallbacks) gpuError_ = res->gpuError;
        if (!res->draft) previewMs_ = res->ms;
        // Results are in submission order; match them to the viewers that still exist.
        const size_t nv = std::min({viewers_.size(), res->images.size(), submittedViewers_});
        for (size_t i = 0; i < nv; ++i) {
            Viewer& v = *viewers_[i];
            v.error = res->errors[i];
            // A changed image makes the detail stale; its new one follows (unchanged: the
            // evaluation was only for the view, so the old detail stays until replaced).
            const Value gpuImage = i < res->gpuImages.size() ? res->gpuImages[i] : Value();
            if (res->images[i] != v.shown || gpuImage.v != v.shownGpu.v) dropDetail(v);
            v.shown = res->images[i];
            v.shownGpu = gpuImage;
            refreshDisplay(v, i == 0);
        }
        // A mask target follows the viewers (see drawFrame).
        if (maskWanted_ && res->images.size() > submittedViewers_) {
            const size_t m = submittedViewers_;
            const Value gpuMask = m < res->gpuImages.size() ? res->gpuImages[m] : Value();
            if (res->images[m] || !gpuMask.empty()) requestDisplay(kSlotMask, res->images[m], false, false, true, gpuMask);
        }
    }
    if (res && res->tilesDone) {
        if (!res->tilesError.empty()) status_ = "The zoomed-in detail failed (" + res->tilesError + "); showing the preview";
        for (const AsyncEvaluator::Tile& t : res->tiles) {
            Viewer* v = nullptr;
            for (Viewer* c : detailViews())
                if ((c == &left_ ? -1 : c->id) == t.tag) v = c;
            if (!v) continue;
            v->detailShown = t.image;
            v->pendingDetail = ViewDetail{nullptr, t.u0, t.v0, t.u1, t.v1};
            refreshDetail(*v, v != &left_ && v->id == 0);
        }
    }
    applyDisplays();
}

bool App::saveFramebuffer(const std::string& path) {
    int w, h;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return false;
    std::vector<unsigned char> px(size_t(w) * h * 4);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Image img(w, h);
    for (int y = 0; y < h; ++y)  // GL rows are bottom-up
        for (int x = 0; x < w * 4; ++x) img.px[size_t(h - 1 - y) * w * 4 + x] = px[size_t(y) * w * 4 + x] / 255.0f;
    std::string err;
    return saveImage(path, img, err);
}

nlohmann::json App::uiState() const {
    nlohmann::json viewers = nlohmann::json::array();
    for (size_t i = 1; i < viewers_.size(); ++i) viewers.push_back({{"pin", viewers_[i]->pin}, {"sync", viewers_[i]->sync}});
    return {{"view", {view_.zoom, view_.panX, view_.panY}},
            {"preview", previewPath_},
            {"graphView", groupPath_.empty() ? editor_.viewState()
                          : groupViews_.count(std::vector<int>()) ? groupViews_.at(std::vector<int>()) : nlohmann::json()},
            {"viewers", viewers},
            {"histogram", showHistogram_},
            {"clipping", clipping_},
            {"maskOverlay", maskOverlay_},
            {"library", libraryPhotoOpen() ? library_.meta(library_.current()).toJson() : nlohmann::json()},
            {"snapshots", [&] {
                 nlohmann::json a = nlohmann::json::array();
                 for (const Snapshot& sn : snapshots_) a.push_back({{"name", sn.name}, {"graph", sn.graph}});
                 return a;
             }()},
            {"export", [&] {
                 nlohmann::json e = exportSettings_.toJson();
                 e["nameTemplate"] = std::string(nameTemplate_);
                 e["path"] = std::string(exportPath_);
                 e["batchDir"] = std::string(batchDir_);
                 return e;
             }()}};
}

void App::applyUiState(const nlohmann::json& j) {
    view_.reset();
    viewers_.resize(1);
    previewPath_.clear();
    snapshots_.clear();
    renamingSnapshot_ = -1;
    try {
        if (auto sn = j.find("snapshots"); sn != j.end() && sn->is_array())
            for (const auto& e : *sn)
                if (e.is_object() && e.contains("graph") && e["graph"].is_object())
                    snapshots_.push_back({e.value("name", std::string("Snapshot")), e["graph"]});
        if (auto v = j.find("view"); v != j.end() && v->size() == 3) {
            view_.zoom = (*v)[0].get<float>();
            view_.panX = (*v)[1].get<float>();
            view_.panY = (*v)[2].get<float>();
        }
        if (auto gv = j.find("graphView"); gv != j.end()) editor_.setViewState(*gv);
        showHistogram_ = j.value("histogram", showHistogram_);
        clipping_ = j.value("clipping", clipping_);
        maskOverlay_ = j.value("maskOverlay", maskOverlay_);
        if (auto p = j.find("preview"); p != j.end()) {
            // Older projects stored a plain node id.
            if (p->is_number_integer() && p->get<int>() != 0) previewPath_ = {p->get<int>()};
            else if (p->is_array()) previewPath_ = p->get<NodePath>();
        }
        if (!pathValid(previewPath_)) previewPath_.clear();
        if (auto e = j.find("export"); e != j.end() && e->is_object()) {
            exportSettings_.fromJson(*e);
            std::snprintf(nameTemplate_, sizeof(nameTemplate_), "%s", exportSettings_.nameTemplate.c_str());
            std::snprintf(exportPath_, sizeof(exportPath_), "%s", e->value("path", std::string()).c_str());
            std::snprintf(batchDir_, sizeof(batchDir_), "%s", e->value("batchDir", std::string()).c_str());
        }
        if (auto vs = j.find("viewers"); vs != j.end() && vs->is_array())
            for (const auto& vj : *vs) {
                auto v = std::make_unique<Viewer>();
                v->id = nextViewerId_++;
                v->pin = vj.value("pin", NodePath{});
                v->sync = vj.value("sync", false);
                viewers_.push_back(std::move(v));
            }
    } catch (const std::exception&) {
        // UI state is cosmetic; ignore anything malformed.
    }
}
