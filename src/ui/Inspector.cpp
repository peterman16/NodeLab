#include "ui/Inspector.h"

#include <algorithm>
#include <cstdio>

#include <imgui.h>
#include <imgui_internal.h>

#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Paths.h"
#include "nodes/group/GroupNodes.h"
#include "ui/ColorDisplay.h"
#include "ui/Eyedropper.h"
#include "ui/FileDialog.h"
#include "ui/GuideWindow.h"
#include "ui/NodeInspectors.h"
#include "ui/ParamWidgets.h"
#include "ui/SliderTrack.h"
#include "ui/Theme.h"


namespace {

// Blender's number fields: double-click types a value, and Backspace over one (or right-click >
// Reset to Default) resets it. ImGui sliders only type on Ctrl+click, and their first click
// already moved the value to the mouse, so a double-click puts back the value from before it.
struct SliderClick {
    ImGuiID id = 0;
    nlohmann::json before;
};
SliderClick g_firstClick;
ImGuiID g_typeNext = 0;  // slider to open for typing on its next frame

// Call before the slider: opens it for typing when a double-click asked for that.
void beginNumberField(const char* label) {
    if (g_typeNext && g_typeNext == ImGui::GetID(label)) {
        ImGui::SetKeyboardFocusHere();
        g_typeNext = 0;
    }
}

// Call right after the slider (the last item). Returns true if it changed the param.
bool endNumberField(Node& node, int i, const nlohmann::json& before, bool isSlider) {
    bool changed = false;
    const ImGuiID id = ImGui::GetItemID();
    if (isSlider && ImGui::IsItemActivated()) {
        if (ImGui::GetIO().MouseClickedCount[ImGuiMouseButton_Left] >= 2 && g_firstClick.id == id) {
            node.params[i] = g_firstClick.before;
            g_typeNext = id;
            // Let go of the slider, or it keeps dragging to the mouse while the button is held.
            ImGui::ClearActiveID();
            changed = true;
        } else {
            g_firstClick = {id, before};
        }
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !ImGui::IsItemActive() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        node.resetParam(i);
        changed = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##numberMenu");
    if (ImGui::BeginPopup("##numberMenu")) {
        if (ImGui::MenuItem("Reset to Default", "Backspace")) {
            node.resetParam(i);
            changed = true;
        }
        if (ImGui::MenuItem("Edit Value", "Double-click")) g_typeNext = id;
        ImGui::EndPopup();
    }
    return changed;
}

}  // namespace

bool editParam(Node& node, int i, float width, bool compact) {
    const ParamDesc& d = node.info().params[i];
    std::string label = compact ? "##" + d.name : d.name;
    bool changed = false;
    ImGui::PushID(i);
    ImGui::SetNextItemWidth(width);
    switch (d.kind) {
        case ParamKind::Float: {
            const nlohmann::json before = node.params[i];
            float v = node.paramF(i);
            beginNumberField(label.c_str());
            const bool drag = compact || d.hardMax > d.max || d.hardMin < d.min;
            const bool track = d.track != SliderTrack::None;
            if (track) {
                // The coloured track goes under a see-through frame; hover and drag still lighten it.
                const ImVec2 p = ImGui::GetCursorScreenPos();
                slidertrack::draw(ImGui::GetWindowDrawList(), p, ImVec2(p.x + ImGui::CalcItemWidth(), p.y + ImGui::GetFrameHeight()),
                                  d, theme::col(theme::Field), ImGui::GetStyle().FrameRounding);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 22));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 34));
                ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(235, 235, 235, 230));
                ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(255, 255, 255, 255));
                // A thin grab, so the value text centred over it stays readable at 0.
                ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 4.0f);
            }
            if (drag) {
                // Unbounded (math) values: drag field whose speed follows the soft range.
                changed = ImGui::DragFloat(label.c_str(), &v, (d.max - d.min) / 300.0f, d.hardMin, d.hardMax, "%.3f",
                                           ImGuiSliderFlags_AlwaysClamp);
            } else {
                changed = ImGui::SliderFloat(label.c_str(), &v, d.min, d.max, d.max - d.min >= 20.0f ? "%.1f" : "%.3f",
                                             ImGuiSliderFlags_AlwaysClamp);
            }
            if (track) {
                ImGui::PopStyleColor(5);
                ImGui::PopStyleVar();
            }
            if (changed) node.params[i] = v;
            changed |= endNumberField(node, i, before, !drag);
            break;
        }
        case ParamKind::Int: {
            const nlohmann::json before = node.params[i];
            int v = node.paramI(i);
            beginNumberField(label.c_str());
            changed = ImGui::SliderInt(label.c_str(), &v, int(d.min), int(d.max), "%d", ImGuiSliderFlags_AlwaysClamp);
            if (changed) node.params[i] = v;
            changed |= endNumberField(node, i, before, true);
            break;
        }
        case ParamKind::Bool: {
            bool v = node.paramB(i);
            changed = ImGui::Checkbox(label.c_str(), &v);
            if (changed) node.params[i] = v;
            break;
        }
        case ParamKind::Enum: {
            int v = node.paramI(i);
            const char* preview = (v >= 0 && v < int(d.options.size())) ? d.options[v].c_str() : "?";
            if (ImGui::BeginCombo(label.c_str(), preview)) {
                for (int k = 0; k < int(d.options.size()); ++k) {
                    if (ImGui::Selectable(d.options[k].c_str(), k == v)) {
                        node.params[i] = k;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case ParamKind::Path: {
            std::string path = node.paramS(i);
            std::string name = path.empty() ? "(none)" : pathToU8(u8ToPath(path).filename());
            if (ImGui::Button("Browse...")) {
                if (auto p = openFileDialog("Choose image", kImageFileFilter)) {
                    chooseImageFile(node, i, *p);
                    changed = true;
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(name.c_str());
            if (!path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            break;
        }
        case ParamKind::Text: {
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "%s", node.paramS(i).c_str());
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                node.params[i] = std::string(buf);
                changed = true;
            }
            break;
        }
        case ParamKind::Curve:
            // Channel buttons share the label's line, leaving more height for the curve.
            ImGui::TextUnformatted(d.name.c_str());
            ImGui::SameLine(0, 16);
            changed = curveEditor("##curves", node.params[i], d.options);
            break;
        case ParamKind::Color: {
            float c[3];
            node.paramC(i, c);
            if (!d.gammaColor) colordisplay::toDisplay(c);
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
            if (d.max > 1.0f) flags |= ImGuiColorEditFlags_HDR;
            if (ImGui::ColorEdit3(label.c_str(), c, flags)) {
                if (!d.gammaColor) colordisplay::fromDisplay(c);
                for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k], d.hardMin, d.hardMax);
                node.params[i] = nlohmann::json::array({c[0], c[1], c[2]});
                changed = true;
            }
            ImGui::SameLine();
            const bool picking = eyedropper().is(node.id, i);
            if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(picking ? "Picking..." : "Pick")) eyedropper().toggle(node.id, i);
            if (picking) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Eyedropper: click a pixel in the Original, Result or a viewer,\n"
                                  "or drag a rectangle to use its average colour. Right-click or Esc cancels.");
            break;
        }
        case ParamKind::SavePath: {
            std::string path = node.paramS(i);
            if (ImGui::Button("Save as...")) {
                if (auto p = saveFileDialog("File Output", kSaveImageFilter, "png")) {
                    node.params[i] = *p;
                    // File Output: the type picked in the dialog sets the Format.
                    const auto& descs = node.info().params;
                    if (size_t(i) + 1 < descs.size() && descs[i + 1].name == "Format")
                        node.params[i + 1] = int(formatFromPath(*p));
                    changed = true;
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(path.empty() ? "(not set)" : pathToU8(u8ToPath(path).filename()).c_str());
            if (!path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            break;
        }
        case ParamKind::Ramp:
            ImGui::TextUnformatted(d.name.c_str());
            changed = rampEditor("##ramp", node.params[i]);
            break;
    }
    ImGui::PopID();
    return changed;
}

bool drawGroupInterface(GroupNode& group, Graph& outer) {
    bool changed = false;
    char name[128];
    std::snprintf(name, sizeof(name), "%s", group.name.c_str());
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("Group name", name, sizeof(name))) {
        group.name = name;
        group.syncInner();
        changed = true;
    }
    static const char* types[] = {"Image", "Channel", "Number"};
    for (int side = 0; side < 2; ++side) {
        const bool output = side == 1;
        auto& pins = output ? group.outs : group.ins;
        ImGui::SeparatorText(output ? "Outputs" : "Inputs");
        ImGui::PushID(side);
        for (int i = 0; i < int(pins.size()); ++i) {
            ImGui::PushID(i);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", pins[i].name.c_str());
            ImGui::SetNextItemWidth(150);
            if (ImGui::InputText("##name", buf, sizeof(buf))) {
                pins[i].name = buf;
                group.syncInner();
                changed = true;
            }
            ImGui::SameLine();
            int t = int(pins[i].type);
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##type", &t, types, 3)) {
                group.setPinType(outer, output, i, PinType(t));
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                group.movePin(outer, output, i, -1);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                group.movePin(outer, output, i, +1);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                group.removePin(outer, output, i);
                changed = true;
                ImGui::PopID();
                break;
            }
            if (!output && pins[i].type != PinType::Image && i < int(group.ranges.size())) {
                // Blender's socket Default / Min / Max: the slider shown on the group node while
                // the input is unconnected.
                GroupNode::InputRange r = group.ranges[size_t(i)];
                float v[3] = {r.def, r.min, r.max};
                ImGui::Indent();
                ImGui::SetNextItemWidth(240);
                if (ImGui::DragFloat3("Default / Min / Max", v, 0.01f, -1e6f, 1e6f, "%.3f")) {
                    // Moving min past max (or back) drags the other along.
                    if (v[1] != r.min) v[2] = std::max(v[2], v[1]);
                    else if (v[2] != r.max) v[1] = std::min(v[1], v[2]);
                    group.setRange(i, {v[0], v[1], v[2]});
                    changed = true;
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton(output ? "Add Output" : "Add Input")) {
            group.addPin(outer, output, {output ? "Result" : "Value", PinType::Image});
            changed = true;
        }
        ImGui::PopID();
    }
    return changed;
}

bool drawInspector(Graph& g, int selectedNode, GroupNode* owner, Graph* ownerParent) {
    Node* n = g.find(selectedNode);
    if (n) {
        if (auto* grp = dynamic_cast<GroupNode*>(n)) {
            ImGui::Text("Group: %s", grp->name.c_str());
            ImGui::TextDisabled("Tab or double-click to edit its contents, Ctrl+Alt+G to ungroup.");
            ImGui::Separator();
            return drawGroupInterface(*grp, g);
        }
        if (auto* v = dynamic_cast<GroupValueNode*>(n); v && owner && ownerParent) {
            const bool output = v->isOutput();
            auto& pins = output ? owner->outs : owner->ins;
            if (v->pin < 0 || v->pin >= int(pins.size())) return false;
            ImGui::Text("%s", output ? "Value Output" : "Value Input");
            ImGui::TextDisabled("The group's %s socket \"%s\". F2 renames it too.", output ? "output" : "input",
                                pins[size_t(v->pin)].name.c_str());
            ImGui::Separator();
            bool changed = false;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", pins[size_t(v->pin)].name.c_str());
            ImGui::SetNextItemWidth(240);
            if (ImGui::InputText("Name", buf, sizeof(buf)) && buf[0]) {
                owner->renamePin(output, v->pin, buf);
                changed = true;
            }
            // The socket's type: Number for a slider, Channel for a mask, Image for pixels.
            static const char* kTypes[] = {"Image", "Channel", "Number"};
            int t = int(pins[size_t(v->pin)].type);
            ImGui::SetNextItemWidth(240);
            if (ImGui::Combo("Type", &t, kTypes, 3)) {
                owner->setPinType(*ownerParent, output, v->pin, PinType(t));
                changed = true;
            }
            if (!output && pins[size_t(v->pin)].type != PinType::Image && v->pin < int(owner->ranges.size())) {
                GroupNode::InputRange r = owner->ranges[size_t(v->pin)];
                float vals[3] = {r.def, r.min, r.max};
                ImGui::SetNextItemWidth(240);
                if (ImGui::DragFloat3("Default / Min / Max", vals, 0.01f, -1e6f, 1e6f, "%.3f")) {
                    if (vals[1] != r.min) vals[2] = std::max(vals[2], vals[1]);
                    else if (vals[2] != r.max) vals[1] = std::min(vals[1], vals[2]);
                    owner->setRange(v->pin, {vals[0], vals[1], vals[2]});
                    changed = true;
                }
            }
            if (ImGui::CollapsingHeader("All of the group's sockets")) changed |= drawGroupInterface(*owner, *ownerParent);
            return changed;
        }
        const bool io = dynamic_cast<GroupInputNode*>(n) || dynamic_cast<GroupOutputNode*>(n);
        if (io && owner && ownerParent) {
            ImGui::Text("%s", n->info().displayName.c_str());
            ImGui::TextDisabled("Edit the group's pins here. Tab to leave the group.");
            ImGui::Separator();
            return drawGroupInterface(*owner, *ownerParent);
        }
    }
    if (!n) {
        ImGui::TextDisabled("Select a node to edit its settings.");
        ImGui::TextDisabled("Right-click the canvas to add nodes. Ctrl+click a node to preview it.");
        ImGui::TextDisabled("Drag empty space to pan, wheel to zoom, Shift+drag to box-select, Home to frame all. Help menu lists all shortcuts.");
        return false;
    }
    const NodeInfo& info = n->info();
    ImGui::Text("%s", info.displayName.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", info.category.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Guide")) openGuide(info.displayName);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What this node does (F1)");
    if (info.type == "conv.expression" || info.type == "conv.image_expression") {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Variables: r g b a (Image input), in1 in2, x y (pixel), u v (0..1), w h\n"
                              "Functions: sin cos tan pow sqrt abs floor ceil log ln exp atan2\n"
                              "           min max clamp mix step smoothstep fract\n"
                              "Example: mix(r, b, smoothstep(0.3, 0.7, v))");
    }
    ImGui::Separator();

    if (info.params.empty()) {
        ImGui::TextDisabled("No settings.");
        return false;
    }

    bool changed = false;
    const float width = ImGui::GetContentRegionAvail().x * 0.6f;
    // Scope widget state (selected curve channel, ramp stop) to this node, so selecting another
    // Curves node doesn't inherit the previous one's channel.
    ImGui::PushID(n->id);
    const ParamRow row = [&](int i) {
        if (!n->paramVisible(i)) return false;
        // A param that backs a connected input is overridden by the wire.
        bool driven = false;
        for (int p = 0; p < int(info.inputs.size()); ++p)
            if (info.inputs[p].fallbackParam == i && g.inputLink(n->id, p)) driven = true;
        if (driven) {
            ImGui::BeginDisabled();
            editParam(*n, i, width, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(wired)");
            return false;
        }
        const bool c = editParam(*n, i, width, false);
        changed |= c;
        return c;
    };
    if (!drawNodeInspector(*n, row, changed, &g))
        for (int i = 0; i < int(info.params.size()); ++i) row(i);
    ImGui::PopID();
    return changed;
}
