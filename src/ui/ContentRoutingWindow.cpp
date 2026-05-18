#include "entity/ui/ContentRoutingWindow.hpp"

#include "entity/ui/ContentRoutingCanvas.hpp"
#include "entity/ui/FeedMapEditorWindow.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingAssetOps.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/timeline/Timeline.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace entity {

namespace {

// Identity uvRect — the "fills the whole source" sentinel.
constexpr std::array<float, 4> kIdentityUV{0.0f, 0.0f, 1.0f, 1.0f};

// Horizontal splitter helper. Draws a thin draggable bar that
// resizes `upperSize` (the height of the area above the splitter)
// within [minUpper, totalHeight - thickness - minLower]. Cursor
// turns to ResizeNS on hover; bar tints brighter while dragging.
// Uses public ImGui API only — the project is on 1.89.7 which
// doesn't have ImGuiChildFlags_ResizeY yet.
void splitterY(const char* id, float* upperSize,
                float minUpper, float minLower, float totalHeight) {
    constexpr float thickness = 6.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;

    ImGui::InvisibleButton(id, ImVec2(width, thickness));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    if (hovered || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (active) {
        *upperSize += ImGui::GetIO().MouseDelta.y;
    }
    const float maxUpper = std::max(minUpper, totalHeight - thickness - minLower);
    *upperSize = std::clamp(*upperSize, minUpper, maxUpper);

    const ImU32 col = active
        ? IM_COL32(90, 150, 210, 255)
        : (hovered ? IM_COL32(110, 110, 130, 255)
                   : IM_COL32(60, 60, 70, 255));
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + thickness), col);
}

// Look up the currently-selected clip's native pixel resolution.
// Returns false (and leaves outW/outH untouched) when there's no
// selection, the selection isn't a Clip, or the clip hasn't reported a
// resolution yet (decoder hasn't populated width/height).
bool getSelectedClipResolution(entt::registry& reg, Timeline* timeline,
                                std::uint32_t& outW, std::uint32_t& outH) {
    if (!timeline) return false;
    const entt::entity sel = timeline->getSelectedClip();
    if (sel == entt::null || !reg.valid(sel)) return false;
    const auto* clip = reg.try_get<Clip>(sel);
    if (!clip || clip->width == 0 || clip->height == 0) return false;
    outW = clip->width;
    outH = clip->height;
    return true;
}

// Materialize the Tiled targets vector from the asset's authoring
// metadata (tiledCount + tiledAxis). The existing targets are
// preserved up to the new count so the user doesn't lose Screen
// assignments when adjusting axis or count; new rows get the next
// available Screen (or entt::null if Screens run out).
void regenerateTiledFromParams(entt::registry& reg, ContentRoutingAsset& asset) {
    const std::uint8_t n = std::max<std::uint8_t>(1, asset.tiledCount);
    const bool horizontal = (asset.tiledAxis == 0);

    std::vector<entt::entity> savedScreens;
    savedScreens.reserve(asset.targets.size());
    for (const auto& t : asset.targets) savedScreens.push_back(t.screen);

    asset.targets.clear();
    asset.targets.reserve(n);

    std::vector<entt::entity> screenPool;
    for (auto [e, _] : reg.view<Screen>().each()) screenPool.push_back(e);

    const float slice = 1.0f / static_cast<float>(n);
    for (std::uint8_t i = 0; i < n; ++i) {
        RouteTarget t;
        if (i < savedScreens.size()) {
            t.screen = savedScreens[i];
        } else if (!screenPool.empty()) {
            t.screen = screenPool[i % screenPool.size()];
        }
        if (horizontal) {
            t.uvRect = {slice * i, 0.0f, slice, 1.0f};
        } else {
            t.uvRect = {0.0f, slice * i, 1.0f, slice};
        }
        asset.targets.push_back(t);
    }
}

// Walk every Clip / GenerativeLayer in the registry, return how many
// have a ContentRoutingRef pointing at the given asset entity.
int countUsage(entt::registry& reg, entt::entity assetEntity) {
    int n = 0;
    for (auto [_, ref] : reg.view<ContentRoutingRef>().each()) {
        if (ref.asset == assetEntity) ++n;
    }
    return n;
}

// Clear every ContentRoutingRef::asset that matches `assetEntity` (used
// before destroying the asset so dangling refs don't outlive it).
void clearRefsTo(entt::registry& reg, entt::entity assetEntity) {
    for (auto [_, ref] : reg.view<ContentRoutingRef>().each()) {
        if (ref.asset == assetEntity) ref.asset = entt::null;
    }
}

// Pick a unique asset name based on `base`. Increments a suffix until
// no existing asset claims it: "<base> 1", "<base> 2", ...
std::string pickUniqueName(entt::registry& reg, const char* base) {
    auto exists = [&](const std::string& candidate) {
        for (auto [_, a] : reg.view<ContentRoutingAsset>().each()) {
            if (a.name == candidate) return true;
        }
        return false;
    };
    for (int i = 1; i < 10000; ++i) {
        std::string candidate = std::string(base) + " " + std::to_string(i);
        if (!exists(candidate)) return candidate;
    }
    return std::string(base);  // shouldn't happen but bail safely
}

// Write an SVG template for a Feed Map asset (ADR-0022 L3). The SVG's
// viewBox matches the source canvas; one outlined <rect> per region
// with the region name + screen name as <text> labels inside. Returns
// true if the file was written; sets `outPath` to the resolved output
// path (best-effort: defaults under <cwd>/.feed-templates/ when no
// project directory is available).
bool exportFeedMapSvg(entt::registry& reg, const ContentRoutingAsset& asset,
                       std::string& outPath) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(".feed-templates");
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::string safeName = asset.name;
    for (char& c : safeName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    fs::path path = dir / (safeName + ".svg");
    outPath = path.string();

    std::ofstream out(path);
    if (!out.is_open()) return false;

    const auto w = std::max<std::uint32_t>(1, asset.sourceWidth);
    const auto h = std::max<std::uint32_t>(1, asset.sourceHeight);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 "
        << w << " " << h << "\" width=\"" << w << "\" height=\"" << h
        << "\">\n";
    out << "  <rect x=\"0\" y=\"0\" width=\"" << w << "\" height=\"" << h
        << "\" fill=\"#202028\" stroke=\"#606070\" stroke-width=\"2\"/>\n";
    out << "  <text x=\"12\" y=\"24\" font-family=\"sans-serif\" font-size=\"18\""
           " fill=\"#aaaaaa\">" << asset.name
        << " &#x2014; " << w << "&#xd7;" << h << "</text>\n";

    // Deterministic per-index hue, mirrors drawSchematic.
    static const char* fills[] = {
        "rgba(220,100,100,0.18)", "rgba(100,200,130,0.18)",
        "rgba(100,140,220,0.18)", "rgba(230,200,100,0.18)",
        "rgba(200,110,220,0.18)", "rgba(120,220,220,0.18)",
    };
    static const char* strokes[] = {
        "#dc6464", "#64c882", "#6488dc", "#e6c864",
        "#c870dc", "#78dcdc",
    };
    constexpr int kHueCount = 6;

    for (std::size_t i = 0; i < asset.targets.size(); ++i) {
        const auto& t = asset.targets[i];
        const float x = t.uvRect[0] * static_cast<float>(w);
        const float y = t.uvRect[1] * static_cast<float>(h);
        const float rw = t.uvRect[2] * static_cast<float>(w);
        const float rh = t.uvRect[3] * static_cast<float>(h);

        std::string regionName = t.name;
        std::string screenName = "(all visible)";
        if (t.screen != entt::null && reg.valid(t.screen) &&
            reg.all_of<Screen>(t.screen)) {
            screenName = reg.get<Screen>(t.screen).name;
        }
        if (regionName.empty()) regionName = "Region " + std::to_string(i + 1);

        out << "  <rect x=\"" << x << "\" y=\"" << y
            << "\" width=\"" << rw << "\" height=\"" << rh
            << "\" fill=\"" << fills[i % kHueCount]
            << "\" stroke=\"" << strokes[i % kHueCount]
            << "\" stroke-width=\"3\"/>\n";
        out << "  <text x=\"" << (x + 12.0f) << "\" y=\"" << (y + 32.0f)
            << "\" font-family=\"sans-serif\" font-size=\"24\""
               " fill=\"#ffffff\" font-weight=\"bold\">"
            << regionName << "</text>\n";
        out << "  <text x=\"" << (x + 12.0f) << "\" y=\"" << (y + 56.0f)
            << "\" font-family=\"sans-serif\" font-size=\"16\""
               " fill=\"#dddddd\">\xe2\x86\x92 "
            << screenName << "</text>\n";
    }
    out << "</svg>\n";
    return out.good();
}

// Canvas drawing moved to ContentRoutingCanvas.cpp so the new
// FeedMapEditorWindow can share the same renderer / drag / zoom /
// snap implementation. See entity::drawContentRoutingCanvas in
// include/entity/ui/ContentRoutingCanvas.hpp.

void renderLeftPane(entt::registry& reg, Timeline* timeline,
                     entt::entity& selected, entt::entity& pendingDelete) {
    // "+ Add" combo button — opens a small popup with kind options.
    if (ImGui::Button("+ Add##routingadd", ImVec2(-1, 0))) {
        ImGui::OpenPopup("##routing_add_kind");
    }
    if (ImGui::BeginPopup("##routing_add_kind")) {
        if (ImGui::Selectable("Direct")) {
            auto e = reg.create();
            auto& a = reg.emplace<ContentRoutingAsset>(e);
            a.name = pickUniqueName(reg, "New Direct");
            a.kind = RouteMode::Direct;
            a.targets.push_back({entt::null, kIdentityUV});
            selected = e;
        }
        if (ImGui::Selectable("Tiled")) {
            auto e = reg.create();
            auto& a = reg.emplace<ContentRoutingAsset>(e);
            a.name = pickUniqueName(reg, "New Tiled");
            a.kind = RouteMode::Tiled;
            a.tiledCount = 2;
            a.tiledAxis = 0;
            regenerateTiledFromParams(reg, a);
            selected = e;
        }
        if (ImGui::Selectable("Feed Map")) {
            auto e = reg.create();
            auto& a = reg.emplace<ContentRoutingAsset>(e);
            a.name = pickUniqueName(reg, "New Feed Map");
            a.kind = RouteMode::FeedMap;
            // Auto-populate source dimensions from the currently-
            // selected clip when one is available — saves the user
            // typing the resolution every time they author a feed map
            // for a specific source.
            std::uint32_t clipW = 0, clipH = 0;
            if (getSelectedClipResolution(reg, timeline, clipW, clipH)) {
                a.sourceWidth  = clipW;
                a.sourceHeight = clipH;
            } else {
                a.sourceWidth  = 1920;
                a.sourceHeight = 1080;
            }
            // Start with a single full-frame region so the table isn't
            // empty on first edit; user adds + names regions from there.
            RouteTarget t;
            t.uvRect = kIdentityUV;
            t.name   = "Region 1";
            a.targets.push_back(t);
            selected = e;
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // Split assets into auto-direct + user-created, sorted by name.
    struct Row { entt::entity e; std::string name; bool autoBound; };
    std::vector<Row> autoRows;
    std::vector<Row> userRows;
    for (auto [e, a] : reg.view<ContentRoutingAsset>().each()) {
        Row r{e, a.name, a.autoBoundScreen != entt::null};
        if (r.autoBound) autoRows.push_back(std::move(r));
        else userRows.push_back(std::move(r));
    }
    auto byName = [](const Row& l, const Row& r) { return l.name < r.name; };
    std::sort(autoRows.begin(), autoRows.end(), byName);
    std::sort(userRows.begin(), userRows.end(), byName);

    auto renderRows = [&](const std::vector<Row>& rows, const char* groupLabel) {
        if (rows.empty()) return;
        ImGui::TextDisabled("%s", groupLabel);
        for (const auto& r : rows) {
            ImGui::PushID(static_cast<int>(r.e));
            const bool isSelected = (selected == r.e);
            if (ImGui::Selectable(r.name.c_str(), isSelected,
                                   ImGuiSelectableFlags_AllowItemOverlap)) {
                selected = r.e;
            }
            if (r.autoBound) {
                ImGui::SameLine();
                ImGui::TextDisabled("auto");
            }
            // Right-click: Delete (with confirm)
            if (ImGui::BeginPopupContextItem("##rowctx")) {
                if (ImGui::MenuItem("Delete...")) {
                    pendingDelete = r.e;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    };

    renderRows(autoRows, "Direct (auto)");
    if (!autoRows.empty() && !userRows.empty()) ImGui::Spacing();
    renderRows(userRows, "User-created");

    if (autoRows.empty() && userRows.empty()) {
        ImGui::TextDisabled("(library is empty — add a Screen to create");
        ImGui::TextDisabled(" its auto-direct routing, or click + Add)");
    }
}

// Forward decl — renderDetailPane now calls renderSelectionStrip
// directly so the strip sits above the canvas (was a sibling in
// ContentRoutingWindow::render before the canvas grew to fill).
void renderSelectionStrip(entt::registry& reg, Timeline* timeline);

void renderDetailPane(entt::registry& reg, entt::entity selected,
                       IRenderer* renderer, Timeline* timeline,
                       ContentRoutingWindow::CanvasDragState& dragState,
                       ContentRoutingWindow::CanvasViewport& view,
                       FeedMapEditorWindow* feedMapEditor,
                       float& upperPaneHeight) {
    if (selected == entt::null || !reg.valid(selected) ||
        !reg.all_of<ContentRoutingAsset>(selected)) {
        ImGui::TextDisabled("Select a routing from the library.");
        return;
    }
    auto& asset = reg.get<ContentRoutingAsset>(selected);
    const bool isAutoBound = (asset.autoBoundScreen != entt::null);

    // Split the right pane vertically: upper area (controls + table)
    // and lower area (canvas preview). The splitter between is
    // draggable so the user can give the canvas as much room as they
    // want without losing access to the controls.
    const ImVec2 paneAvail = ImGui::GetContentRegionAvail();
    if (upperPaneHeight <= 0.0f) {
        // First-time init — pick a sensible default that shows the
        // controls + a few table rows by default.
        upperPaneHeight = std::min(420.0f, paneAvail.y * 0.55f);
    }
    constexpr float kMinUpper = 180.0f;
    constexpr float kMinLower = 140.0f;

    ImGui::BeginChild("##cr_detail_upper", ImVec2(0, upperPaneHeight), false);

    // Editable name — diverging from the bound Screen's name breaks
    // autosync in RoutingLibrarySystem's next tick.
    {
        char nameBuf[256];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", asset.name.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##assetname", nameBuf, sizeof(nameBuf))) {
            asset.name = nameBuf;
        }
    }

    if (isAutoBound) {
        ImGui::TextDisabled("Auto-direct routing for a Screen — editing");
        ImGui::TextDisabled("name or targets breaks the autosync link.");
    }

    // Kind selector.
    ImGui::Spacing();
    ImGui::Text("Kind");
    int kindIdx = 0;
    if      (asset.kind == RouteMode::Tiled)   kindIdx = 1;
    else if (asset.kind == RouteMode::FeedMap) kindIdx = 2;
    const char* kindLabels[] = {"Direct", "Tiled", "Feed Map"};
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("##assetkind", &kindIdx, kindLabels, IM_ARRAYSIZE(kindLabels))) {
        RouteMode newKind = RouteMode::Direct;
        if      (kindIdx == 1) newKind = RouteMode::Tiled;
        else if (kindIdx == 2) newKind = RouteMode::FeedMap;
        if (newKind != asset.kind) {
            asset.kind = newKind;
            switch (newKind) {
                case RouteMode::Direct:
                    if (asset.targets.size() > 1) asset.targets.resize(1);
                    if (asset.targets.empty())
                        asset.targets.push_back({entt::null, kIdentityUV});
                    asset.targets[0].uvRect = kIdentityUV;
                    break;
                case RouteMode::Tiled:
                    if (asset.tiledCount < 2) asset.tiledCount = 2;
                    regenerateTiledFromParams(reg, asset);
                    break;
                case RouteMode::FeedMap:
                    // Preserve existing targets — Feed Map just unlocks
                    // per-region names + source-canvas authoring. Start
                    // with one full-frame region if the asset had no
                    // targets at all.
                    if (asset.targets.empty()) {
                        RouteTarget t;
                        t.uvRect = kIdentityUV;
                        t.name = "Region 1";
                        asset.targets.push_back(t);
                    }
                    break;
            }
        }
    }

    // Feed Map extras.
    if (asset.kind == RouteMode::FeedMap) {
        ImGui::Spacing();
        ImGui::Text("Source canvas");
        int sw = static_cast<int>(asset.sourceWidth);
        int sh = static_cast<int>(asset.sourceHeight);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Width##fmw", &sw, 0, 0)) {
            asset.sourceWidth = static_cast<std::uint32_t>(std::max(1, sw));
            view = {};  // aspect change — reset viewport to fit
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Height##fmh", &sh, 0, 0)) {
            asset.sourceHeight = static_cast<std::uint32_t>(std::max(1, sh));
            view = {};
        }
        ImGui::SameLine();
        // Match Selected Clip — explicit re-sync to the currently-
        // selected clip's resolution. Greyed out when no clip is
        // selected or the clip has no reported resolution.
        std::uint32_t clipW = 0, clipH = 0;
        const bool canMatch = getSelectedClipResolution(reg, timeline, clipW, clipH);
        if (!canMatch) ImGui::BeginDisabled();
        if (ImGui::Button("Match Selected Clip##fmmatch")) {
            asset.sourceWidth  = clipW;
            asset.sourceHeight = clipH;
            view = {};
        }
        if (!canMatch) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Select a clip with a known resolution on the timeline.");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Export Template...##fmexp")) {
            std::string outPath;
            if (exportFeedMapSvg(reg, asset, outPath)) {
                std::printf("[ContentRoutingWindow] exported feed-map template -> %s\n",
                             outPath.c_str());
            } else {
                std::printf("[ContentRoutingWindow] failed to write feed-map template\n");
            }
        }
        ImGui::TextDisabled("SVG written under <cwd>/.feed-templates/");

        // Edit in dedicated Feed Map Editor window. The button hides
        // itself if the editor window wasn't wired (e.g. in tests).
        if (feedMapEditor) {
            ImGui::Spacing();
            if (ImGui::Button("Edit in Feed Map Editor...##fm_open",
                                ImVec2(220.0f, 0.0f))) {
                feedMapEditor->editAsset(selected);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(dedicated workspace with big canvas)");
        }
    }

    // Tiled extras.
    if (asset.kind == RouteMode::Tiled) {
        ImGui::Spacing();
        ImGui::Text("Tiled layout");
        int count = static_cast<int>(asset.tiledCount);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderInt("Count##tiledn", &count, 1, 16)) {
            asset.tiledCount = static_cast<std::uint8_t>(std::clamp(count, 1, 16));
            regenerateTiledFromParams(reg, asset);
        }
        int axisIdx = asset.tiledAxis;
        ImGui::Text("Axis");
        ImGui::SameLine();
        if (ImGui::RadioButton("Horizontal##tiledax", axisIdx == 0)) {
            asset.tiledAxis = 0;
            regenerateTiledFromParams(reg, asset);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Vertical##tiledax", axisIdx == 1)) {
            asset.tiledAxis = 1;
            regenerateTiledFromParams(reg, asset);
        }
        ImGui::SameLine();
        if (ImGui::Button("Regenerate##tiledregen")) {
            regenerateTiledFromParams(reg, asset);
        }
    }

    // Selection strip — context for what timeline clip the routing
    // applies to. Placed here so it sits adjacent to the asset's
    // identity controls; the table + canvas come below.
    renderSelectionStrip(reg, timeline);

    // Targets table.
    ImGui::Spacing();
    ImGui::Text("Targets");

    // Build a screen-name array shared across all per-row dropdowns.
    std::vector<entt::entity> screenEntities;
    std::vector<std::string> screenLabels;
    screenEntities.push_back(entt::null);
    screenLabels.emplace_back("(all visible)");
    for (auto [e, s] : reg.view<Screen>().each()) {
        screenEntities.push_back(e);
        screenLabels.push_back(s.name);
    }
    std::vector<const char*> screenLabelCstrs;
    screenLabelCstrs.reserve(screenLabels.size());
    for (const auto& l : screenLabels) screenLabelCstrs.push_back(l.c_str());

    int removeIdx = -1;
    const bool isFeedMap = (asset.kind == RouteMode::FeedMap);
    const int columnCount = isFeedMap ? 7 : 6;
    // Fill the remaining upper-child height, minus space reserved
    // for the "+ Add Region" button below the table. Floor at a few
    // rows so the table is always usable even when the user drags
    // the splitter down.
    const float reservedBelow = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    const float tableMaxH = std::max(80.0f,
        ImGui::GetContentRegionAvail().y - reservedBelow);
    if (ImGui::BeginTable("##routestable", columnCount,
                           ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY,
                           ImVec2(0.0f, tableMaxH))) {
        if (isFeedMap) {
            ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        }
        ImGui::TableSetupColumn("Screen", ImGuiTableColumnFlags_WidthStretch);
        // Feed Map shows pixel coordinates so content creators can match
        // template files directly; Tiled / Direct keep UV floats since
        // they don't carry a source-canvas size.
        const float colWidth = isFeedMap ? 72.0f : 64.0f;
        ImGui::TableSetupColumn(isFeedMap ? "Px X" : "X",
                                  ImGuiTableColumnFlags_WidthFixed, colWidth);
        ImGui::TableSetupColumn(isFeedMap ? "Px Y" : "Y",
                                  ImGuiTableColumnFlags_WidthFixed, colWidth);
        ImGui::TableSetupColumn(isFeedMap ? "Px W" : "W",
                                  ImGuiTableColumnFlags_WidthFixed, colWidth);
        ImGui::TableSetupColumn(isFeedMap ? "Px H" : "H",
                                  ImGuiTableColumnFlags_WidthFixed, colWidth);
        ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();

        const bool uvEditable = (asset.kind == RouteMode::Tiled ||
                                   asset.kind == RouteMode::FeedMap);
        const int colScreen = isFeedMap ? 1 : 0;
        const int colX = colScreen + 1;
        const int colY = colScreen + 2;
        const int colW = colScreen + 3;
        const int colH = colScreen + 4;
        const int colDel = colScreen + 5;

        for (std::size_t i = 0; i < asset.targets.size(); ++i) {
            auto& t = asset.targets[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            // Region name (Feed Map only).
            if (isFeedMap) {
                ImGui::TableSetColumnIndex(0);
                char nameBuf[128];
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", t.name.c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##rname", nameBuf, sizeof(nameBuf))) {
                    t.name = nameBuf;
                }
            }

            // Screen picker.
            ImGui::TableSetColumnIndex(colScreen);
            int screenIdx = 0;
            for (std::size_t k = 0; k < screenEntities.size(); ++k) {
                if (screenEntities[k] == t.screen) {
                    screenIdx = static_cast<int>(k); break;
                }
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##screen", &screenIdx,
                              screenLabelCstrs.data(),
                              static_cast<int>(screenLabelCstrs.size()))) {
                t.screen = screenEntities[static_cast<std::size_t>(screenIdx)];
            }

            // uvRect fields. Feed Map edits in pixels (DragInt scaled by
            // sourceWidth/Height); Tiled / Direct keep UV (DragFloat).
            // Storage stays as float uvRect either way — bus wire
            // format is unchanged.
            auto editAxis = [&](int col, int axis, const char* label,
                                 std::uint32_t pixelDim) {
                ImGui::TableSetColumnIndex(col);
                ImGui::SetNextItemWidth(-1);
                if (!uvEditable) ImGui::BeginDisabled();
                if (isFeedMap && pixelDim > 0) {
                    int px = static_cast<int>(std::lround(
                        t.uvRect[axis] * static_cast<float>(pixelDim)));
                    const int rangeMin = -2 * static_cast<int>(pixelDim);
                    const int rangeMax =  2 * static_cast<int>(pixelDim);
                    if (ImGui::DragInt(label, &px, 1.0f, rangeMin, rangeMax, "%d")) {
                        t.uvRect[axis] = static_cast<float>(px) /
                                          static_cast<float>(pixelDim);
                    }
                } else {
                    ImGui::DragFloat(label, &t.uvRect[axis], 0.005f,
                                      -2.0f, 2.0f, "%.3f");
                }
                if (!uvEditable) ImGui::EndDisabled();
            };
            editAxis(colX, 0, "##x", asset.sourceWidth);
            editAxis(colY, 1, "##y", asset.sourceHeight);
            editAxis(colW, 2, "##w", asset.sourceWidth);
            editAxis(colH, 3, "##h", asset.sourceHeight);

            ImGui::TableSetColumnIndex(colDel);
            if (ImGui::Button("X", ImVec2(-1, 0))) {
                removeIdx = static_cast<int>(i);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeIdx >= 0) {
        asset.targets.erase(asset.targets.begin() + removeIdx);
    }

    if (asset.kind != RouteMode::Direct || asset.targets.empty()) {
        const char* addLabel = (asset.kind == RouteMode::FeedMap)
            ? "+ Add Region##routerow"
            : "+ Add Target##routerow";
        if (ImGui::Button(addLabel)) {
            RouteTarget t;
            t.uvRect = kIdentityUV;
            if (asset.kind == RouteMode::FeedMap) {
                t.name = "Region " + std::to_string(asset.targets.size() + 1);
            }
            asset.targets.push_back(t);
        }
        if (asset.kind == RouteMode::Direct) {
            ImGui::SameLine();
            ImGui::TextDisabled("Direct mode uses the first target only.");
        }
    }

    ImGui::EndChild();  // ##cr_detail_upper

    // Draggable horizontal divider between upper controls and the
    // canvas preview. Resizes upperPaneHeight in place.
    splitterY("##cr_detail_splitter", &upperPaneHeight,
              kMinUpper, kMinLower, paneAvail.y);

    ImGui::BeginChild("##cr_detail_lower", ImVec2(0, 0), false);

    // Canvas preview. Drops the clip's most-recently-uploaded video
    // frame into the background when a clip is selected on the timeline
    // and the renderer is wired (ADR-0022 L5). Falls back to the flat
    // schematic when no clip is selected, the renderer is missing, or
    // the texture isn't uploaded yet.
    ImGui::Text("Canvas");
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit##cr_view_reset")) {
        view = {};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(wheel = zoom, middle-drag = pan)");
    void* texId = nullptr;
    if (renderer && timeline) {
        const entt::entity sel = timeline->getSelectedClip();
        if (sel != entt::null && reg.valid(sel)) {
            if (const auto* vt = reg.try_get<VideoTexture>(sel)) {
                if (vt->isAllocated()) {
                    texId = renderer->getVideoTextureIDForSlot(vt->descriptorSlot);
                }
            }
        }
    }
    // Hand the canvas all remaining vertical space in the lower
    // child. drawContentRoutingCanvas does the Photoshop-style
    // editor-canvas rendering (source floats inside, dark area
    // around it).
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    drawContentRoutingCanvas(reg, asset, texId, dragState, view, avail);

    ImGui::EndChild();  // ##cr_detail_lower
}

// Header strip above the schematic when a clip / generative layer is
// selected in the timeline. Surfaces enough context (filename, source
// resolution) that the user can tell what they're routing without a
// full poster-frame preview (that one needs per-slot video-texture
// access on D3D12Renderer — a follow-up). Reads the timeline's current
// selection passively; UI-only, no registry writes.
void renderSelectionStrip(entt::registry& reg, Timeline* timeline) {
    if (!timeline) return;
    entt::entity sel = timeline->getSelectedClip();
    if (sel == entt::null || !reg.valid(sel)) return;

    const auto* asset = routing::tryGetAsset(reg, sel);
    const char* kindBadge = "Default";
    ImVec4 kindColor{0.7f, 0.7f, 0.7f, 1.0f};
    if (asset) {
        switch (asset->kind) {
            case RouteMode::Direct:
                kindBadge = "Direct";
                kindColor = ImVec4(0.6f, 0.85f, 0.6f, 1.0f); break;
            case RouteMode::Tiled:
                kindBadge = "Tiled";
                kindColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); break;
            case RouteMode::FeedMap:
                kindBadge = "Feed Map";
                kindColor = ImVec4(0.6f, 0.7f, 1.0f, 1.0f); break;
        }
    }

    ImGui::Separator();
    ImGui::TextColored(kindColor, "Routed:");
    ImGui::SameLine();
    if (const auto* clip = reg.try_get<Clip>(sel)) {
        ImGui::Text("%s", clip->filepath.c_str());
        ImGui::TextDisabled("  source %ux%u  %.2f fps  %d frames",
                             clip->width, clip->height, clip->framerate,
                             clip->totalMediaFrames);
    } else if (reg.all_of<GenerativeLayer>(sel)) {
        ImGui::Text("Generative layer (entity %u)",
                     static_cast<std::uint32_t>(sel));
    } else {
        ImGui::TextDisabled("(non-content layer)");
    }
    ImGui::SameLine();
    ImGui::TextColored(kindColor, "[%s]", kindBadge);
}

void renderDeleteConfirm(entt::registry& reg, entt::entity& pendingDelete,
                          entt::entity& selected) {
    if (pendingDelete == entt::null) return;
    if (!reg.valid(pendingDelete) ||
        !reg.all_of<ContentRoutingAsset>(pendingDelete)) {
        pendingDelete = entt::null;
        return;
    }

    ImGui::OpenPopup("Delete routing?");
    if (ImGui::BeginPopupModal("Delete routing?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& a = reg.get<ContentRoutingAsset>(pendingDelete);
        const int usage = countUsage(reg, pendingDelete);
        ImGui::Text("Delete \"%s\"?", a.name.c_str());
        if (usage > 0) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                "Used by %d clip%s — their routings will fall back to Default (All).",
                                usage, usage == 1 ? "" : "s");
        }
        if (a.autoBoundScreen != entt::null) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f),
                                "Auto-direct asset — will be regenerated on next tick.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            clearRefsTo(reg, pendingDelete);
            if (selected == pendingDelete) selected = entt::null;
            reg.destroy(pendingDelete);
            pendingDelete = entt::null;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            pendingDelete = entt::null;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

ContentRoutingWindow::ContentRoutingWindow(Timeline* timeline)
    : m_timeline(timeline) {}

void ContentRoutingWindow::render() {
    if (!m_timeline) {
        ImGui::TextDisabled("No timeline.");
        return;
    }
    auto& registry = m_timeline->getRegistry();

    // Clamp m_selectedAsset to a valid asset entity (covers asset
    // destruction from another code path — e.g. RoutingLibrarySystem
    // cascade-deleting an auto-direct entry when its Screen is removed).
    if (m_selectedAsset != entt::null &&
        (!registry.valid(m_selectedAsset) ||
         !registry.all_of<ContentRoutingAsset>(m_selectedAsset))) {
        m_selectedAsset = entt::null;
    }

    const float leftWidth = 240.0f;
    ImGui::BeginChild("##cr_left", ImVec2(leftWidth, 0), true);
    renderLeftPane(registry, m_timeline, m_selectedAsset, m_pendingDelete);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##cr_right", ImVec2(0, 0), true);
    // renderDetailPane now owns the selection-strip placement (above
    // the canvas) so the canvas can take all remaining vertical space.
    renderDetailPane(registry, m_selectedAsset, m_renderer, m_timeline,
                      m_canvasDrag, m_canvasView, m_feedMapEditor,
                      m_upperPaneHeight);
    ImGui::EndChild();

    renderDeleteConfirm(registry, m_pendingDelete, m_selectedAsset);
}

} // namespace entity
