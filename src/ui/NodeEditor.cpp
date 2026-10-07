#include "ui/NodeEditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <imgui_internal.h>

#include "core/Curve.h"
#include "core/Ramp.h"
#include "graph/Layout.h"
#include "graph/NodeMenu.h"
#include "graph/NodeRegistry.h"
#include "graph/Recipes.h"
#include "nodes/group/GroupNodes.h"
#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Paths.h"
#include "io/Presets.h"
#include "ui/ColorDisplay.h"
#include "ui/Eyedropper.h"
#include "ui/FileDialog.h"
#include "ui/GuideWindow.h"
#include "ui/SliderTrack.h"
#include "ui/Theme.h"

namespace {

// Node geometry in grid units (multiplied by zoom on screen).
constexpr float kNodeW = 200.0f;
constexpr float kTitleH = 26.0f;
constexpr float kRowH = 24.0f;
constexpr float kPad = 6.0f;
constexpr float kPinR = 5.0f;
constexpr float kMinZoom = 0.25f, kMaxZoom = 2.0f;


bool containsNoCase(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    auto lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    };
    lower(h);
    lower(n);
    return h.find(n) != std::string::npos;
}

ImU32 categoryColor(const std::string& cat) { return theme::categoryColor(cat); }

bool pinBacked(const NodeInfo& info, int param) {
    for (const auto& p : info.inputs)
        if (p.fallbackParam == param) return true;
    return false;
}

// Rows a param occupies on the node body (0 = inspector only / shown on its input pin).
int paramRows(const Node& n, int i) {
    const NodeInfo& info = n.info();
    if (info.compact || !n.paramVisible(i)) return 0;
    switch (info.params[i].kind) {
        case ParamKind::Float: return pinBacked(info, i) ? 0 : 1;
        case ParamKind::Path:
        case ParamKind::Enum:
        case ParamKind::Bool:
        case ParamKind::Text:
        case ParamKind::Ramp:
        case ParamKind::Color:
        case ParamKind::SavePath: return 1;
        case ParamKind::Curve: return 4;
        case ParamKind::Int: return 0;
    }
    return 0;
}

// A reroute is a small title-bar-only node, its pins on the ends: the body selects and drags it
// like any node (a dot with both pins inside it mostly started wires instead).
constexpr float kRerouteW = 90.0f;
bool isReroute(const Node& n) { return n.info().type == "util.reroute"; }

float nodeHeightGrid(const Node& n) {
    if (isReroute(n)) return kTitleH;
    if (n.collapsed) {
        int pins = int(std::max(n.info().inputs.size(), n.info().outputs.size()));
        return std::max(kTitleH + 6.0f, kTitleH * 0.5f + pins * 8.0f + 8.0f);
    }
    const NodeInfo& info = n.info();
    int rows = int(info.inputs.size() + info.outputs.size());
    for (int i = 0; i < int(info.params.size()); ++i) rows += paramRows(n, i);
    return kTitleH + kPad * 2 + rows * kRowH;
}

// Drawn size in grid units, for automatic spacing.
NodeSize gridSize(const Node& n) { return {isReroute(n) ? kRerouteW : kNodeW, nodeHeightGrid(n)}; }

void bezierPoints(ImVec2 a, ImVec2 b, float zoom, ImVec2& c1, ImVec2& c2) {
    float d = std::max(std::fabs(b.x - a.x) * 0.5f, 40.0f * zoom);
    c1 = ImVec2(a.x + d, a.y);
    c2 = ImVec2(b.x - d, b.y);
}

// Index of the input (inputs=true) or output pin best matching `other`, or -1.
// Inputs: other is the source type; exact type first, then anything convertible. Free inputs preferred.
int pickPin(const Graph& g, const Node& n, bool inputs, PinType other) {
    const auto& pins = inputs ? n.info().inputs : n.info().outputs;
    for (int pass = 0; pass < 4; ++pass) {
        const bool exact = pass % 2 == 0, needFree = inputs && pass < 2;
        for (int i = 0; i < int(pins.size()); ++i) {
            if (needFree && g.inputLink(n.id, i)) continue;
            bool ok = exact ? pins[i].type == other
                            : (inputs ? canConvert(other, pins[i].type) : canConvert(pins[i].type, other));
            if (ok) return i;
        }
    }
    return -1;
}

// Slider-like field: fill proportional to the value, label left, value right.
void drawValueField(ImDrawList* dl, const ImRect& box, const char* label, float v, const ParamDesc& d, float fs,
                    float zoom, bool hovered) {
    const float frac = d.max > d.min ? std::clamp((v - d.min) / (d.max - d.min), 0.0f, 1.0f) : 0.0f;
    const float round = 3.0f * zoom;
    if (d.track != SliderTrack::None) {
        // Coloured track (Temperature, Hue...) with a marker at the value instead of a fill.
        slidertrack::draw(dl, box.Min, box.Max, d, theme::col(hovered ? theme::FieldHover : theme::Field), round);
        const float x = box.Min.x + box.GetWidth() * frac;
        const float hw = std::max(1.0f, zoom);
        dl->AddRectFilled(ImVec2(x - hw - 1, box.Min.y), ImVec2(x + hw + 1, box.Max.y), IM_COL32(0, 0, 0, 160));
        dl->AddRectFilled(ImVec2(x - hw, box.Min.y), ImVec2(x + hw, box.Max.y), IM_COL32(255, 255, 255, 235));
    } else {
        dl->AddRectFilled(box.Min, box.Max, theme::col(hovered ? theme::FieldHover : theme::Field), round);
        dl->AddRectFilled(box.Min, ImVec2(box.Min.x + box.GetWidth() * frac, box.Max.y),
                          theme::col(hovered ? theme::SliderFillHover : theme::SliderFill), round);
    }
    if (fs < 6.0f) return;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    ImFont* font = ImGui::GetFont();
    const ImVec2 vs = font->CalcTextSizeA(fs, FLT_MAX, 0, buf);
    const float ty = box.GetCenter().y - fs * 0.5f;
    const float pad = 6.0f * zoom;
    ImVec4 clip(box.Min.x, box.Min.y, box.Max.x - vs.x - pad * 2, box.Max.y);
    dl->AddText(font, fs, ImVec2(box.Min.x + pad, ty), theme::col(theme::FieldText), label, nullptr, 0.0f, &clip);
    dl->AddText(font, fs, ImVec2(box.Max.x - pad - vs.x, ty), theme::col(theme::FieldValue), buf);
}

}  // namespace

ImU32 pinColor(PinType t) {
    switch (t) {
        case PinType::Image: return theme::col(theme::WireImage);
        case PinType::Channel: return theme::col(theme::WireChannel);
        case PinType::Number: return theme::col(theme::WireNumber);
    }
    return IM_COL32_WHITE;
}

// ---------------------------------------------------------------- view helpers

ImVec2 NodeEditor::toScreen(ImVec2 p) const {
    return ImVec2(origin_.x + pan_.x + p.x * zoom_, origin_.y + pan_.y + p.y * zoom_);
}

ImVec2 NodeEditor::toGrid(ImVec2 p) const {
    return ImVec2((p.x - origin_.x - pan_.x) / zoom_, (p.y - origin_.y - pan_.y) / zoom_);
}

void NodeEditor::placeAtScreen(Node& n, ImVec2 screen) const {
    ImVec2 gp = toGrid(screen);
    n.x = std::round(gp.x - kNodeW * 0.5f);
    n.y = std::round(gp.y - kTitleH * 0.5f);
}

NodeEditor::Layout NodeEditor::layoutFor(const Node& n) const {
    const NodeInfo& info = n.info();
    const float z = zoom_;
    Layout L;
    L.min = toScreen(ImVec2(n.x, n.y));
    if (isReroute(n)) {
        L.max = ImVec2(L.min.x + kRerouteW * z, L.min.y + kTitleH * z);
        L.titleH = kTitleH * z;
        L.outPins.emplace_back(L.max.x, L.min.y + L.titleH * 0.5f);
        L.inPins.emplace_back(L.min.x, L.min.y + L.titleH * 0.5f);
        L.valueBoxes.push_back(ImRect());
        L.paramBoxes.resize(info.params.size());
        return L;
    }
    L.max = ImVec2(L.min.x + kNodeW * z, L.min.y + nodeHeightGrid(n) * z);
    L.titleH = kTitleH * z;
    if (n.collapsed) {
        // Pins stacked along the sides of the title bar.
        for (size_t i = 0; i < info.outputs.size(); ++i) L.outPins.emplace_back(L.max.x, L.min.y + (kTitleH * 0.5f + i * 8.0f) * z);
        for (size_t i = 0; i < info.inputs.size(); ++i) {
            L.inPins.emplace_back(L.min.x, L.min.y + (kTitleH * 0.5f + i * 8.0f) * z);
            L.valueBoxes.push_back(ImRect());
        }
        L.paramBoxes.resize(info.params.size());
        return L;
    }
    float y = L.min.y + (kTitleH + kPad) * z;
    const float row = kRowH * z;
    for (size_t i = 0; i < info.outputs.size(); ++i, y += row) L.outPins.emplace_back(L.max.x, y + row * 0.5f);
    L.paramBoxes.resize(info.params.size());
    for (int i = 0; i < int(info.params.size()); ++i) {
        int rows = paramRows(n, i);
        if (rows == 0) continue;
        L.paramBoxes[i] = ImRect(L.min.x + 10 * z, y + 2 * z, L.max.x - 10 * z, y + row * rows - 2 * z);
        y += row * rows;
    }
    for (size_t i = 0; i < info.inputs.size(); ++i, y += row) {
        L.inPins.emplace_back(L.min.x, y + row * 0.5f);
        ImRect box;
        if (info.inputs[i].fallbackParam >= 0 && info.params[info.inputs[i].fallbackParam].kind == ParamKind::Float)
            box = ImRect(L.min.x + 10 * z, y + 2 * z, L.max.x - 10 * z, y + row - 2 * z);
        L.valueBoxes.push_back(box);
    }
    return L;
}

void NodeEditor::syncOrder(Graph& g) {
    g.pruneInvalidLinks();  // safety net: never draw a wire to a pin that no longer exists
    std::erase_if(order_, [&](int id) { return !g.find(id); });
    for (const auto& [id, n] : g.nodes())
        if (std::find(order_.begin(), order_.end(), id) == order_.end()) order_.push_back(id);
    for (auto it = selection_.begin(); it != selection_.end();) it = g.find(*it) ? std::next(it) : selection_.erase(it);
    if (selectedLink_ && std::none_of(g.links().begin(), g.links().end(),
                                      [&](const Link& l) { return l.id == selectedLink_; }))
        selectedLink_ = 0;
}

void NodeEditor::doFrame(const Graph& g) {
    if (g.nodes().empty() || size_.x < 50 || size_.y < 50) {
        zoom_ = 1.0f;
        pan_ = ImVec2(40, 40);
        return;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& [id, n] : g.nodes()) {
        x0 = std::min(x0, n->x);
        y0 = std::min(y0, n->y);
        x1 = std::max(x1, n->x + kNodeW);
        y1 = std::max(y1, n->y + nodeHeightGrid(*n));
    }
    zoom_ = std::clamp(std::min((size_.x - 60) / (x1 - x0), (size_.y - 60) / (y1 - y0)), 0.3f, 1.0f);
    pan_ = ImVec2(size_.x * 0.5f - (x0 + x1) * 0.5f * zoom_, size_.y * 0.5f - (y0 + y1) * 0.5f * zoom_);
}

void NodeEditor::onGraphReplaced(bool frame) {
    selection_.clear();
    selectedLink_ = 0;
    order_.clear();
    mode_ = Mode::None;
    editing_ = {};
    selectedFrame_ = 0;
    activeNode_ = 0;
    activeParam_ = -1;
    insertLink_ = 0;
    hoverPin_ = {};
    if (frame) fitFrames_ = 3;
}

void NodeEditor::select(int nodeId) {
    selection_ = {nodeId};
    selectedLink_ = 0;
}

nlohmann::json NodeEditor::viewState() const { return {pan_.x, pan_.y, zoom_}; }

void NodeEditor::setViewState(const nlohmann::json& j) {
    if (!j.is_array() || j.size() != 3) return;
    pan_ = ImVec2(j[0].get<float>(), j[1].get<float>());
    zoom_ = std::clamp(j[2].get<float>(), kMinZoom, kMaxZoom);
    fitFrames_ = 0;
}

// ---------------------------------------------------------------- hit testing

int NodeEditor::hitNode(const Graph& g, ImVec2 p) const {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const Node* n = g.find(*it);
        if (!n) continue;
        Layout L = layoutFor(*n);
        if (ImRect(L.min, L.max).Contains(p)) return *it;
    }
    return 0;
}

NodeEditor::PinRef NodeEditor::hitPin(const Graph& g, ImVec2 p) const {
    const float r = std::max(8.0f, 9.0f * zoom_);
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const Node* n = g.find(*it);
        if (!n) continue;
        Layout L = layoutFor(*n);
        for (int i = 0; i < int(L.outPins.size()); ++i)
            if (ImLengthSqr(L.outPins[i] - p) < r * r) return {*it, i, true};
        for (int i = 0; i < int(L.inPins.size()); ++i)
            if (ImLengthSqr(L.inPins[i] - p) < r * r) return {*it, i, false};
    }
    return {};
}

void NodeEditor::linkEnds(const Graph& g, const Link& l, ImVec2& a, ImVec2& b) const {
    a = layoutFor(*g.find(l.fromNode)).outPins[l.fromPin];
    b = layoutFor(*g.find(l.toNode)).inPins[l.toPin];
}

int NodeEditor::hitLink(const Graph& g, ImVec2 p) const {
    const float tol = std::max(5.0f, 5.0f * zoom_);
    for (const Link& l : g.links()) {
        ImVec2 a, b, c1, c2;
        linkEnds(g, l, a, b);
        bezierPoints(a, b, zoom_, c1, c2);
        ImVec2 closest = ImBezierCubicClosestPointCasteljau(a, c1, c2, b, p, 1.0f);
        if (ImLengthSqr(closest - p) < tol * tol) return l.id;
    }
    return 0;
}

// ---------------------------------------------------------------- drawing

void NodeEditor::drawGrid(ImDrawList* dl) const {
    float step = 24.0f * zoom_;
    while (step < 10.0f) step *= 4.0f;
    const ImU32 col = theme::col(theme::Grid);
    for (float x = std::fmod(pan_.x, step); x < size_.x; x += step)
        dl->AddLine(ImVec2(origin_.x + x, origin_.y), ImVec2(origin_.x + x, origin_.y + size_.y), col);
    for (float y = std::fmod(pan_.y, step); y < size_.y; y += step)
        dl->AddLine(ImVec2(origin_.x, origin_.y + y), ImVec2(origin_.x + size_.x, origin_.y + y), col);
}

void NodeEditor::drawLinks(ImDrawList* dl, const Graph& g) const {
    const float thick = std::max(1.5f, 2.5f * zoom_);
    for (const Link& l : g.links()) {
        ImVec2 a, b, c1, c2;
        linkEnds(g, l, a, b);
        bezierPoints(a, b, zoom_, c1, c2);
        ImU32 col = pinColor(g.find(l.fromNode)->info().outputs[l.fromPin].type);
        float t = thick;
        if (l.id == insertLink_ || l.id == selectedLink_) {
            dl->AddBezierCubic(a, c1, c2, b, IM_COL32(255, 255, 255, 90), thick * 3.0f);
            col = IM_COL32(255, 255, 255, 255);
            t = thick * 1.4f;
        }
        dl->AddBezierCubic(a, c1, c2, b, col, t);
    }
}

bool NodeEditor::drawValueBox(Node& n, int param, const ImRect& box, ImDrawList* dl, const char* label) {
    const ParamDesc& d = n.info().params[param];
    const float fs = ImGui::GetFontSize() * zoom_;
    bool changed = false;
    ImGui::PushID(param);
    ImGui::SetCursorScreenPos(box.Min);

    if (editing_.node == n.id && editing_.param == param) {
        // Text entry mode (click without dragging).
        float v = n.paramF(param);
        ImGui::SetNextItemWidth(box.GetWidth());
        ImGui::SetWindowFontScale(zoom_);
        if (editing_.frames++ == 0) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat("##edit", &v, 0, 0, "%.3f", ImGuiInputTextFlags_AutoSelectAll)) {
            n.params[param] = std::clamp(v, d.min, d.max);
            changed = true;
        }
        const bool active = ImGui::IsItemActive();
        ImGui::SetWindowFontScale(1.0f);
        if (active) editing_.wasActive = true;
        else if (editing_.wasActive || editing_.frames > 3) editing_ = {};  // focus left the field: done
        ImGui::PopID();
        return changed;
    }

    static bool dragged = false;  // only one value box can be active at a time
    ImGui::InvisibleButton("##val", box.GetSize());
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) {
        dragged = false;
        activeNode_ = n.id;
        activeParam_ = param;
    }
    if (ImGui::IsItemDeactivated()) activeNode_ = 0, activeParam_ = -1;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        const ImGuiIO& io = ImGui::GetIO();
        float speed = (d.max - d.min) / std::max(20.0f, box.GetWidth());
        if (io.KeyShift) speed *= 0.1f;
        float v = std::clamp(n.paramF(param) + io.MouseDelta.x * speed, d.min, d.max);
        if (v != n.paramF(param)) {
            n.params[param] = v;
            changed = true;
        }
        dragged = true;
    }
    if (ImGui::IsItemDeactivated() && !dragged) editing_ = EditState{n.id, param, 0, false};
    if (ImGui::IsItemHovered()) {
        // Blender: Backspace over a field resets it; right-click offers the same.
        valueHovered_ = true;
        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            n.resetParam(param);
            changed = true;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            valueMenuNode_ = n.id, valueMenuParam_ = param;
            openValueMenu_ = true;
        }
    }

    drawValueField(dl, box, label, n.paramF(param), d, fs, zoom_, ImGui::IsItemHovered() || ImGui::IsItemActive());
    ImGui::PopID();
    return changed;
}

// One param row on the node body. canInteract: this node is topmost under the mouse.
bool NodeEditor::drawParamRow(ImDrawList* dl, Node& n, int i, const ImRect& box, bool canInteract) {
    const ParamDesc& d = n.info().params[i];
    const float z = zoom_;
    const float fs = ImGui::GetFontSize() * z;
    const bool showText = fs >= 5.0f;
    const ImU32 textCol = theme::col(theme::FieldText);
    const ImVec4 bclip(box.Min.x + 3 * z, box.Min.y, box.Max.x - 3 * z, box.Max.y);
    auto text = [&](float x, const char* s, ImU32 col) {
        if (showText) dl->AddText(ImGui::GetFont(), fs, ImVec2(x, box.GetCenter().y - fs * 0.5f), col, s, nullptr, 0.0f, &bclip);
    };
    const bool mineEditing = editing_.node == n.id && editing_.param == i;
    const bool mineActive = activeNode_ == n.id && activeParam_ == i;
    const bool mineMenu = enumNode_ == n.id && enumParam_ == i;
    bool changed = false;
    bool hovered = false;
    ImGui::PushID(2000 + i);

    auto button = [&]() {
        ImGui::SetCursorScreenPos(box.Min);
        bool clicked = ImGui::InvisibleButton("##row", box.GetSize());
        hovered = ImGui::IsItemHovered();
        return clicked;
    };

    switch (d.kind) {
        case ParamKind::Float:
            if (canInteract || mineEditing || mineActive) changed = drawValueBox(n, i, box, dl, d.name.c_str());
            else drawValueField(dl, box, d.name.c_str(), n.paramF(i), d, fs, z, false);
            break;
        case ParamKind::Path: {
            std::string path = n.paramS(i);
            std::string label = path.empty() ? "Choose image..." : pathToU8(u8ToPath(path).filename());
            if (canInteract && button()) {
                if (auto p = openFileDialog("Choose image", kImageFileFilter)) {
                    chooseImageFile(n, i, *p);
                    changed = true;
                }
            }
            if (hovered && !path.empty()) ImGui::SetTooltip("%s", path.c_str());
            dl->AddRectFilled(box.Min, box.Max, theme::col(hovered ? theme::ButtonHover : theme::Button), 3 * z);
            text(box.Min.x + 6 * z, label.c_str(), textCol);
            break;
        }
        case ParamKind::Enum: {
            int v = n.paramI(i);
            const char* opt = (v >= 0 && v < int(d.options.size())) ? d.options[v].c_str() : "?";
            if (canInteract && button()) {
                enumNode_ = n.id;
                enumParam_ = i;
                ImGui::OpenPopup("##enum");
            }
            dl->AddRectFilled(box.Min, box.Max, theme::col(hovered ? theme::ButtonHover : theme::Button), 3 * z);
            text(box.Min.x + 6 * z, opt, textCol);
            float ax = box.Max.x - 10 * z, ay = box.GetCenter().y, as = 3.5f * z;
            dl->AddTriangleFilled(ImVec2(ax - as, ay - as * 0.6f), ImVec2(ax + as, ay - as * 0.6f), ImVec2(ax, ay + as * 0.8f), textCol);
            // The popup lives in this node's ID scope, so it is drawn here while open even if the
            // mouse has moved off the node.
            if (enumNode_ == n.id && enumParam_ == i) {
                if (ImGui::BeginPopup("##enum")) {
                    for (int k = 0; k < int(d.options.size()); ++k)
                        if (ImGui::Selectable(d.options[k].c_str(), k == v)) {
                            n.params[i] = k;
                            changed = true;
                        }
                    ImGui::EndPopup();
                } else {
                    enumNode_ = 0;
                    enumParam_ = -1;
                }
            }
            (void)mineMenu;
            break;
        }
        case ParamKind::Bool: {
            if (canInteract && button()) {
                n.params[i] = !n.paramB(i);
                changed = true;
            }
            const float sz = box.GetHeight() - 4 * z;
            ImVec2 c0(box.Min.x + 2 * z, box.GetCenter().y - sz * 0.5f), c1(c0.x + sz, c0.y + sz);
            dl->AddRectFilled(c0, c1, theme::col(hovered ? theme::ButtonHover : theme::Button), 3 * z);
            if (n.paramB(i))
                dl->AddRectFilled(ImVec2(c0.x + 3 * z, c0.y + 3 * z), ImVec2(c1.x - 3 * z, c1.y - 3 * z),
                                  theme::col(theme::CheckFill), 2 * z);
            text(c1.x + 6 * z, d.name.c_str(), textCol);
            break;
        }
        case ParamKind::Text: {
            if (mineEditing) {
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", n.paramS(i).c_str());
                ImGui::SetCursorScreenPos(box.Min);
                ImGui::SetNextItemWidth(box.GetWidth());
                ImGui::SetWindowFontScale(z);
                if (editing_.frames++ == 0) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##text", buf, sizeof(buf))) {
                    n.params[i] = std::string(buf);
                    changed = true;
                }
                const bool active = ImGui::IsItemActive();
                ImGui::SetWindowFontScale(1.0f);
                if (active) editing_.wasActive = true;
                else if (editing_.wasActive || editing_.frames > 3) editing_ = {};
                break;
            }
            if (canInteract && button()) editing_ = EditState{n.id, i, 0, false};
            dl->AddRectFilled(box.Min, box.Max, theme::col(hovered ? theme::FieldHover : theme::Field), 3 * z);
            text(box.Min.x + 6 * z, n.paramS(i).c_str(), IM_COL32(200, 230, 200, 255));
            if (hovered) ImGui::SetTooltip("%s - click to edit", d.name.c_str());
            break;
        }
        case ParamKind::Ramp: {
            // Gradient preview; stops are edited in the inspector.
            ColorRamp ramp = rampFromJson(n.params[i]);
            const int segs = 48;
            auto col = [](float* c) {
                colordisplay::toDisplay(c);
                return ImGui::GetColorU32(ImVec4(c[0], c[1], c[2], 1.0f));
            };
            for (int k = 0; k < segs; ++k) {
                float c0[4], c1[4];
                ramp.eval(float(k) / segs, c0);
                ramp.eval(float(k + 1) / segs, c1);
                float x0 = box.Min.x + box.GetWidth() * k / segs, x1 = box.Min.x + box.GetWidth() * (k + 1) / segs;
                dl->AddRectFilledMultiColor(ImVec2(x0, box.Min.y), ImVec2(x1, box.Max.y), col(c0), col(c1), col(c1), col(c0));
            }
            dl->AddRect(box.Min, box.Max, IM_COL32(20, 20, 24, 255));
            break;
        }
        case ParamKind::Curve: {
            dl->AddRectFilled(box.Min, box.Max, IM_COL32(26, 26, 30, 255), 3 * z);
            dl->AddLine(ImVec2(box.Min.x, box.Max.y), ImVec2(box.Max.x, box.Min.y), IM_COL32(60, 60, 68, 255));
            // Standard curves: r, g, b then master on top. Custom channels (hue curves, float curve)
            // come from the param's key list.
            std::vector<std::string> keys = {"r", "g", "b", "master"};
            std::vector<ImU32> cols = {IM_COL32(220, 80, 80, 200), IM_COL32(80, 200, 90, 200), IM_COL32(90, 130, 230, 200),
                                       IM_COL32(235, 235, 240, 255)};
            bool standard = true;
            if (!d.options.empty()) {
                keys.clear();
                cols.clear();
                standard = false;
                for (const auto& k : d.options)
                    if (k[0] != '@') keys.push_back(k.substr(0, k.find(':'))), cols.push_back(IM_COL32(235, 235, 240, 220));
            }
            const nlohmann::json& cj = n.params[i];
            for (int c = 0; c < int(keys.size()); ++c) {
                CurvePoints pts = curveFromJson(cj.is_object() && cj.contains(keys[c]) ? cj[keys[c]] : nlohmann::json());
                if (standard && c < 3 && isIdentityCurve(pts)) continue;
                ImVec2 prev;
                for (int k = 0; k <= 32; ++k) {
                    float x = k / 32.0f, y = evalCurve(pts, x);
                    ImVec2 p(box.Min.x + x * box.GetWidth(), box.Max.y - y * box.GetHeight());
                    if (k) dl->AddLine(prev, p, cols[c], 1.5f);
                    prev = p;
                }
            }
            break;
        }
        case ParamKind::Color: {
            float c[3];
            n.paramC(i, c);
            if (!d.gammaColor) colordisplay::toDisplay(c);
            if (canInteract && button()) ImGui::OpenPopup("##color");
            const float sw = box.GetHeight() * 1.6f;
            ImRect swatch(ImVec2(box.Max.x - sw, box.Min.y), box.Max);
            dl->AddRectFilled(box.Min, box.Max, theme::col(hovered ? theme::FieldHover : theme::Field), 3 * z);
            dl->AddRectFilled(swatch.Min, swatch.Max,
                              ImGui::GetColorU32(ImVec4(std::min(c[0], 1.0f), std::min(c[1], 1.0f), std::min(c[2], 1.0f), 1.0f)), 3 * z);
            text(box.Min.x + 6 * z, d.name.c_str(), textCol);
            if (ImGui::BeginPopup("##color")) {
                ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoAlpha;
                if (d.max > 1.0f) flags |= ImGuiColorEditFlags_HDR;  // lift/gain may go above 1
                if (ImGui::ColorPicker3("##picker", c, flags)) {
                    if (!d.gammaColor) colordisplay::fromDisplay(c);
                    for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k], d.hardMin, d.hardMax);
                    n.params[i] = nlohmann::json::array({c[0], c[1], c[2]});
                    changed = true;
                }
                if (ImGui::Button(eyedropper().is(n.id, i) ? "Cancel Eyedropper" : "Pick from Image",
                                  ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    eyedropper().toggle(n.id, i);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            break;
        }
        case ParamKind::SavePath: {
            std::string path = n.paramS(i);
            std::string label = path.empty() ? "Save as..." : pathToU8(u8ToPath(path).filename());
            if (canInteract && button()) {
                if (auto p = saveFileDialog("File Output", kSaveImageFilter, "png")) {
                    n.params[i] = *p;
                    // File Output: the type picked in the dialog sets the Format.
                    const auto& descs = n.info().params;
                    if (size_t(i) + 1 < descs.size() && descs[i + 1].name == "Format")
                        n.params[i + 1] = int(formatFromPath(*p));
                    changed = true;
                }
            }
            if (hovered && !path.empty()) ImGui::SetTooltip("%s", path.c_str());
            dl->AddRectFilled(box.Min, box.Max, theme::col(hovered ? theme::ButtonHover : theme::Button), 3 * z);
            text(box.Min.x + 6 * z, label.c_str(), textCol);
            break;
        }
        case ParamKind::Int: break;
    }
    ImGui::PopID();
    return changed;
}

bool NodeEditor::drawNode(ImDrawList* dl, Graph& g, Node& n, int preview, Result& r) {
    const NodeInfo& info = n.info();
    const Layout L = layoutFor(n);
    const float z = zoom_;
    // Collapsed nodes and reroutes are just a title bar with their pins on its ends.
    const bool bar = n.collapsed || isReroute(n);
    const float fs = ImGui::GetFontSize() * z;
    const bool showText = fs >= 5.0f;
    const bool interactive = z >= 0.45f;
    const ImVec4 clip(L.min.x, L.min.y, L.max.x, L.max.y);
    const float round = 6.0f * z;
    const bool selected = selection_.count(n.id) > 0;
    bool changed = false;

    // body + title
    dl->AddRectFilled(ImVec2(L.min.x + 3, L.min.y + 4), ImVec2(L.max.x + 3, L.max.y + 4), IM_COL32(0, 0, 0, 70), round);
    dl->AddRectFilled(L.min, L.max, theme::col(theme::NodeBody), round);
    ImU32 titleCol = n.id == preview ? theme::col(theme::PreviewTitle) : categoryColor(info.category);
    if (n.muted) titleCol = theme::col(theme::MutedTitle);
    dl->AddRectFilled(L.min, ImVec2(L.max.x, L.min.y + L.titleH), titleCol, round,
                      bar ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop);
    if (showText) {
        std::string title = n.label.empty() ? info.displayName : n.label;
        if (n.muted) title += "  (muted)";
        dl->AddText(ImGui::GetFont(), fs, ImVec2(L.min.x + (bar ? 14 : 8) * z, L.min.y + (L.titleH - fs) * 0.5f),
                    n.muted ? IM_COL32(200, 170, 170, 255) : theme::col(theme::TitleText), title.c_str(), nullptr, 0.0f, &clip);
    }
    if (n.muted && !bar && !L.inPins.empty() && !L.outPins.empty()) {
        // Red pass-through line like Blender's muted nodes.
        dl->AddLine(L.inPins[0], L.outPins[0], IM_COL32(200, 70, 70, 200), std::max(1.5f, 2.0f * z));
    }
    dl->AddRect(L.min, L.max, selected ? theme::col(theme::Selection) : theme::col(theme::NodeOutline), round, 0,
                selected ? 2.0f : 1.0f);
    if (showTimings && showText)
        if (auto t = timings_.find(n.id); t != timings_.end()) {
            // Above the node like Blender's overlay; slow nodes stand out in amber.
            char buf[32];
            const auto g = gpuNodes_.find(n.id);
            const bool onGpu = g != gpuNodes_.end() && g->second;
            std::snprintf(buf, sizeof buf, t->second < 10.0 ? "%.1f ms%s" : "%.0f ms%s", t->second, onGpu ? "  GPU" : "");
            const float tfs = fs * 0.85f;
            dl->AddText(ImGui::GetFont(), tfs, ImVec2(L.min.x + 4 * z, L.min.y - tfs - 3 * z),
                        t->second >= 50.0 ? IM_COL32(236, 170, 80, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled), buf);
        }

    // Only the topmost node under the mouse gets interactive widgets, so overlapping nodes behave.
    const bool ownsMouse = hitNode(g, ImGui::GetIO().MousePos) == n.id;
    ImGui::PushID(n.id);

    auto drawPin = [&](ImVec2 p, PinType t, bool hovered) {
        float pr = std::max(3.0f, kPinR * z) * (hovered ? 1.4f : 1.0f);
        if (t == PinType::Image) {
            dl->AddRectFilled(ImVec2(p.x - pr, p.y - pr), ImVec2(p.x + pr, p.y + pr), pinColor(t), 1.5f);
            dl->AddRect(ImVec2(p.x - pr, p.y - pr), ImVec2(p.x + pr, p.y + pr), theme::col(theme::NodeOutline), 1.5f);
        } else {
            dl->AddCircleFilled(p, pr, pinColor(t));
            dl->AddCircle(p, pr, theme::col(theme::NodeOutline));
        }
    };
    const ImU32 labelCol = theme::col(theme::LabelText);
    if (bar) {
        // A reroute's pins take the colour of the wire coming in.
        auto pinType = [&](PinType t) {
            if (isReroute(n))
                if (const Link* l = g.inputLink(n.id, 0))
                    if (const Node* from = g.find(l->fromNode)) return from->info().outputs[size_t(l->fromPin)].type;
            return t;
        };
        for (int i = 0; i < int(info.outputs.size()); ++i) drawPin(L.outPins[i], pinType(info.outputs[i].type), false);
        for (int i = 0; i < int(info.inputs.size()); ++i) drawPin(L.inPins[i], pinType(info.inputs[i].type), false);
        ImGui::PopID();
        return false;
    }

    for (int i = 0; i < int(info.outputs.size()); ++i) {
        ImVec2 p = L.outPins[i];
        if (showText) {
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, info.outputs[i].name.c_str());
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x - 12 * z - ts.x, p.y - fs * 0.5f), labelCol,
                        info.outputs[i].name.c_str(), nullptr, 0.0f, &clip);
        }
        bool hov = hoverPin_.node == n.id && hoverPin_.output && hoverPin_.pin == i;
        drawPin(p, info.outputs[i].type, hov);
        if (n.id == preview && i == previewPin_ && info.outputs.size() > 1)
            dl->AddCircle(p, std::max(6.0f, 9.0f * z), IM_COL32(236, 150, 50, 255), 0, 2.0f);
    }

    for (int i = 0; i < int(info.params.size()); ++i) {
        const ImRect& box = L.paramBoxes[i];
        if (box.GetWidth() <= 0) continue;
        changed |= drawParamRow(dl, n, i, box, interactive && ownsMouse);
    }

    for (int i = 0; i < int(info.inputs.size()); ++i) {
        ImVec2 p = L.inPins[i];
        const bool linked = g.inputLink(n.id, i) != nullptr;
        const ImRect& box = L.valueBoxes[i];
        const int fp = info.inputs[i].fallbackParam;
        const char* name = info.inputs[i].name.c_str();
        if (!linked && box.GetWidth() > 0) {
            // Unconnected input with a value: show it as an editable field (label inside).
            const bool mine = (editing_.node == n.id && editing_.param == fp) ||
                              (activeNode_ == n.id && activeParam_ == fp);
            if (interactive && (ownsMouse || mine)) changed |= drawValueBox(n, fp, box, dl, name);
            else drawValueField(dl, box, name, n.paramF(fp), info.params[fp], fs, z, false);
        } else if (showText) {
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + 12 * z, p.y - fs * 0.5f), labelCol, name, nullptr, 0.0f, &clip);
        }
        bool hov = hoverPin_.node == n.id && !hoverPin_.output && hoverPin_.pin == i;
        drawPin(p, info.inputs[i].type, hov);
    }

    ImGui::PopID();
    if (changed) r.evalChanged = r.docChanged = true;
    return changed;
}

// ---------------------------------------------------------------- gestures

void NodeEditor::updateInsertCandidate(const Graph& g) {
    insertLink_ = 0;
    if (selection_.size() != 1) return;
    const int id = *selection_.begin();
    const Node* n = g.find(id);
    if (!n || n->info().inputs.empty() || n->info().outputs.empty()) return;
    for (const Link& l : g.links())
        if (l.fromNode == id || l.toNode == id) return;  // only free-floating nodes get spliced in

    Layout L = layoutFor(*n);
    ImRect rect(L.min, L.max);
    rect.Expand(4.0f);
    const ImVec2 center = rect.GetCenter();
    float best = 1e30f;
    for (const Link& l : g.links()) {
        PinType fromT = g.find(l.fromNode)->info().outputs[l.fromPin].type;
        PinType toT = g.find(l.toNode)->info().inputs[l.toPin].type;
        int in = pickPin(g, *n, true, fromT);
        int out = pickPin(g, *n, false, toT);
        if (in < 0 || out < 0) continue;
        ImVec2 a, b, c1, c2;
        linkEnds(g, l, a, b);
        bezierPoints(a, b, zoom_, c1, c2);
        for (int s = 0; s <= 24; ++s) {
            ImVec2 p = ImBezierCubicCalc(a, c1, c2, b, s / 24.0f);
            if (!rect.Contains(p)) continue;
            float d = ImLengthSqr(p - center);
            if (d < best) {
                best = d;
                insertLink_ = l.id;
                insertIn_ = in;
                insertOut_ = out;
            }
        }
    }
}

void NodeEditor::finishLinkDrag(Graph& g, Result& r) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto connect = [&](PinRef a, PinRef b) {
        if (a.output == b.output) return;
        const PinRef& o = a.output ? a : b;
        const PinRef& i = a.output ? b : a;
        if (g.connect(o.node, o.pin, i.node, i.pin)) r.evalChanged = r.docChanged = true;
    };
    const PinRef target = hitPin(g, mouse);
    const int targetNode = hitNode(g, mouse);
    const Node* from = g.find(linkFrom_.node);
    if (!from) {
    } else if (target.valid() && target.node != linkFrom_.node) {
        connect(linkFrom_, target);
    } else if (targetNode && targetNode != linkFrom_.node) {
        // Dropped on a node body: use its best matching pin.
        const Node* tn = g.find(targetNode);
        PinType t = linkFrom_.output ? from->info().outputs[linkFrom_.pin].type : from->info().inputs[linkFrom_.pin].type;
        int pin = pickPin(g, *tn, linkFrom_.output, t);
        if (pin >= 0) connect(linkFrom_, PinRef{targetNode, pin, !linkFrom_.output});
    } else if (!targetNode && !linkDetached_) {
        // Dropped on empty canvas: offer the add menu; the new node gets connected.
        menuConnect_ = linkFrom_;
        menuPos_ = mouse;
        search_[0] = '\0';
        swapTargets_.clear();
        ImGui::OpenPopup("AddNode");
    }
    linkDetached_ = false;
    hoverPin_ = {};
}

bool NodeEditor::deleteSelection(Graph& g, int& preview, bool reconnect) {
    bool any = false;
    if (selectedLink_) {
        g.removeLink(selectedLink_);
        selectedLink_ = 0;
        any = true;
    }
    for (int id : selection_) {
        // Deleting a node in the middle of a chain joins its neighbours (one node at a time, so
        // deleting several consecutive nodes still leaves the chain connected).
        if (reconnect) g.bridgeNode(id);
        g.removeNode(id);
        if (preview == id) preview = 0;
        any = true;
    }
    selection_.clear();
    return any;
}

bool NodeEditor::duplicateSelection(Graph& g) {
    if (selection_.empty()) return false;
    std::map<int, int> remap;
    for (int id : selection_)
        if (Node* copy = g.duplicateNode(id, 30.0f, 30.0f)) remap[id] = copy->id;
    std::vector<Link> internal;
    for (const Link& l : g.links())
        if (remap.count(l.fromNode) && remap.count(l.toNode)) internal.push_back(l);
    for (const Link& l : internal) g.connect(remap[l.fromNode], l.fromPin, remap[l.toNode], l.toPin);
    selection_.clear();
    for (auto& [oldId, newId] : remap) selection_.insert(newId);
    selectedLink_ = 0;
    return true;
}

// ---------------------------------------------------------------- main entry

NodeEditor::Result NodeEditor::draw(Graph& g, int& selected, int& preview, int& previewPin) {
    Result r;
    ImGuiIO& io = ImGui::GetIO();
    previewPin_ = previewPin;
    valueHovered_ = false;
    origin_ = ImGui::GetCursorScreenPos();
    size_ = ImGui::GetContentRegionAvail();
    size_.x = std::max(size_.x, 1.0f);
    size_.y = std::max(size_.y, 1.0f);
    syncOrder(g);
    if (frameSelectionNext_) {
        frameSelectionNext_ = false;
        fitFrames_ = 0;
        frameSelected(g);
    }
    if (fitFrames_ > 0) {
        doFrame(g);
        --fitFrames_;
    }

    // Background item catches clicks that no node widget takes.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", size_,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool bgHovered = ImGui::IsItemHovered();
    const bool bgActivated = ImGui::IsItemActivated();
    const ImVec2 mouse = io.MousePos;
    const ImRect canvas(origin_, ImVec2(origin_.x + size_.x, origin_.y + size_.y));
    const bool mouseInCanvas =
        canvas.Contains(mouse) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(canvas.Min, canvas.Max, true);
    dl->AddRectFilled(canvas.Min, canvas.Max, theme::col(theme::Canvas));
    drawGrid(dl);
    drawFrames(dl, g);
    drawLinks(dl, g);
    for (int id : std::vector<int>(order_))
        if (Node* n = g.find(id)) drawNode(dl, g, *n, preview, r);

    // Insert candidate is re-drawn on top so it stays visible under the dragged node.
    if (insertLink_) {
        for (const Link& l : g.links()) {
            if (l.id != insertLink_) continue;
            ImVec2 a, b, c1, c2;
            linkEnds(g, l, a, b);
            bezierPoints(a, b, zoom_, c1, c2);
            dl->AddBezierCubic(a, c1, c2, b, IM_COL32(255, 255, 255, 150), std::max(1.5f, 2.5f * zoom_));
        }
    }

    // Wire being dragged
    if (mode_ == Mode::DragLink) {
        if (const Node* from = g.find(linkFrom_.node)) {
            Layout L = layoutFor(*from);
            ImVec2 p = linkFrom_.output ? L.outPins[linkFrom_.pin] : L.inPins[linkFrom_.pin];
            PinType t = linkFrom_.output ? from->info().outputs[linkFrom_.pin].type : from->info().inputs[linkFrom_.pin].type;
            ImVec2 a = linkFrom_.output ? p : mouse, b = linkFrom_.output ? mouse : p, c1, c2;
            bezierPoints(a, b, zoom_, c1, c2);
            dl->AddBezierCubic(a, c1, c2, b, pinColor(t), std::max(1.5f, 2.5f * zoom_));
        }
    }
    if ((mode_ == Mode::Knife || mode_ == Mode::RerouteCut) && knife_.size() > 1) {
        ImU32 col = mode_ == Mode::Knife ? IM_COL32(230, 80, 80, 230) : IM_COL32(120, 200, 120, 230);
        dl->AddPolyline(knife_.data(), int(knife_.size()), col, 0, 2.0f);
    }
    if (mode_ == Mode::BoxSelect) {
        ImRect box(ImMin(pressPos_, mouse), ImMax(pressPos_, mouse));
        dl->AddRectFilled(box.Min, box.Max, IM_COL32(100, 140, 220, 40));
        dl->AddRect(box.Min, box.Max, IM_COL32(100, 140, 220, 200));
    }
    if (g.nodes().empty()) {
        const char* hint = "Right-click to add a node";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(canvas.GetCenter().x - ts.x * 0.5f, canvas.GetCenter().y), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    }
    dl->PopClipRect();

    // ---- grab (G / Shift+D): selection follows the mouse until a click
    if (mode_ == Mode::Grab) {
        ImVec2 delta((mouse.x - pressPos_.x) / zoom_, (mouse.y - pressPos_.y) / zoom_);
        for (auto& [id, start] : dragStart_)
            if (Node* n = g.find(id)) {
                n->x = std::round(start.x + delta.x);
                n->y = std::round(start.y + delta.y);
            }
        r.docChanged = true;
        updateInsertCandidate(g);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            finishDragNodes(g, r);
            mode_ = Mode::None;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            for (auto& [id, start] : dragStart_)
                if (Node* n = g.find(id)) n->x = start.x, n->y = start.y;
            insertLink_ = 0;
            mode_ = Mode::None;
        }
    }

    // ---- press
    if (mode_ == Mode::Grab) {
        // handled above
    } else if (bgActivated && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pressPos_ = mouse;
        editing_ = {};
        const PinRef pin = hitPin(g, mouse);
        const int nid = pin.valid() ? 0 : hitNode(g, mouse);
        if (pin.valid()) {
            linkDetached_ = false;
            linkFrom_ = pin;
            if (!pin.output) {
                // Dragging off a connected input picks up its wire (drop elsewhere = disconnect).
                if (const Link* l = g.inputLink(pin.node, pin.pin)) {
                    linkFrom_ = PinRef{l->fromNode, l->fromPin, true};
                    g.removeLink(l->id);
                    linkDetached_ = true;
                    r.evalChanged = r.docChanged = true;
                }
            }
            mode_ = Mode::DragLink;
        } else if (nid) {
            order_.erase(std::find(order_.begin(), order_.end(), nid));
            order_.push_back(nid);
            selectedLink_ = 0;
            selectedFrame_ = 0;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && dynamic_cast<GroupNode*>(g.find(nid))) {
                r.enterGroup = nid;
                mode_ = Mode::None;
            } else if (io.KeyCtrl && io.KeyShift) {
                // Cycle through the node's outputs in the preview (Blender's Ctrl+Shift+click).
                const int nOut = std::max<int>(1, int(g.find(nid)->info().outputs.size()));
                if (preview == nid) {
                    previewPin = (previewPin + 1) % nOut;
                } else {
                    preview = nid;
                    previewPin = 0;
                }
                r.previewChanged = true;
                mode_ = Mode::None;
            } else if (io.KeyCtrl) {
                preview = (preview == nid) ? 0 : nid;
                previewPin = 0;
                r.previewChanged = true;
                mode_ = Mode::None;
            } else {
                if (io.KeyShift) {
                    if (!selection_.erase(nid)) selection_.insert(nid);
                } else if (!selection_.count(nid)) {
                    selection_ = {nid};
                }
                pressNode_ = nid;
                dragStart_.clear();
                for (int id : selection_) dragStart_[id] = ImVec2(g.find(id)->x, g.find(id)->y);
                mode_ = Mode::PressNode;
            }
        } else if (int fc = hitFrameCorner(g, mouse)) {
            Frame* f = g.findFrame(fc);
            selectedFrame_ = fc;
            selection_.clear();
            frameStart_[0] = f->x, frameStart_[1] = f->y, frameStart_[2] = f->w, frameStart_[3] = f->h;
            mode_ = Mode::ResizeFrame;
        } else if (int ft = hitFrameTitle(g, mouse)) {
            Frame* f = g.findFrame(ft);
            selectedFrame_ = ft;
            selection_.clear();
            selectedLink_ = 0;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                menuFrame_ = ft;
                std::snprintf(frameLabel_, sizeof(frameLabel_), "%s", f->label.c_str());
                ImGui::OpenPopup("FrameRename");
                mode_ = Mode::None;
            } else {
                // Nodes whose center lies inside the frame travel with it.
                frameStart_[0] = f->x, frameStart_[1] = f->y, frameStart_[2] = f->w, frameStart_[3] = f->h;
                frameNodes_.clear();
                for (const auto& [id, n] : g.nodes()) {
                    float cx = n->x + kNodeW * 0.5f, cy = n->y + nodeHeightGrid(*n) * 0.5f;
                    if (cx > f->x && cx < f->x + f->w && cy > f->y && cy < f->y + f->h) frameNodes_[id] = ImVec2(n->x, n->y);
                }
                mode_ = Mode::DragFrame;
            }
        } else if (int lid = hitLink(g, mouse)) {
            selectedLink_ = lid;
            selection_.clear();
            selectedFrame_ = 0;
            mode_ = Mode::None;
        } else {
            mode_ = io.KeyShift ? Mode::BoxSelect : Mode::Pan;
        }
    } else if (bgActivated && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        pressPos_ = mouse;
        mode_ = Mode::Pan;
    } else if (bgHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mode_ == Mode::None && (io.KeyCtrl || io.KeyShift)) {
        knife_ = {mouse};
        mode_ = io.KeyCtrl ? Mode::Knife : Mode::RerouteCut;
    } else if (bgHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mode_ == Mode::None) {
        menuPos_ = mouse;
        search_[0] = '\0';
        menuConnect_ = {};
        if (int nid = hitNode(g, mouse)) {
            menuNode_ = nid;
            if (!selection_.count(nid)) selection_ = {nid};
            ImGui::OpenPopup("NodeMenu");
        } else if (int ft = hitFrameTitle(g, mouse)) {
            menuFrame_ = ft;
            selectedFrame_ = ft;
            ImGui::OpenPopup("FrameMenu");
        } else {
            swapTargets_.clear();
            ImGui::OpenPopup("AddNode");
        }
    }

    // ---- ongoing gestures
    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    switch (mode_) {
        case Mode::Pan:
            pan_.x += io.MouseDelta.x;
            pan_.y += io.MouseDelta.y;
            if (!leftDown && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                // A click (no drag) on empty canvas clears the selection.
                if (ImLengthSqr(mouse - pressPos_) < 9.0f && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    selection_.clear();
                    selectedLink_ = 0;
                    selectedFrame_ = 0;
                }
                mode_ = Mode::None;
            }
            break;
        case Mode::BoxSelect:
            if (!leftDown) {
                ImRect box(ImMin(pressPos_, mouse), ImMax(pressPos_, mouse));
                selection_.clear();
                for (const auto& [id, n] : g.nodes()) {
                    Layout L = layoutFor(*n);
                    if (box.Overlaps(ImRect(L.min, L.max))) selection_.insert(id);
                }
                mode_ = Mode::None;
            }
            break;
        case Mode::PressNode:
            if (!leftDown) {
                if (!io.KeyShift) selection_ = {pressNode_};  // plain click on a node in a group selects just it
                mode_ = Mode::None;
            } else if (ImLengthSqr(mouse - pressPos_) > 9.0f) {
                mode_ = Mode::DragNodes;
                if (io.KeyAlt) {
                    // Alt+drag pulls the nodes out of their chain and closes the gap.
                    for (int id : selection_) g.bridgeNode(id);
                    r.evalChanged = r.docChanged = true;
                }
            }
            break;
        case Mode::DragNodes: {
            ImVec2 delta((mouse.x - pressPos_.x) / zoom_, (mouse.y - pressPos_.y) / zoom_);
            for (auto& [id, start] : dragStart_)
                if (Node* n = g.find(id)) {
                    n->x = std::round(start.x + delta.x);
                    n->y = std::round(start.y + delta.y);
                }
            r.docChanged = true;
            updateInsertCandidate(g);
            if (!leftDown) {
                finishDragNodes(g, r);
                mode_ = Mode::None;
            }
            break;
        }
        case Mode::Knife:
        case Mode::RerouteCut: {
            if (ImLengthSqr(mouse - knife_.back()) > 16.0f) knife_.push_back(mouse);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                // Every wire crossing the stroke is cut, or gets a reroute dot at the crossing.
                auto cross = [](ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImVec2& hit) {
                    float den = (b.x - a.x) * (d.y - c.y) - (b.y - a.y) * (d.x - c.x);
                    if (std::fabs(den) < 1e-6f) return false;
                    float t = ((c.x - a.x) * (d.y - c.y) - (c.y - a.y) * (d.x - c.x)) / den;
                    float u = ((c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x)) / den;
                    if (t < 0 || t > 1 || u < 0 || u > 1) return false;
                    hit = ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
                    return true;
                };
                std::vector<std::pair<Link, ImVec2>> hits;
                for (const Link& l : g.links()) {
                    ImVec2 a, b, c1, c2, prev, hit;
                    linkEnds(g, l, a, b);
                    bezierPoints(a, b, zoom_, c1, c2);
                    bool found = false;
                    prev = a;
                    for (int s = 1; s <= 24 && !found; ++s) {
                        ImVec2 p = ImBezierCubicCalc(a, c1, c2, b, s / 24.0f);
                        for (size_t k = 1; k < knife_.size() && !found; ++k) found = cross(prev, p, knife_[k - 1], knife_[k], hit);
                        prev = p;
                    }
                    if (found) hits.push_back({l, hit});
                }
                for (auto& [l, hit] : hits) {
                    g.removeLink(l.id);
                    if (mode_ == Mode::RerouteCut) {
                        ImVec2 gp = toGrid(hit);
                        if (Node* rr = g.addNode("util.reroute", std::round(gp.x - kRerouteW * 0.5f), std::round(gp.y - kTitleH * 0.5f))) {
                            g.connect(l.fromNode, l.fromPin, rr->id, 0);
                            g.connect(rr->id, 0, l.toNode, l.toPin);
                        }
                    }
                }
                if (!hits.empty()) r.evalChanged = r.docChanged = true;
                knife_.clear();
                mode_ = Mode::None;
            }
            break;
        }
        case Mode::Grab: break;
        case Mode::DragLink:
            hoverPin_ = hitPin(g, mouse);
            if (!leftDown) {
                finishLinkDrag(g, r);
                mode_ = Mode::None;
            }
            break;
        case Mode::DragFrame:
        case Mode::ResizeFrame: {
            Frame* f = g.findFrame(selectedFrame_);
            if (!f) {
                mode_ = Mode::None;
                break;
            }
            ImVec2 d((mouse.x - pressPos_.x) / zoom_, (mouse.y - pressPos_.y) / zoom_);
            if (mode_ == Mode::DragFrame) {
                f->x = std::round(frameStart_[0] + d.x);
                f->y = std::round(frameStart_[1] + d.y);
                for (auto& [id, start] : frameNodes_)
                    if (Node* n = g.find(id)) {
                        n->x = std::round(start.x + d.x);
                        n->y = std::round(start.y + d.y);
                    }
            } else {
                f->w = std::max(120.0f, std::round(frameStart_[2] + d.x));
                f->h = std::max(80.0f, std::round(frameStart_[3] + d.y));
            }
            r.docChanged = true;
            if (!leftDown) mode_ = Mode::None;
            break;
        }
        case Mode::None: break;
    }

    // ---- zoom around the cursor
    if (mouseInCanvas && io.MouseWheel != 0.0f && !ImGui::IsAnyItemActive()) {
        ImVec2 gp = toGrid(mouse);
        zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), kMinZoom, kMaxZoom);
        pan_ = ImVec2(mouse.x - origin_.x - gp.x * zoom_, mouse.y - origin_.y - gp.y * zoom_);
    }

    // ---- keyboard
    // Blender's add menu, opened at the mouse like a right-click on empty canvas. Like Blender it
    // goes to the editor under the mouse, so it also works before the canvas has been clicked.
    if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) &&
        !io.WantTextInput && mode_ == Mode::None && ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_A)) {
        menuPos_ = ImGui::GetMousePos();
        search_[0] = 0;
        menuConnect_ = {};
        swapTargets_.clear();
        ImGui::OpenPopup("AddNode");
    }
    // Swap the selected nodes for another type (Blender's Shift+S), using the add menu to pick it.
    // Hover is enough, like Shift+A, since the pointer is usually over the node being swapped.
    if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) &&
        !io.WantTextInput && mode_ == Mode::None &&
        ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_S))
        openSwapMenu(g);
    // Find a node by label or name; hover is enough here too.
    if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) &&
        !io.WantTextInput && mode_ == Mode::None && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
        findRequested_ = true;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput) {
        const bool noMods = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !valueHovered_) ||
            (ImGui::IsKeyPressed(ImGuiKey_X) && noMods)) {
            if (selectedFrame_) {
                g.removeFrame(selectedFrame_);  // the frame only; its nodes stay
                selectedFrame_ = 0;
                r.docChanged = true;
            } else if (deleteSelection(g, preview, !io.KeyAlt)) {  // Alt: delete without reconnecting
                r.evalChanged = r.docChanged = true;
            }
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copySelection(g);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V) && paste(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_M) && noMods && toggleMute(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_H) && noMods && toggleCollapse(g)) r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_G) && noMods && !selection_.empty()) beginGrab(g);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_D) && duplicateSelection(g)) {
            beginGrab(g);
            r.evalChanged = r.docChanged = true;
        }
        if (cycle_.from && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                            ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
            cycle_ = {};
        if (ImGui::IsKeyPressed(ImGuiKey_F) && noMods) {
            if (cycle_.from ? cycleLink(g, 0) : makeLinks(g)) r.evalChanged = r.docChanged = true;
        }
        if (cycle_.from && noMods) {
            if ((ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1)) && cycleLink(g, 1))
                r.evalChanged = r.docChanged = true;
            if ((ImGui::IsKeyPressed(ImGuiKey_2) || ImGui::IsKeyPressed(ImGuiKey_Keypad2)) && cycleLink(g, 2))
                r.evalChanged = r.docChanged = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_D) && detachLinks(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_S) && swapLinks(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_L) && !io.KeyCtrl && !io.KeyAlt) selectLinked(g, io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) fitFrames_ = 1;
        if ((ImGui::IsKeyPressed(ImGuiKey_Period) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal)) && noMods) frameSelected(g);
        if (ImGui::IsKeyPressed(ImGuiKey_F2) && selection_.size() == 1) {
            renameNode_ = *selection_.begin();
            std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", g.find(renameNode_)->label.c_str());
            ImGui::OpenPopup("NodeRename");
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G) && groupSelection(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_G) && ungroupSelection(g))
            r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_J) && frameSelection(g)) r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_P) && moveSelectionToFrame(g, 0)) r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_P) && arrange(g)) r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Tab) && !io.KeyCtrl) {
            if (int gid = selectedGroup(g)) r.enterGroup = gid;
            else r.exitGroup = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D) && duplicateSelection(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
            for (const auto& [id, n] : g.nodes()) selection_.insert(id);
    }

    drawAddMenu(g, r);
    drawNodeMenu(g, preview, r);
    // A value box's right-click menu (opened and drawn here, in one ID scope).
    if (openValueMenu_) {
        ImGui::OpenPopup("ValueMenu");
        openValueMenu_ = false;
    }
    if (ImGui::BeginPopup("ValueMenu")) {
        Node* vn = g.find(valueMenuNode_);
        if (!vn || valueMenuParam_ < 0 || valueMenuParam_ >= int(vn->params.size())) {
            ImGui::CloseCurrentPopup();
        } else {
            if (ImGui::MenuItem("Reset to Default", "Backspace")) {
                vn->resetParam(valueMenuParam_);
                r.evalChanged = r.docChanged = true;
            }
            if (ImGui::MenuItem("Edit Value", "Click")) editing_ = EditState{vn->id, valueMenuParam_, 0, false};
        }
        ImGui::EndPopup();
    }
    drawFrameMenu(g, r);
    drawRenamePopup(g, r);
    drawPresetPopup(g);
    if (findRequested_) {
        findRequested_ = false;
        search_[0] = 0;
        searchSel_ = 0;
        menuPos_ = ImGui::GetMousePos();
        ImGui::OpenPopup("FindNode");
    }
    drawFindMenu(g, r);

    // F's wire stays open to 1 / 2 / F while the same nodes are selected and it still exists.
    if (cycle_.from) {
        const bool alive = std::any_of(g.links().begin(), g.links().end(), [&](const Link& l) {
            return l.fromNode == cycle_.from && l.fromPin == cycle_.fromPin && l.toNode == cycle_.to && l.toPin == cycle_.toPin;
        });
        if (!alive || selection_ != cycle_.selection) cycle_ = {};
        else drawLinkCycleHint(g);
    }

    // Node widgets moved the layout cursor around; leave it at a valid spot covering the canvas.
    ImGui::SetCursorScreenPos(origin_);
    ImGui::Dummy(size_);

    selected = selection_.size() == 1 ? *selection_.begin() : 0;
    return r;
}

// ---------------------------------------------------------------- menus

void NodeEditor::openSwapMenu(const Graph& g) {
    swapTargets_.clear();
    for (int id : selection_)
        if (const Node* n = g.find(id); n && !n->info().hidden) swapTargets_.push_back(id);
    if (swapTargets_.empty()) return;
    menuPos_ = ImGui::GetMousePos();
    search_[0] = 0;
    menuConnect_ = {};
    ImGui::OpenPopup("AddNode");
}

void NodeEditor::drawAddMenu(Graph& g, Result& r) {
    // A fixed size, inside the window it opens in: a popup overhanging its window gets an OS
    // window of its own, and resizing that as the results changed per keystroke flashed black.
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const ImGuiViewport* vp = ImGui::GetWindowViewport();
        const ImVec2 size(ImGui::GetFontSize() * 20.0f, ImGui::GetFrameHeightWithSpacing() * 2 +
                                                          ImGui::GetTextLineHeightWithSpacing() * 12 + st.WindowPadding.y * 2);
        ImVec2 pos = menuPos_;
        pos.x = std::max(vp->Pos.x, std::min(pos.x, vp->Pos.x + vp->Size.x - size.x));
        pos.y = std::max(vp->Pos.y, std::min(pos.y, vp->Pos.y + vp->Size.y - size.y));
        ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(size);
    }
    if (!ImGui::BeginPopup("AddNode")) {
        menuConnect_ = {};
        swapTargets_.clear();
        return;
    }
    const bool swapping = !swapTargets_.empty();
    if (swapping) {
        if (swapTargets_.size() == 1) ImGui::TextDisabled("Swap node to:");
        else ImGui::TextDisabled("Swap %d nodes to:", int(swapTargets_.size()));
    }
    // When a wire was dropped here, only offer nodes that can accept / provide it.
    const Node* src = g.find(menuConnect_.node);
    if (!src) menuConnect_ = {};
    PinType wireType = PinType::Image;
    if (src) wireType = menuConnect_.output ? src->info().outputs[menuConnect_.pin].type
                                            : src->info().inputs[menuConnect_.pin].type;
    auto compatible = [&](const NodeInfo& inf) {
        if (!src) return true;
        const auto& pins = menuConnect_.output ? inf.inputs : inf.outputs;
        return std::any_of(pins.begin(), pins.end(), [&](const PinDesc& p) {
            return menuConnect_.output ? canConvert(wireType, p.type) : canConvert(p.type, wireType);
        });
    };

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    // Typing goes straight here (focused when the menu opens); empty search shows categories.
    if (ImGui::InputTextWithHint("##search", "Search nodes...", search_, sizeof(search_))) searchSel_ = 0;

    const auto& reg = NodeRegistry::instance();
    std::string chosen;
    if (search_[0]) {
        // Name matches first, then category matches.
        std::vector<std::string> results;
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& type : reg.types()) {
                const NodeInfo* inf = reg.find(type);
                if (inf->hidden || !compatible(*inf)) continue;
                bool nameHit = containsNoCase(inf->displayName, search_);
                const bool menuHit = containsNoCase(nodemenu::menuOf(type), search_) || containsNoCase(inf->category, search_);
                if (pass == 0 ? nameHit : (!nameHit && menuHit)) results.push_back(type);
            }
        const int n = int(results.size());
        bool moved = false;
        if (n > 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) searchSel_ = (searchSel_ + 1) % n, moved = true;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) searchSel_ = (searchSel_ + n - 1) % n, moved = true;
        }
        searchSel_ = n ? std::clamp(searchSel_, 0, n - 1) : 0;
        if (n == 0) ImGui::TextDisabled("No matching nodes");
        ImGui::BeginChild("##results", ImVec2(0, 0), ImGuiChildFlags_None);
        for (int i = 0; i < n; ++i) {
            const NodeInfo* inf = reg.find(results[i]);
            ImGui::PushID(i);
            if (ImGui::Selectable(inf->displayName.c_str(), i == searchSel_)) chosen = results[i];
            if (i == searchSel_ && moved) ImGui::SetScrollHereY();
            ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
            ImGui::TextDisabled("%s", nodemenu::menuOf(results[i]).c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (chosen.empty() && n > 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
            chosen = results[searchSel_];
    } else {
        // The wheel steps a highlight through the open submenu's nodes, over the category name or
        // the list itself (scrolling it along if it runs off screen); click or Enter adds it.
        ImGuiWindow* menuWin = ImGui::GetCurrentWindow();
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowAppearing()) wheelMenu_.clear(), wheelSel_ = -1, presetNames_ = presets::list();
        for (const auto& m : nodemenu::menus()) {
            std::vector<std::string> shown;
            for (const auto& t : m.items)
                if (!t.empty() && compatible(*reg.find(t))) shown.push_back(t);
            if (shown.empty() || !ImGui::BeginMenu(m.name.c_str())) continue;
            ImGuiWindow* sub = ImGui::GetCurrentWindow();
            if (wheelMenu_ != m.name) wheelMenu_ = m.name, wheelSel_ = -1;
            const int n = int(shown.size());
            int step = 0;
            if (io.MouseWheel != 0 && (GImGui->HoveredWindow == menuWin || GImGui->HoveredWindow == sub))
                step = io.MouseWheel < 0 ? 1 : -1;
            if (step) wheelSel_ = wheelSel_ < 0 ? (step > 0 ? 0 : n - 1) : std::clamp(wheelSel_ + step, 0, n - 1);
            // A separator only between two shown sections, as some can be empty for a dropped wire.
            bool pendingSep = false;
            int row = 0;
            for (const auto& type : m.items) {
                if (type.empty()) {
                    pendingSep = row > 0;
                    continue;
                }
                const NodeInfo* inf = reg.find(type);
                if (!compatible(*inf)) continue;
                if (pendingSep) ImGui::Separator();
                pendingSep = false;
                if (ImGui::Selectable(inf->displayName.c_str(), row == wheelSel_)) chosen = type;
                // Moving the mouse onto a row makes it the one the wheel steps from.
                if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) wheelSel_ = row;
                if (step && row == wheelSel_) ImGui::SetScrollHereY();
                ++row;
            }
            if (chosen.empty() && wheelSel_ >= 0 &&
                (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
                chosen = shown[wheelSel_];
            ImGui::EndMenu();
        }
        // Inside a group: a single group input or output socket as a node of its own.
        if (insideGroup && !swapping) {
            const NodeInfo& vi = GroupValueInputNode::staticInfo();
            const NodeInfo& vo = GroupValueOutputNode::staticInfo();
            if ((compatible(vi) || compatible(vo)) && ImGui::BeginMenu("Group")) {
                if (compatible(vi) && ImGui::MenuItem(vi.displayName.c_str())) chosen = vi.type;
                if (compatible(vo) && ImGui::MenuItem(vo.displayName.c_str())) chosen = vo.type;
                ImGui::EndMenu();
            }
        }
        if (!src && !swapping && !presetNames_.empty() && ImGui::BeginMenu("Presets")) {
            std::string remove;
            for (const auto& name : presetNames_) {
                if (!ImGui::MenuItem(name.c_str())) continue;
                const nlohmann::json clip = presets::load(name);
                if (!clip.is_null() && insertClip(g, clip, toGrid(menuPos_))) r.evalChanged = r.docChanged = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Delete Preset")) {
                for (const auto& name : presetNames_)
                    if (ImGui::MenuItem(name.c_str())) remove = name;
                ImGui::EndMenu();
            }
            if (!remove.empty()) {
                presets::remove(remove);
                presetNames_ = presets::list();
            }
            ImGui::EndMenu();
        }
        if (!src && !swapping) {
            ImGui::Separator();
            if (ImGui::MenuItem("Frame", "Ctrl+J")) {
                ImVec2 gp = toGrid(menuPos_);
                g.addFrame(std::round(gp.x), std::round(gp.y), 360, 240);
                r.docChanged = true;
                ImGui::CloseCurrentPopup();
            }
        }
    }

    if (!chosen.empty() && swapping) {
        for (int id : swapTargets_) g.swapNode(id, chosen);
        // The new type may be taller; nudge neighbours out of the way.
        spaceOut(g, std::set<int>(swapTargets_.begin(), swapTargets_.end()), gridSize);
        swapTargets_.clear();
        r.evalChanged = r.docChanged = true;
        ImGui::CloseCurrentPopup();
    } else if (!chosen.empty()) {
        if (Node* n = g.addNode(chosen)) {
            placeAtScreen(*n, menuPos_);
            if (src) {
                int pin = pickPin(g, *n, menuConnect_.output, wireType);
                if (pin >= 0) {
                    if (menuConnect_.output) g.connect(menuConnect_.node, menuConnect_.pin, n->id, pin);
                    else g.connect(n->id, pin, menuConnect_.node, menuConnect_.pin);
                }
            }
            spaceOut(g, {n->id}, gridSize);  // make room rather than landing on top of other nodes
            select(n->id);
            r.evalChanged = r.docChanged = true;
        }
        menuConnect_ = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void NodeEditor::drawNodeMenu(Graph& g, int& preview, Result& r) {
    if (!ImGui::BeginPopup("NodeMenu")) return;
    const Node* mn0 = g.find(menuNode_);
    bool openRename = false, openSwap = false;
    if (ImGui::MenuItem("Duplicate", "Ctrl+D") && duplicateSelection(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem(preview == menuNode_ ? "Stop Previewing" : "Preview", "Ctrl+Click")) {
        preview = (preview == menuNode_) ? 0 : menuNode_;
        r.previewChanged = true;
    }
    if (ImGui::MenuItem("Open in New Viewer")) r.openViewer = menuNode_;
    if (ImGui::MenuItem("Guide", "F1") && mn0) openGuide(mn0->info().displayName);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy", "Ctrl+C")) copySelection(g);
    Node* mn = g.find(menuNode_);
    if (ImGui::MenuItem("Mute", "M", mn && mn->muted) && toggleMute(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Collapse", "H", mn && mn->collapsed) && toggleCollapse(g)) r.docChanged = true;
    if (ImGui::MenuItem("Reset to Defaults", nullptr, false, mn && !mn->info().params.empty()) && resetSelection(g))
        r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Rename...", "F2") && mn) {
        renameNode_ = menuNode_;
        std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", mn->label.c_str());
        openRename = true;
    }
    if (ImGui::MenuItem("Make Links", "F", false, selection_.size() > 1) && makeLinks(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Detach Links", "Alt+D") && detachLinks(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Swap Links", "Alt+S", false, selection_.size() <= 2) && swapLinks(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::BeginMenu("Mask Selected Nodes")) {
        // Their edit applies only where a new mask is white: a Mix after them blends it with
        // what came in, so it works with any nodes (not just Basic's Factor).
        for (int k = 0; k < recipes::kMaskKinds; ++k)
            if (ImGui::MenuItem(recipes::kMaskKindNames[k])) {
                r.maskSelection = k;
                r.maskNodes.assign(selection_.begin(), selection_.end());
            }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Swap...", "Shift+S", false, mn && !mn->info().hidden)) openSwap = true;
    if (ImGui::MenuItem(selection_.size() > 1 ? "Arrange Selected" : "Arrange All", "Shift+P") && arrange(g))
        r.docChanged = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Group", "Ctrl+G") && groupSelection(g)) r.evalChanged = r.docChanged = true;
    const bool isGroup = dynamic_cast<GroupNode*>(g.find(menuNode_)) != nullptr;
    if (ImGui::MenuItem("Ungroup", "Ctrl+Alt+G", false, isGroup) && ungroupSelection(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Edit Group", "Tab", false, isGroup)) r.enterGroup = menuNode_;
    bool openPreset = false;
    if (ImGui::MenuItem("Save as Preset...")) {
        // Named after the group (or the node's label or type) it is most likely to be.
        const Node* pn = g.find(menuNode_);
        const auto* pg = dynamic_cast<const GroupNode*>(pn);
        std::snprintf(presetName_, sizeof(presetName_), "%s",
                      pg ? pg->name.c_str() : pn ? (pn->label.empty() ? pn->info().displayName : pn->label).c_str() : "");
        presetStatus_.clear();
        openPreset = true;
    }
    if (ImGui::MenuItem("Frame Selection", "Ctrl+J") && frameSelection(g)) r.docChanged = true;
    if (ImGui::BeginMenu("Move to Frame", !g.frames().empty() || (mn && frameOf(g, *mn)))) {
        const int current = mn ? frameOf(g, *mn) : 0;
        for (const Frame& f : g.frames()) {
            ImGui::PushID(f.id);
            if (ImGui::MenuItem(f.label.c_str(), nullptr, f.id == current) && moveSelectionToFrame(g, f.id))
                r.docChanged = true;
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove from Frame", "Alt+P", false, current != 0) && moveSelectionToFrame(g, 0))
            r.docChanged = true;
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete (reconnect)", "Del / X") && deleteSelection(g, preview, true)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Delete", "Alt+Del") && deleteSelection(g, preview, false)) r.evalChanged = r.docChanged = true;
    ImGui::EndPopup();
    if (openRename) ImGui::OpenPopup("NodeRename");
    if (openPreset) ImGui::OpenPopup("SavePreset");
    if (openSwap) openSwapMenu(g);
}

// ---------------------------------------------------------------- frames

void NodeEditor::drawFrames(ImDrawList* dl, const Graph& g) const {
    const float z = zoom_;
    const float fs = ImGui::GetFontSize() * z;
    for (const Frame& f : g.frames()) {
        ImVec2 a = toScreen(ImVec2(f.x, f.y)), b = toScreen(ImVec2(f.x + f.w, f.y + f.h));
        ImU32 body = ImGui::GetColorU32(ImVec4(f.color[0], f.color[1], f.color[2], 0.35f));
        ImU32 title = ImGui::GetColorU32(ImVec4(f.color[0] * 1.2f, f.color[1] * 1.2f, f.color[2] * 1.2f, 0.85f));
        dl->AddRectFilled(a, b, body, 6 * z);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + kTitleH * z), title, 6 * z, ImDrawFlags_RoundCornersTop);
        const bool sel = f.id == selectedFrame_;
        dl->AddRect(a, b, sel ? theme::col(theme::Selection) : IM_COL32(0, 0, 0, 80), 6 * z, 0, sel ? 2.0f : 1.0f);
        if (fs >= 5.0f) {
            ImVec4 clip(a.x, a.y, b.x, b.y);
            dl->AddText(ImGui::GetFont(), fs * 1.1f, ImVec2(a.x + 8 * z, a.y + (kTitleH * z - fs * 1.1f) * 0.5f),
                        IM_COL32(245, 245, 250, 255), f.label.c_str(), nullptr, 0.0f, &clip);
        }
        // resize grip
        float gs = 12 * z;
        dl->AddTriangleFilled(ImVec2(b.x - gs, b.y - 2), ImVec2(b.x - 2, b.y - gs), ImVec2(b.x - 2, b.y - 2),
                              IM_COL32(255, 255, 255, 60));
    }
}

int NodeEditor::hitFrameTitle(const Graph& g, ImVec2 p) const {
    for (auto it = g.frames().rbegin(); it != g.frames().rend(); ++it) {
        ImVec2 a = toScreen(ImVec2(it->x, it->y)), b = toScreen(ImVec2(it->x + it->w, it->y + kTitleH));
        if (ImRect(a, b).Contains(p)) return it->id;
    }
    return 0;
}

int NodeEditor::hitFrameCorner(const Graph& g, ImVec2 p) const {
    for (auto it = g.frames().rbegin(); it != g.frames().rend(); ++it) {
        ImVec2 b = toScreen(ImVec2(it->x + it->w, it->y + it->h));
        float gs = std::max(10.0f, 14 * zoom_);
        if (ImRect(ImVec2(b.x - gs, b.y - gs), b).Contains(p)) return it->id;
    }
    return 0;
}

void NodeEditor::drawFrameMenu(Graph& g, Result& r) {
    if (ImGui::BeginPopup("FrameMenu")) {
        Frame* f = g.findFrame(menuFrame_);
        if (!f) {
            ImGui::CloseCurrentPopup();
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", f->label.c_str());
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("Label", buf, sizeof(buf))) {
                f->label = buf;
                r.docChanged = true;
            }
            static const float presets[][3] = {{0.30f, 0.34f, 0.42f}, {0.45f, 0.22f, 0.22f}, {0.22f, 0.40f, 0.24f},
                                               {0.22f, 0.30f, 0.50f}, {0.46f, 0.40f, 0.18f}, {0.38f, 0.24f, 0.46f}};
            for (int k = 0; k < 6; ++k) {
                if (k) ImGui::SameLine();
                ImGui::PushID(k);
                if (ImGui::ColorButton("##preset", ImVec4(presets[k][0], presets[k][1], presets[k][2], 1.0f))) {
                    std::copy(presets[k], presets[k] + 3, f->color);
                    r.docChanged = true;
                }
                ImGui::PopID();
            }
            if (ImGui::ColorEdit3("Color", f->color, ImGuiColorEditFlags_NoInputs)) r.docChanged = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Move Selected Nodes Here", nullptr, false, !selection_.empty()) &&
                moveSelectionToFrame(g, menuFrame_))
                r.docChanged = true;
            if (ImGui::MenuItem("Fit to Contents")) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const auto& [id, n] : g.nodes()) {
                    float cx = n->x + kNodeW * 0.5f, cy = n->y + nodeHeightGrid(*n) * 0.5f;
                    if (cx < f->x || cx > f->x + f->w || cy < f->y || cy > f->y + f->h) continue;
                    x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
                    x1 = std::max(x1, n->x + kNodeW), y1 = std::max(y1, n->y + nodeHeightGrid(*n));
                }
                if (x1 > x0) {
                    f->x = x0 - 24, f->y = y0 - kTitleH - 24, f->w = x1 - x0 + 48, f->h = y1 - y0 + kTitleH + 48;
                    r.docChanged = true;
                }
            }
            if (ImGui::MenuItem("Delete Frame", "Del")) {
                g.removeFrame(menuFrame_);
                selectedFrame_ = 0;
                r.docChanged = true;
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("FrameRename")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(220);
        bool done = ImGui::InputText("##label", frameLabel_, sizeof(frameLabel_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (Frame* f = g.findFrame(menuFrame_); f && f->label != frameLabel_) {
            f->label = frameLabel_;
            r.docChanged = true;
        }
        if (done) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

bool NodeEditor::arrange(Graph& g) {
    std::set<int> ids;
    if (selection_.size() > 1) ids = selection_;
    else
        for (const auto& [id, n] : g.nodes()) ids.insert(id);
    if (!arrangeNodes(g, ids, gridSize)) return false;
    // Arranging a selection can land it on the nodes around it.
    if (ids.size() < g.nodes().size()) spaceOut(g, ids, gridSize);
    return true;
}

bool NodeEditor::frameSelection(Graph& g) {
    if (selection_.empty()) {
        ImVec2 gp = toGrid(ImGui::GetIO().MousePos);
        selectedFrame_ = g.addFrame(std::round(gp.x), std::round(gp.y), 360, 240)->id;
        return true;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int id : selection_)
        if (const Node* n = g.find(id)) {
            x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
            x1 = std::max(x1, n->x + kNodeW), y1 = std::max(y1, n->y + nodeHeightGrid(*n));
        }
    Frame* f = g.addFrame(x0 - 24, y0 - kTitleH - 24, x1 - x0 + 48, y1 - y0 + kTitleH + 48);
    selectedFrame_ = f->id;
    return true;
}

namespace {
float nodeWidthGrid(const Node& n) { return isReroute(n) ? kRerouteW : kNodeW; }
bool frameHolds(const Frame& f, const Node& n) {
    float cx = n.x + nodeWidthGrid(n) * 0.5f, cy = n.y + nodeHeightGrid(n) * 0.5f;
    return cx > f.x && cx < f.x + f.w && cy > f.y && cy < f.y + f.h;
}
}  // namespace

int NodeEditor::frameOf(const Graph& g, const Node& n) const {
    int best = 0;
    float bestArea = 0;
    for (const Frame& f : g.frames())
        if (frameHolds(f, n) && (!best || f.w * f.h < bestArea)) best = f.id, bestArea = f.w * f.h;
    return best;
}

bool NodeEditor::moveSelectionToFrame(Graph& g, int frameId) {
    std::vector<Node*> moving;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int id : selection_)
        if (Node* n = g.find(id); n && (frameId ? frameOf(g, *n) != frameId : frameOf(g, *n) != 0)) {
            moving.push_back(n);
            x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
            x1 = std::max(x1, n->x + nodeWidthGrid(*n)), y1 = std::max(y1, n->y + nodeHeightGrid(*n));
        }
    if (moving.empty()) return false;
    constexpr float kMargin = 24.0f, kGap = 40.0f;

    // Target spot for the selection's top-left corner, keeping the nodes' layout relative to each other.
    float tx, ty;
    if (frameId) {
        Frame* f = g.findFrame(frameId);
        if (!f) return false;
        // Next to whatever the frame already holds, or at its top-left when it's empty.
        float mx1 = -1e9f, my0 = 1e9f;
        for (const auto& [id, n] : g.nodes())
            if (!selection_.count(id) && frameOf(g, *n) == frameId)
                mx1 = std::max(mx1, n->x + nodeWidthGrid(*n)), my0 = std::min(my0, n->y);
        if (mx1 > -1e9f) tx = mx1 + kGap, ty = my0;
        else tx = f->x + kMargin, ty = f->y + kTitleH + kMargin;
        // Grow the frame so the nodes' centres (and bodies) end up inside it.
        f->w = std::max(f->w, tx + (x1 - x0) + kMargin - f->x);
        f->h = std::max(f->h, ty + (y1 - y0) + kMargin - f->y);
    } else {
        // Out of the frame, just to the right of the outermost frame the nodes were in.
        float fx1 = -1e9f;
        for (const Frame& f : g.frames())
            for (Node* n : moving)
                if (frameHolds(f, *n)) fx1 = std::max(fx1, f.x + f.w);
        tx = fx1 + kGap, ty = y0;
    }
    for (Node* n : moving) n->x = std::round(n->x - x0 + tx), n->y = std::round(n->y - y0 + ty);
    return true;
}

// ---------------------------------------------------------------- groups

int NodeEditor::selectedGroup(const Graph& g) const {
    if (selection_.size() != 1) return 0;
    return dynamic_cast<GroupNode*>(g.find(*selection_.begin())) ? *selection_.begin() : 0;
}

bool NodeEditor::groupSelection(Graph& g) {
    if (selection_.empty()) return false;
    int gid = groupNodes(g, selection_);
    if (!gid) return false;
    select(gid);
    return true;
}

bool NodeEditor::ungroupSelection(Graph& g) {
    std::set<int> restored;
    bool any = false;
    for (int id : std::set<int>(selection_)) {
        if (!dynamic_cast<GroupNode*>(g.find(id))) continue;
        for (int n : ungroupNode(g, id)) restored.insert(n);
        any = true;
    }
    if (any) selection_ = restored;
    return any;
}

// ---------------------------------------------------------------- Blender-style conveniences

void NodeEditor::beginGrab(Graph& g) {
    dragStart_.clear();
    for (int id : selection_)
        if (Node* n = g.find(id)) dragStart_[id] = ImVec2(n->x, n->y);
    pressPos_ = ImGui::GetIO().MousePos;
    mode_ = Mode::Grab;
}

void NodeEditor::finishDragNodes(Graph& g, Result& r) {
    if (insertLink_) {
        // Splice the dragged node into the wire it was dropped on.
        Link l = *std::find_if(g.links().begin(), g.links().end(), [&](const Link& k) { return k.id == insertLink_; });
        const int id = *selection_.begin();
        g.removeLink(l.id);
        g.connect(l.fromNode, l.fromPin, id, insertIn_);
        g.connect(id, insertOut_, l.toNode, l.toPin);
        r.evalChanged = r.docChanged = true;

        // Auto-offset: if the spliced node overlaps what comes after it, push the downstream
        // nodes right to make room (Blender does the same).
        Node* n = g.find(id);
        Node* next = g.find(l.toNode);
        const float gap = 40.0f;
        if (n && next && next->x < n->x + kNodeW + gap) {
            const float shift = n->x + kNodeW + gap - next->x;
            std::set<int> down{l.toNode};
            std::vector<int> stack{l.toNode};
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                for (const Link& k : g.links())
                    if (k.fromNode == cur && k.toNode != id && down.insert(k.toNode).second) stack.push_back(k.toNode);
            }
            for (int d : down)
                if (Node* dn = g.find(d)) dn->x += shift;
        }
        spaceOut(g, {id}, gridSize);  // anything else it landed on (nodes above/below the wire)
    }
    insertLink_ = 0;
}

nlohmann::json NodeEditor::selectionJson(const Graph& g) const {
    nlohmann::json all = g.toJson();
    nlohmann::json clip = {{"nodes", nlohmann::json::array()}, {"links", nlohmann::json::array()}};
    for (const auto& n : all["nodes"])
        if (selection_.count(n["id"].get<int>())) clip["nodes"].push_back(n);
    for (const auto& l : all["links"])
        if (selection_.count(l["from"][0].get<int>()) && selection_.count(l["to"][0].get<int>())) clip["links"].push_back(l);
    return clip;
}

void NodeEditor::copySelection(const Graph& g) {
    if (selection_.empty()) return;
    nlohmann::json clip = selectionJson(g);
    clip["nodelabClipboard"] = 1;
    ImGui::SetClipboardText(clip.dump().c_str());
}

bool NodeEditor::paste(Graph& g) {
    const char* text = ImGui::GetClipboardText();
    if (!text) return false;
    nlohmann::json clip = nlohmann::json::parse(text, nullptr, false);
    if (clip.is_discarded() || !clip.contains("nodelabClipboard")) return false;
    return insertClip(g, clip, toGrid(ImGui::GetIO().MousePos));
}

bool NodeEditor::insertClip(Graph& g, const nlohmann::json& clip, ImVec2 at) {
    if (!clip.is_object() || !clip.contains("nodes")) return false;
    // Rebuild in a scratch graph, then clone into this one.
    Graph tmp;
    try {
        tmp.fromJson({{"nextId", 1}, {"nodes", clip["nodes"]}, {"links", clip.value("links", nlohmann::json::array())}});
    } catch (const std::exception&) {
        return false;
    }
    if (tmp.nodes().empty()) return false;
    float x0 = 1e9f, y0 = 1e9f;
    for (const auto& [id, n] : tmp.nodes()) x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
    std::map<int, int> remap;
    for (const auto& [id, n] : tmp.nodes())
        if (Node* c = g.cloneNode(*n, std::round(n->x - x0 + at.x), std::round(n->y - y0 + at.y))) remap[id] = c->id;
    for (const Link& l : tmp.links())
        if (remap.count(l.fromNode) && remap.count(l.toNode)) g.connect(remap[l.fromNode], l.fromPin, remap[l.toNode], l.toPin);
    selection_.clear();
    for (auto& [a, b] : remap) selection_.insert(b);
    spaceOut(g, selection_, gridSize);
    selectedLink_ = 0;
    return true;
}

bool NodeEditor::toggleMute(Graph& g) {
    bool any = false, allMuted = true;
    for (int id : selection_)
        if (Node* n = g.find(id); n && n->info().outputs.size() && !n->muted) allMuted = false;
    for (int id : selection_)
        if (Node* n = g.find(id); n && !n->info().outputs.empty()) {
            n->muted = !allMuted;
            any = true;
        }
    return any;
}

bool NodeEditor::resetSelection(Graph& g) {
    bool any = false;
    for (int id : selection_)
        if (Node* n = g.find(id)) {
            const std::vector<nlohmann::json> before = n->params;
            n->resetParams();
            any |= n->params != before;
        }
    return any;
}

bool NodeEditor::toggleCollapse(Graph& g) {
    bool any = false, allCollapsed = true;
    for (int id : selection_)
        if (Node* n = g.find(id); n && !n->collapsed) allCollapsed = false;
    for (int id : selection_)
        if (Node* n = g.find(id); n && !isReroute(*n)) {
            n->collapsed = !allCollapsed;
            any = true;
        }
    return any;
}

bool NodeEditor::makeLinks(Graph& g) {
    // Left to right (top to bottom in a column), each node into the next.
    std::vector<Node*> nodes;
    for (int id : selection_)
        if (Node* n = g.find(id)) nodes.push_back(n);
    std::sort(nodes.begin(), nodes.end(), [](Node* a, Node* b) { return a->x != b->x ? a->x < b->x : a->y < b->y; });
    bool any = false;
    cycle_ = {};
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        const Node* a = nodes[i];
        const Node* b = nodes[i + 1];
        // Already wired together: pressing F again shouldn't stack a second wire between them.
        if (std::any_of(g.links().begin(), g.links().end(), [&](const Link& l) { return l.fromNode == a->id && l.toNode == b->id; }))
            continue;
        // The first output that has a free input of its type (else one it converts to); failing
        // that, replace the best wired input rather than doing nothing, as Blender's Shift+F.
        int bestO = -1, bestI = -1;
        for (int pass = 0; pass < 2 && bestI < 0; ++pass)
            for (int o = 0; o < int(a->info().outputs.size()) && bestI < 0; ++o) {
                const int in = pickPin(g, *b, true, a->info().outputs[o].type);
                if (in >= 0 && (pass == 1 || !g.inputLink(b->id, in))) bestO = o, bestI = in;
            }
        if (bestI < 0) continue;
        LinkCycle c;
        if (const Link* old = g.inputLink(b->id, bestI)) c.replacedFrom = old->fromNode, c.replacedFromPin = old->fromPin, c.replacedTo = bestI;
        if (!g.connect(a->id, bestO, b->id, bestI)) continue;
        any = true;
        c.from = a->id, c.fromPin = bestO, c.to = b->id, c.toPin = bestI;
        c.selection = selection_;
        cycle_ = c;
    }
    return any;
}

bool NodeEditor::cycleLink(Graph& g, int what) {
    const Node* a = g.find(cycle_.from);
    const Node* b = g.find(cycle_.to);
    if (!a || !b) return false;
    const auto& outs = a->info().outputs;
    const auto& ins = b->info().inputs;
    const int nOut = int(outs.size()), nIn = int(ins.size());
    // Inputs another wire feeds are skipped, so stepping never undoes other work (except the one
    // F replaced, which comes back when the cycle moves on).
    auto usable = [&](int in) {
        if (in == cycle_.toPin || in == cycle_.replacedTo) return true;
        return g.inputLink(b->id, in) == nullptr;
    };
    const int total = what == 1 ? nOut : what == 2 ? nIn : nOut * nIn;
    const int cur = what == 1 ? cycle_.fromPin : what == 2 ? cycle_.toPin : cycle_.fromPin * nIn + cycle_.toPin;
    for (int step = 1; step < total; ++step) {
        const int k = (cur + step) % total;
        const int o = what == 1 ? k : what == 2 ? cycle_.fromPin : k / nIn;
        const int i = what == 1 ? cycle_.toPin : what == 2 ? k : k % nIn;
        if (!usable(i) || !canConvert(outs[o].type, ins[i].type)) continue;
        if (const Link* l = g.inputLink(b->id, cycle_.toPin); l && l->fromNode == a->id && l->fromPin == cycle_.fromPin)
            g.removeLink(l->id);
        if (!g.connect(a->id, o, b->id, i)) {
            g.connect(a->id, cycle_.fromPin, b->id, cycle_.toPin);  // rejected (a cycle): put it back
            continue;
        }
        cycle_.fromPin = o, cycle_.toPin = i;
        if (cycle_.replacedTo >= 0 && i != cycle_.replacedTo && !g.inputLink(b->id, cycle_.replacedTo))
            g.connect(cycle_.replacedFrom, cycle_.replacedFromPin, b->id, cycle_.replacedTo);
        return true;
    }
    return false;
}

void NodeEditor::drawLinkCycleHint(const Graph& g) {
    const Node* a = g.find(cycle_.from);
    const Node* b = g.find(cycle_.to);
    if (!a || !b || cycle_.fromPin >= int(a->info().outputs.size()) || cycle_.toPin >= int(b->info().inputs.size())) return;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s: %s  ->  %s: %s      1 output   2 input   F next   Esc done", a->title().c_str(),
                  a->info().outputs[cycle_.fromPin].name.c_str(), b->title().c_str(), b->info().inputs[cycle_.toPin].name.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const ImVec2 p(origin_.x + 10, origin_.y + size_.y - ts.y - 16);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + ts.x + 6, p.y + ts.y + 4), IM_COL32(20, 20, 24, 225), 4.0f);
    dl->AddText(p, IM_COL32(235, 235, 240, 255), buf);
}

bool NodeEditor::detachLinks(Graph& g) {
    // Only the wires with both ends in the selection; wires to the rest of the graph stay.
    std::vector<int> ids;
    for (const Link& l : g.links())
        if (selection_.count(l.fromNode) && selection_.count(l.toNode)) ids.push_back(l.id);
    for (int id : ids) g.removeLink(id);
    return !ids.empty();
}

bool NodeEditor::swapLinks(Graph& g) {
    std::vector<Node*> sel;
    for (int id : selection_)
        if (Node* n = g.find(id)) sel.push_back(n);
    if (sel.size() == 1) {
        Node& n = *sel[0];
        const auto& ins = n.info().inputs;
        std::vector<Link> in;
        for (const Link& l : g.links())
            if (l.toNode == n.id) in.push_back(l);
        std::sort(in.begin(), in.end(), [](const Link& x, const Link& y) { return x.toPin < y.toPin; });
        if (in.size() >= 2) {
            // The first two wired inputs trade wires.
            const Link x = in[0], y = in[1];
            g.removeLink(x.id);
            g.removeLink(y.id);
            const bool ok = g.connect(x.fromNode, x.fromPin, n.id, y.toPin) && g.connect(y.fromNode, y.fromPin, n.id, x.toPin);
            if (!ok) {
                g.connect(x.fromNode, x.fromPin, n.id, x.toPin);
                g.connect(y.fromNode, y.fromPin, n.id, y.toPin);
            }
            return ok;
        }
        if (in.size() == 1) {
            // One wire: on to the next input that takes it.
            const Link l = in[0];
            const PinType t = g.find(l.fromNode)->info().outputs[l.fromPin].type;
            const int nIn = int(ins.size());
            for (int step = 1; step < nIn; ++step) {
                const int i = (l.toPin + step) % nIn;
                if (!canConvert(t, ins[i].type)) continue;
                g.removeLink(l.id);
                if (g.connect(l.fromNode, l.fromPin, n.id, i)) return true;
                g.connect(l.fromNode, l.fromPin, n.id, l.toPin);
            }
        }
        return false;
    }
    if (sel.size() != 2) return false;
    Node* A = sel[0];
    Node* B = sel[1];
    std::vector<Link> ls;
    for (const Link& l : g.links())
        if (l.fromNode == A->id || l.fromNode == B->id || l.toNode == A->id || l.toNode == B->id) ls.push_back(l);
    for (const Link& l : ls) g.removeLink(l.id);
    // Each end on A or B moves to the other node: the same pin when it has the same type, else
    // the best one of that type (inputs: a free one first).
    auto partner = [&](int id) { return id == A->id ? B : id == B->id ? A : g.find(id); };
    auto mapPin = [&](const Node& from, const Node& to, int pin, bool inputs) {
        if (&from == &to) return pin;
        const auto& fp = inputs ? from.info().inputs : from.info().outputs;
        const auto& tp = inputs ? to.info().inputs : to.info().outputs;
        const PinType t = fp[size_t(pin)].type;
        if (pin < int(tp.size()) && tp[size_t(pin)].type == t && !(inputs && g.inputLink(to.id, pin))) return pin;
        return pickPin(g, to, inputs, t);
    };
    for (const Link& l : ls) {
        const Node* f = g.find(l.fromNode);
        const Node* t = g.find(l.toNode);
        const Node* nf = partner(l.fromNode);
        const Node* nt = partner(l.toNode);
        if (!f || !t || !nf || !nt) continue;
        const int fp = mapPin(*f, *nf, l.fromPin, false), tp = mapPin(*t, *nt, l.toPin, true);
        if (fp >= 0 && tp >= 0) g.connect(nf->id, fp, nt->id, tp);
    }
    std::swap(A->x, B->x);
    std::swap(A->y, B->y);
    return true;
}

void NodeEditor::selectLinked(const Graph& g, bool downstream) {
    std::vector<int> stack(selection_.begin(), selection_.end());
    while (!stack.empty()) {
        int cur = stack.back();
        stack.pop_back();
        for (const Link& l : g.links()) {
            int next = downstream ? (l.fromNode == cur ? l.toNode : 0) : (l.toNode == cur ? l.fromNode : 0);
            if (next && selection_.insert(next).second) stack.push_back(next);
        }
    }
}

void NodeEditor::frameSelected(const Graph& g) {
    if (selection_.empty()) {
        fitFrames_ = 1;
        return;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int id : selection_)
        if (const Node* n = g.find(id)) {
            x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
            x1 = std::max(x1, n->x + kNodeW), y1 = std::max(y1, n->y + nodeHeightGrid(*n));
        }
    zoom_ = std::clamp(std::min((size_.x - 80) / (x1 - x0), (size_.y - 80) / (y1 - y0)), kMinZoom, 1.5f);
    pan_ = ImVec2(size_.x * 0.5f - (x0 + x1) * 0.5f * zoom_, size_.y * 0.5f - (y0 + y1) * 0.5f * zoom_);
}

void NodeEditor::drawFindMenu(Graph& g, Result& r) {
    // Labelled matches first (the names users gave), then this graph before nested groups, then
    // by position: top to bottom, left to right. Nodes inside groups show their group path.
    struct Hit {
        int id;
        std::vector<int> path;  // group ids from g down to the node's graph
        std::string text, type;
        float x, y;
    };
    std::vector<Hit> hits;
    size_t total = 0;
    std::vector<int> path;
    auto collect = [&](auto& self, const Graph& gr, const std::string& prefix) -> void {
        for (const auto& [id, n] : gr.nodes()) {
            ++total;
            const std::string& name = n->info().displayName;
            if (!search_[0] || containsNoCase(n->label, search_) || containsNoCase(name, search_))
                hits.push_back({id, path, prefix + (n->label.empty() ? name : n->label), n->label.empty() ? std::string() : name, n->x, n->y});
            if (const auto* grp = dynamic_cast<const GroupNode*>(n.get())) {
                path.push_back(id);
                self(self, grp->inner(), prefix + (n->label.empty() ? grp->name : n->label) + " > ");
                path.pop_back();
            }
        }
    };
    if (!ImGui::IsPopupOpen("FindNode")) return;
    collect(collect, g, "");
    {
        // Fixed size inside the window, as the add menu (see drawAddMenu): room for every node of a
        // small graph, so it doesn't jump while filtering, and a scrolling list for a big one.
        const ImGuiStyle& st = ImGui::GetStyle();
        const ImGuiViewport* vp = ImGui::GetWindowViewport();
        const int rows = std::clamp(int(total), 3, 12);
        const ImVec2 size(ImGui::GetFontSize() * 20.0f, ImGui::GetFrameHeightWithSpacing() * 2 +
                                                          ImGui::GetTextLineHeightWithSpacing() * rows + st.WindowPadding.y * 2);
        ImVec2 pos = menuPos_;
        pos.x = std::max(vp->Pos.x, std::min(pos.x, vp->Pos.x + vp->Size.x - size.x));
        pos.y = std::max(vp->Pos.y, std::min(pos.y, vp->Pos.y + vp->Size.y - size.y));
        ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(size);
    }
    if (!ImGui::BeginPopup("FindNode")) return;
    ImGui::TextDisabled("Find Node");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##find", "Label or node name...", search_, sizeof(search_))) searchSel_ = 0;

    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.type.empty() != b.type.empty()) return !a.type.empty();
        if (a.path != b.path) return a.path.size() != b.path.size() ? a.path.size() < b.path.size() : a.path < b.path;
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    const int n = int(hits.size());
    bool moved = false;
    if (n > 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) searchSel_ = (searchSel_ + 1) % n, moved = true;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) searchSel_ = (searchSel_ + n - 1) % n, moved = true;
    }
    searchSel_ = n ? std::clamp(searchSel_, 0, n - 1) : 0;
    if (n == 0) ImGui::TextDisabled("No matching nodes");
    int chosen = -1;
    ImGui::BeginChild("##results", ImVec2(0, 0), ImGuiChildFlags_None);
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);  // node ids repeat across groups
        if (ImGui::Selectable(hits[i].text.c_str(), i == searchSel_)) chosen = i;
        if (i == searchSel_ && moved) ImGui::SetScrollHereY();
        if (!hits[i].type.empty()) {
            ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
            ImGui::TextDisabled("%s", hits[i].type.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (chosen < 0 && n > 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        chosen = searchSel_;
    if (chosen >= 0) {
        const Hit& hit = hits[size_t(chosen)];
        if (hit.path.empty()) {
            select(hit.id);
            frameSelected(g);
        } else {
            r.findPath = hit.path;  // the App opens the group; it then selects and frames the node there
            r.findNode = hit.id;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void NodeEditor::drawPresetPopup(const Graph& g) {
    if (!ImGui::BeginPopup("SavePreset")) return;
    ImGui::TextDisabled("Save the %d selected node%s as a preset (Add > Presets)", int(selection_.size()),
                        selection_.size() == 1 ? "" : "s");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(240);
    bool done = ImGui::InputText("##preset", presetName_, sizeof(presetName_), ImGuiInputTextFlags_EnterReturnsTrue);
    const bool exists = std::find(presetNames_.begin(), presetNames_.end(), std::string(presetName_)) != presetNames_.end();
    ImGui::SameLine();
    done |= ImGui::Button(exists ? "Replace" : "Save");
    if (!presetStatus_.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", presetStatus_.c_str());
    if (done) {
        if (presets::save(presetName_, selectionJson(g), presetStatus_)) {
            presetNames_ = presets::list();
            ImGui::CloseCurrentPopup();
        }
    }
    if (ImGui::IsWindowAppearing()) presetNames_ = presets::list();
    ImGui::EndPopup();
}

void NodeEditor::drawRenamePopup(Graph& g, Result& r) {
    if (!ImGui::BeginPopup("NodeRename")) return;
    Node* n = g.find(renameNode_);
    if (!n) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("Label for %s (empty = default)", n->info().displayName.c_str());
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(240);
    bool done = ImGui::InputText("##label", renameBuf_, sizeof(renameBuf_), ImGuiInputTextFlags_EnterReturnsTrue);
    if (n->label != renameBuf_) {
        n->label = renameBuf_;
        r.docChanged = true;
    }
    if (done) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
