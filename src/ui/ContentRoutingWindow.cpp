#include "entity/ui/ContentRoutingWindow.hpp"

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
#include <sstream>
#include <string>
#include <vector>

namespace entity {

namespace {

// Identity uvRect — the "fills the whole source" sentinel.
constexpr std::array<float, 4> kIdentityUV{0.0f, 0.0f, 1.0f, 1.0f};

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

// Draw the routing canvas (ADR-0022 L5). Replaces the L2 drawSchematic.
// Background is the clip's most-recently-uploaded video frame when
// `texId` is non-null; otherwise a flat dark-grey panel. Region
// overlay + labels stay similar to L2, plus corner handles for FeedMap
// and Tiled. Interaction is via a single canvas-wide InvisibleButton
// with manual mouse hit-testing; drag state lives on the window across
// frames. Mutates `asset.targets[i].uvRect` during drag — caller must
// pass a mutable reference.
void drawCanvas(entt::registry& reg,
                ContentRoutingAsset& asset,
                void* texId,
                ContentRoutingWindow::CanvasDragState& dragState) {
    const float aspect = (asset.sourceHeight > 0)
        ? static_cast<float>(asset.sourceWidth) / static_cast<float>(asset.sourceHeight)
        : (16.0f / 9.0f);
    const float canvasW = std::min(ImGui::GetContentRegionAvail().x, 400.0f);
    const float canvasH = canvasW / std::max(0.1f, aspect);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 bgMax(origin.x + canvasW, origin.y + canvasH);
    auto* dl = ImGui::GetWindowDrawList();

    // Background: poster-frame texture when available, else flat panel.
    if (texId) {
        dl->AddImage(reinterpret_cast<ImTextureID>(texId), origin, bgMax);
    } else {
        dl->AddRectFilled(origin, bgMax, IM_COL32(40, 40, 50, 255));
    }
    dl->AddRect(origin, bgMax, IM_COL32(120, 120, 130, 255));

    // Deterministic per-index hues. Translucent fills, opaque handles.
    static const ImU32 fillHues[] = {
        IM_COL32(220, 100, 100, 90),
        IM_COL32(100, 200, 130, 90),
        IM_COL32(100, 140, 220, 90),
        IM_COL32(230, 200, 100, 90),
        IM_COL32(200, 110, 220, 90),
        IM_COL32(120, 220, 220, 90),
    };
    static const ImU32 strokeHues[] = {
        IM_COL32(220, 100, 100, 230),
        IM_COL32(100, 200, 130, 230),
        IM_COL32(100, 140, 220, 230),
        IM_COL32(230, 200, 100, 230),
        IM_COL32(200, 110, 220, 230),
        IM_COL32(120, 220, 220, 230),
    };
    constexpr int kHueCount = static_cast<int>(sizeof(fillHues) / sizeof(fillHues[0]));

    const bool handlesVisible = (asset.kind != RouteMode::Direct);

    struct RegionRect { float x0, y0, x1, y1; };
    std::vector<RegionRect> rects(asset.targets.size());

    // Pass 1: per-region fill + outline + labels.
    for (std::size_t i = 0; i < asset.targets.size(); ++i) {
        const auto& t = asset.targets[i];
        const float x0 = origin.x + t.uvRect[0] * canvasW;
        const float y0 = origin.y + t.uvRect[1] * canvasH;
        const float x1 = x0 + t.uvRect[2] * canvasW;
        const float y1 = y0 + t.uvRect[3] * canvasH;
        rects[i] = {x0, y0, x1, y1};

        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fillHues[i % kHueCount]);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                    IM_COL32(240, 240, 240, 200), 0.0f, 0, 2.0f);

        std::string screenName = "(all visible)";
        if (t.screen != entt::null && reg.valid(t.screen) &&
            reg.all_of<Screen>(t.screen)) {
            screenName = reg.get<Screen>(t.screen).name;
        }
        if (!t.name.empty()) {
            dl->AddText(ImVec2(x0 + 4.0f, y0 + 4.0f),
                        IM_COL32(255, 255, 255, 240),
                        t.name.c_str());
            // ASCII "->" so ImGui's default font (no U+2192) doesn't
            // render the missing-glyph "?" placeholder.
            std::string sub = std::string("-> ") + screenName;
            dl->AddText(ImVec2(x0 + 4.0f, y0 + 20.0f),
                        IM_COL32(220, 220, 220, 220),
                        sub.c_str());
        } else {
            dl->AddText(ImVec2(x0 + 4.0f, y0 + 4.0f),
                        IM_COL32(255, 255, 255, 230),
                        screenName.c_str());
        }
    }

    // Pass 2: corner handles on top (FeedMap + Tiled only).
    if (handlesVisible) {
        constexpr float radius = 5.0f;
        for (std::size_t i = 0; i < asset.targets.size(); ++i) {
            const auto& r = rects[i];
            const ImU32 fill = strokeHues[i % kHueCount];
            const ImVec2 corners[4] = {
                {r.x0, r.y0}, {r.x1, r.y0}, {r.x0, r.y1}, {r.x1, r.y1}
            };
            for (const auto& c : corners) {
                dl->AddCircleFilled(c, radius, fill);
                dl->AddCircle(c, radius, IM_COL32(255, 255, 255, 230), 0, 1.5f);
            }
        }
    }

    // Canvas-wide invisible button drives mouse interaction. Drawn last
    // so it sits on top of the schematic but accepts clicks across the
    // whole canvas; per-region hit-testing is manual below.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##cr_canvas", ImVec2(canvasW, canvasH),
                            ImGuiButtonFlags_MouseButtonLeft);
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // Hit-test: corner handles win over bodies; later-drawn (higher
    // index) wins on body overlap so the topmost region is grabbable.
    auto hitTest = [&](int& outRegion, int& outHandle) {
        outRegion = -1;
        outHandle = 0;
        constexpr float kCornerHit = 8.0f;
        if (handlesVisible) {
            for (int i = static_cast<int>(asset.targets.size()) - 1; i >= 0; --i) {
                const auto& r = rects[i];
                auto over = [&](float cx, float cy) {
                    return mouse.x >= cx - kCornerHit && mouse.x <= cx + kCornerHit &&
                           mouse.y >= cy - kCornerHit && mouse.y <= cy + kCornerHit;
                };
                if (over(r.x0, r.y0)) { outRegion = i; outHandle = 1; return; }
                if (over(r.x1, r.y0)) { outRegion = i; outHandle = 2; return; }
                if (over(r.x0, r.y1)) { outRegion = i; outHandle = 3; return; }
                if (over(r.x1, r.y1)) { outRegion = i; outHandle = 4; return; }
            }
        }
        for (int i = static_cast<int>(asset.targets.size()) - 1; i >= 0; --i) {
            const auto& r = rects[i];
            if (mouse.x >= r.x0 && mouse.x <= r.x1 &&
                mouse.y >= r.y0 && mouse.y <= r.y1) {
                outRegion = i;
                outHandle = 0;
                return;
            }
        }
    };

    if (active) {
        // Begin drag on the frame the mouse goes down.
        if (dragState.regionIdx < 0) {
            int r = -1, h = 0;
            hitTest(r, h);
            dragState.regionIdx = r;
            dragState.handle    = h;
        }
        // Apply mouse delta. Handle == 0 moves the whole region;
        // 1-4 resize from NW/NE/SW/SE respectively.
        if (dragState.regionIdx >= 0 &&
            dragState.regionIdx < static_cast<int>(asset.targets.size())) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            const float dx = d.x / canvasW;
            const float dy = d.y / canvasH;
            auto& uv = asset.targets[dragState.regionIdx].uvRect;
            switch (dragState.handle) {
                case 0: uv[0] += dx; uv[1] += dy; break;
                case 1: uv[0] += dx; uv[2] -= dx; uv[1] += dy; uv[3] -= dy; break;
                case 2: uv[2] += dx;             uv[1] += dy; uv[3] -= dy; break;
                case 3: uv[0] += dx; uv[2] -= dx;             uv[3] += dy; break;
                case 4: uv[2] += dx;                          uv[3] += dy; break;
            }
            // Keep width / height positive so the rect doesn't flip.
            uv[2] = std::max(0.01f, uv[2]);
            uv[3] = std::max(0.01f, uv[3]);
        }
    } else {
        dragState.regionIdx = -1;
    }

    // Cursor change + tooltip when hovering (not actively dragging the
    // wrong thing — IsItemActive already covers the active case).
    if (hovered) {
        int hr = -1, hh = 0;
        hitTest(hr, hh);
        if (hr >= 0) {
            if (hh == 0)                 ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            else if (hh == 1 || hh == 4) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            else                          ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);

            const auto& t = asset.targets[hr];
            if (asset.kind == RouteMode::FeedMap &&
                asset.sourceWidth > 0 && asset.sourceHeight > 0) {
                const int px = static_cast<int>(std::lround(t.uvRect[0] * asset.sourceWidth));
                const int py = static_cast<int>(std::lround(t.uvRect[1] * asset.sourceHeight));
                const int pw = static_cast<int>(std::lround(t.uvRect[2] * asset.sourceWidth));
                const int ph = static_cast<int>(std::lround(t.uvRect[3] * asset.sourceHeight));
                ImGui::SetTooltip("%s\n%d,%d  %dx%d px",
                                   t.name.empty() ? "(region)" : t.name.c_str(),
                                   px, py, pw, ph);
            } else {
                ImGui::SetTooltip("uv: %.3f,%.3f  %.3fx%.3f",
                                   t.uvRect[0], t.uvRect[1],
                                   t.uvRect[2], t.uvRect[3]);
            }
        }
    }
}

void renderLeftPane(entt::registry& reg, entt::entity& selected,
                     entt::entity& pendingDelete) {
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
            a.sourceWidth  = 1920;
            a.sourceHeight = 1080;
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

void renderDetailPane(entt::registry& reg, entt::entity selected,
                       IRenderer* renderer, Timeline* timeline,
                       ContentRoutingWindow::CanvasDragState& dragState) {
    if (selected == entt::null || !reg.valid(selected) ||
        !reg.all_of<ContentRoutingAsset>(selected)) {
        ImGui::TextDisabled("Select a routing from the library.");
        return;
    }
    auto& asset = reg.get<ContentRoutingAsset>(selected);
    const bool isAutoBound = (asset.autoBoundScreen != entt::null);

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
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Height##fmh", &sh, 0, 0)) {
            asset.sourceHeight = static_cast<std::uint32_t>(std::max(1, sh));
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
    if (ImGui::BeginTable("##routestable", columnCount,
                           ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
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

    // Canvas preview. Drops the clip's most-recently-uploaded video
    // frame into the background when a clip is selected on the timeline
    // and the renderer is wired (ADR-0022 L5). Falls back to the flat
    // schematic when no clip is selected, the renderer is missing, or
    // the texture isn't uploaded yet.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Canvas");
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
    drawCanvas(reg, asset, texId, dragState);
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
    renderLeftPane(registry, m_selectedAsset, m_pendingDelete);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##cr_right", ImVec2(0, 0), true);
    renderDetailPane(registry, m_selectedAsset, m_renderer, m_timeline, m_canvasDrag);
    renderSelectionStrip(registry, m_timeline);
    ImGui::EndChild();

    renderDeleteConfirm(registry, m_pendingDelete, m_selectedAsset);
}

} // namespace entity
