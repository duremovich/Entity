#include "entity/ui/ContentRoutingCanvas.hpp"

#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/Screen.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace entity {

// Photoshop/Figma-style editor canvas. The available pane IS the
// editor surface (dark "infinite canvas" background). The source
// canvas (asset.sourceWidth x asset.sourceHeight) floats inside,
// positioned by view.panScreen and scaled by view.zoom. Mouse-wheel
// zooms anchored at cursor; middle-drag pans the source within the
// editor with no clamping (user can park the source partway off
// the editor edge). Pixel grid kicks in when each source pixel
// covers >=8 screen pixels (i.e. view.zoom >= 8). Drag/snap/handles
// all operate in screen-pixel space via the uvToScreen/screenToUV
// helpers below.
//
// Shared between ContentRoutingWindow's small preview and the
// FeedMapEditorWindow's full-window editor. Both pass their
// available size as availSize; the editor's huge availSize is what
// makes the precision editing usable.

void drawContentRoutingCanvas(entt::registry& reg,
                               ContentRoutingAsset& asset,
                               void* texId,
                               ContentRoutingWindow::CanvasDragState& dragState,
                               ContentRoutingWindow::CanvasViewport& view,
                               ImVec2 availSize) {
    using CanvasHandle = ContentRoutingWindow::CanvasHandle;

    // Editor canvas = available pane. Floor at a tiny min so a
    // collapsed pane doesn't break the math.
    const float editorW = std::max(80.0f, availSize.x);
    const float editorH = std::max(60.0f, availSize.y);

    // Wrap in BeginChild so wheel events stay on the canvas (don't
    // bubble to a parent scroll), and so ImGui clips drawing to the
    // editor rect for free.
    ImGui::BeginChild("##cr_canvas_child",
                       ImVec2(editorW, editorH), false,
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 editorMin = ImGui::GetCursorScreenPos();
    const ImVec2 editorMax(editorMin.x + editorW, editorMin.y + editorH);
    auto* dl = ImGui::GetWindowDrawList();

    // Editor background — dark "infinite canvas" + faint border.
    dl->AddRectFilled(editorMin, editorMax, IM_COL32(28, 28, 33, 255));
    dl->AddRect(editorMin, editorMax, IM_COL32(80, 80, 90, 255));

    // Source dimensions (in source pixels). Floor at 1 to avoid /0.
    const float srcW = std::max(1.0f, static_cast<float>(asset.sourceWidth));
    const float srcH = std::max(1.0f, static_cast<float>(asset.sourceHeight));

    // Auto-fit on first draw (zoom == 0 sentinel). 90% of fit gives
    // a small margin so the source isn't flush against the editor edge.
    if (view.zoom <= 0.0f) {
        const float fitZoom = std::min(editorW / srcW, editorH / srcH) * 0.9f;
        view.zoom = std::max(0.001f, fitZoom);
        view.panScreen.x = (editorW - srcW * view.zoom) * 0.5f;
        view.panScreen.y = (editorH - srcH * view.zoom) * 0.5f;
    }

    // Source rect on screen.
    const float srcScreenW = srcW * view.zoom;
    const float srcScreenH = srcH * view.zoom;
    const ImVec2 srcMin(editorMin.x + view.panScreen.x,
                        editorMin.y + view.panScreen.y);
    const ImVec2 srcMax(srcMin.x + srcScreenW, srcMin.y + srcScreenH);

    // uv (0..1 within source) <-> screen coords.
    auto uvToScreen = [&](float ux, float uy) -> ImVec2 {
        return ImVec2(srcMin.x + ux * srcScreenW, srcMin.y + uy * srcScreenH);
    };
    auto screenToUV = [&](ImVec2 s) -> ImVec2 {
        return ImVec2((s.x - srcMin.x) / srcScreenW,
                      (s.y - srcMin.y) / srcScreenH);
    };

    // Source rectangle background — poster-frame texture or flat
    // panel. BeginChild's clip rect handles edge clipping for free.
    if (texId) {
        dl->AddImage(reinterpret_cast<ImTextureID>(texId), srcMin, srcMax);
    } else {
        dl->AddRectFilled(srcMin, srcMax, IM_COL32(40, 40, 50, 255));
    }
    dl->AddRect(srcMin, srcMax, IM_COL32(160, 160, 170, 255), 0.0f, 0, 1.5f);

    // Pixel grid: faint 1px lines on every integer source-pixel
    // boundary when each source pixel covers >=8 screen pixels.
    if (asset.sourceWidth > 0 && asset.sourceHeight > 0 && view.zoom >= 8.0f) {
        const int sx0 = std::max(0,
            static_cast<int>(std::floor((editorMin.x - srcMin.x) / view.zoom)));
        const int sx1 = std::min(static_cast<int>(asset.sourceWidth),
            static_cast<int>(std::ceil((editorMax.x - srcMin.x) / view.zoom)));
        const int sy0 = std::max(0,
            static_cast<int>(std::floor((editorMin.y - srcMin.y) / view.zoom)));
        const int sy1 = std::min(static_cast<int>(asset.sourceHeight),
            static_cast<int>(std::ceil((editorMax.y - srcMin.y) / view.zoom)));
        if ((sx1 - sx0) <= 1000 && (sy1 - sy0) <= 1000) {
            const ImU32 gridCol = IM_COL32(255, 255, 255, 32);
            const float yTop = std::max(srcMin.y, editorMin.y);
            const float yBot = std::min(srcMax.y, editorMax.y);
            for (int x = sx0; x <= sx1; ++x) {
                const float xs = srcMin.x + x * view.zoom;
                dl->AddLine(ImVec2(xs, yTop), ImVec2(xs, yBot), gridCol, 1.0f);
            }
            const float xL = std::max(srcMin.x, editorMin.x);
            const float xR = std::min(srcMax.x, editorMax.x);
            for (int y = sy0; y <= sy1; ++y) {
                const float ys = srcMin.y + y * view.zoom;
                dl->AddLine(ImVec2(xL, ys), ImVec2(xR, ys), gridCol, 1.0f);
            }
        }
    }

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
    const bool pixelSnapEnabled = (asset.kind == RouteMode::FeedMap) &&
                                    asset.sourceWidth > 0 && asset.sourceHeight > 0;

    struct RegionRect { float x0, y0, x1, y1; };
    std::vector<RegionRect> rects(asset.targets.size());

    // Pass 1: per-region fill + outline + labels.
    for (std::size_t i = 0; i < asset.targets.size(); ++i) {
        const auto& t = asset.targets[i];
        const ImVec2 tl = uvToScreen(t.uvRect[0], t.uvRect[1]);
        const ImVec2 br = uvToScreen(t.uvRect[0] + t.uvRect[2],
                                      t.uvRect[1] + t.uvRect[3]);
        rects[i] = {tl.x, tl.y, br.x, br.y};

        dl->AddRectFilled(ImVec2(tl.x, tl.y), ImVec2(br.x, br.y), fillHues[i % kHueCount]);
        dl->AddRect(ImVec2(tl.x, tl.y), ImVec2(br.x, br.y),
                    IM_COL32(240, 240, 240, 200), 0.0f, 0, 2.0f);

        std::string screenName = "(all visible)";
        if (t.screen != entt::null && reg.valid(t.screen) &&
            reg.all_of<Screen>(t.screen)) {
            screenName = reg.get<Screen>(t.screen).name;
        }
        if (!t.name.empty()) {
            dl->AddText(ImVec2(tl.x + 4.0f, tl.y + 4.0f),
                        IM_COL32(255, 255, 255, 240),
                        t.name.c_str());
            std::string sub = std::string("-> ") + screenName;
            dl->AddText(ImVec2(tl.x + 4.0f, tl.y + 20.0f),
                        IM_COL32(220, 220, 220, 220),
                        sub.c_str());
        } else {
            dl->AddText(ImVec2(tl.x + 4.0f, tl.y + 4.0f),
                        IM_COL32(255, 255, 255, 230),
                        screenName.c_str());
        }
    }

    // Pass 2: handles. Corners as filled circles, edge midpoints as
    // 8x8 filled squares.
    if (handlesVisible) {
        constexpr float cornerR = 5.0f;
        constexpr float edgeHalf = 4.0f;
        for (std::size_t i = 0; i < asset.targets.size(); ++i) {
            const auto& r = rects[i];
            const ImU32 fill = strokeHues[i % kHueCount];
            const ImVec2 corners[4] = {
                {r.x0, r.y0}, {r.x1, r.y0}, {r.x0, r.y1}, {r.x1, r.y1}
            };
            for (const auto& c : corners) {
                dl->AddCircleFilled(c, cornerR, fill);
                dl->AddCircle(c, cornerR, IM_COL32(255, 255, 255, 230), 0, 1.5f);
            }
            const float mx = (r.x0 + r.x1) * 0.5f;
            const float my = (r.y0 + r.y1) * 0.5f;
            const ImVec2 edges[4] = {
                {mx, r.y0}, {r.x1, my}, {mx, r.y1}, {r.x0, my}
            };
            for (const auto& e : edges) {
                dl->AddRectFilled(ImVec2(e.x - edgeHalf, e.y - edgeHalf),
                                   ImVec2(e.x + edgeHalf, e.y + edgeHalf), fill);
                dl->AddRect(ImVec2(e.x - edgeHalf, e.y - edgeHalf),
                             ImVec2(e.x + edgeHalf, e.y + edgeHalf),
                             IM_COL32(255, 255, 255, 230), 0.0f, 0, 1.5f);
            }
        }
    }

    // Editor-wide invisible button. Accepts left (drag regions) and
    // middle (pan source).
    ImGui::SetCursorScreenPos(editorMin);
    ImGui::InvisibleButton("##cr_canvas", ImVec2(editorW, editorH),
                            ImGuiButtonFlags_MouseButtonLeft |
                            ImGuiButtonFlags_MouseButtonMiddle);
    const bool itemActive = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool leftDragging = itemActive && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool middlePanning = itemActive && ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    // Wheel zoom anchored at cursor. Compute the source-pixel point
    // under the cursor before zoom; after zoom, shift panScreen so
    // that same source-pixel point still lands under the cursor.
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const float oldZoom = view.zoom;
            const float factor = (wheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
            const float newZoom = std::clamp(oldZoom * factor, 0.02f, 128.0f);
            const float srcX = (mouse.x - editorMin.x - view.panScreen.x) / oldZoom;
            const float srcY = (mouse.y - editorMin.y - view.panScreen.y) / oldZoom;
            view.zoom = newZoom;
            view.panScreen.x = mouse.x - editorMin.x - srcX * newZoom;
            view.panScreen.y = mouse.y - editorMin.y - srcY * newZoom;
        }
    }

    // Middle-drag pans the source within the editor. No clamping —
    // user can park the source partway off the editor edge.
    if (middlePanning) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        view.panScreen.x += d.x;
        view.panScreen.y += d.y;
    }

    auto hitTest = [&](int& outRegion, CanvasHandle& outHandle) {
        outRegion = -1;
        outHandle = CanvasHandle::Body;
        auto over = [&](float cx, float cy) {
            constexpr float kHandleHit = 8.0f;
            return mouse.x >= cx - kHandleHit && mouse.x <= cx + kHandleHit &&
                   mouse.y >= cy - kHandleHit && mouse.y <= cy + kHandleHit;
        };
        if (handlesVisible) {
            for (int i = static_cast<int>(asset.targets.size()) - 1; i >= 0; --i) {
                const auto& r = rects[i];
                if (over(r.x0, r.y0)) { outRegion = i; outHandle = CanvasHandle::NW; return; }
                if (over(r.x1, r.y0)) { outRegion = i; outHandle = CanvasHandle::NE; return; }
                if (over(r.x0, r.y1)) { outRegion = i; outHandle = CanvasHandle::SW; return; }
                if (over(r.x1, r.y1)) { outRegion = i; outHandle = CanvasHandle::SE; return; }
            }
            for (int i = static_cast<int>(asset.targets.size()) - 1; i >= 0; --i) {
                const auto& r = rects[i];
                const float mx = (r.x0 + r.x1) * 0.5f;
                const float my = (r.y0 + r.y1) * 0.5f;
                if (over(mx,  r.y0)) { outRegion = i; outHandle = CanvasHandle::N; return; }
                if (over(r.x1, my))  { outRegion = i; outHandle = CanvasHandle::E; return; }
                if (over(mx,  r.y1)) { outRegion = i; outHandle = CanvasHandle::S; return; }
                if (over(r.x0, my))  { outRegion = i; outHandle = CanvasHandle::W; return; }
            }
        }
        for (int i = static_cast<int>(asset.targets.size()) - 1; i >= 0; --i) {
            const auto& r = rects[i];
            if (mouse.x >= r.x0 && mouse.x <= r.x1 &&
                mouse.y >= r.y0 && mouse.y <= r.y1) {
                outRegion = i;
                outHandle = CanvasHandle::Body;
                return;
            }
        }
    };

    struct SnapGuide { bool xActive{false}; float xUV{0}; bool yActive{false}; float yUV{0}; };
    SnapGuide guide;

    if (leftDragging) {
        if (dragState.regionIdx < 0) {
            int r = -1;
            CanvasHandle h = CanvasHandle::Body;
            hitTest(r, h);
            dragState.regionIdx = r;
            dragState.handle    = h;
        }
        if (dragState.regionIdx >= 0 &&
            dragState.regionIdx < static_cast<int>(asset.targets.size())) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            // Convert screen-pixel delta to UV — srcScreenW pixels span
            // the source's UV [0,1].
            const float dx = d.x / srcScreenW;
            const float dy = d.y / srcScreenH;
            auto& uv = asset.targets[dragState.regionIdx].uvRect;
            const CanvasHandle h = dragState.handle;
            switch (h) {
                case CanvasHandle::Body: uv[0] += dx; uv[1] += dy; break;
                case CanvasHandle::NW:   uv[0] += dx; uv[2] -= dx; uv[1] += dy; uv[3] -= dy; break;
                case CanvasHandle::N:                              uv[1] += dy; uv[3] -= dy; break;
                case CanvasHandle::NE:   uv[2] += dx;              uv[1] += dy; uv[3] -= dy; break;
                case CanvasHandle::W:    uv[0] += dx; uv[2] -= dx; break;
                case CanvasHandle::E:    uv[2] += dx; break;
                case CanvasHandle::SW:   uv[0] += dx; uv[2] -= dx;              uv[3] += dy; break;
                case CanvasHandle::S:                                            uv[3] += dy; break;
                case CanvasHandle::SE:   uv[2] += dx;                            uv[3] += dy; break;
            }
            uv[2] = std::max(0.01f, uv[2]);
            uv[3] = std::max(0.01f, uv[3]);

            // Region snap — 8 screen pixels in UV space. Tightens with
            // zoom (snap shrinks as user zooms in for precision).
            const float thresholdX = 8.0f / srcScreenW;
            const float thresholdY = 8.0f / srcScreenH;
            std::vector<float> candX{0.0f, 1.0f};
            std::vector<float> candY{0.0f, 1.0f};
            for (std::size_t i = 0; i < asset.targets.size(); ++i) {
                if (static_cast<int>(i) == dragState.regionIdx) continue;
                const auto& other = asset.targets[i].uvRect;
                candX.push_back(other[0]);
                candX.push_back(other[0] + other[2]);
                candY.push_back(other[1]);
                candY.push_back(other[1] + other[3]);
            }
            auto findNearest = [](float v, const std::vector<float>& cand,
                                   float thresh) -> std::optional<float> {
                float best = thresh;
                std::optional<float> result;
                for (float c : cand) {
                    float dd = std::abs(c - v);
                    if (dd < best) { best = dd; result = c; }
                }
                return result;
            };
            auto snapLeft = [&]() {
                if (auto t = findNearest(uv[0], candX, thresholdX)) {
                    const float oldRight = uv[0] + uv[2];
                    uv[0] = *t;
                    uv[2] = oldRight - uv[0];
                    guide.xActive = true; guide.xUV = *t;
                }
            };
            auto snapRight = [&]() {
                const float right = uv[0] + uv[2];
                if (auto t = findNearest(right, candX, thresholdX)) {
                    uv[2] = *t - uv[0];
                    guide.xActive = true; guide.xUV = *t;
                }
            };
            auto snapTop = [&]() {
                if (auto t = findNearest(uv[1], candY, thresholdY)) {
                    const float oldBot = uv[1] + uv[3];
                    uv[1] = *t;
                    uv[3] = oldBot - uv[1];
                    guide.yActive = true; guide.yUV = *t;
                }
            };
            auto snapBot = [&]() {
                const float bot = uv[1] + uv[3];
                if (auto t = findNearest(bot, candY, thresholdY)) {
                    uv[3] = *t - uv[1];
                    guide.yActive = true; guide.yUV = *t;
                }
            };
            auto snapTranslateX = [&]() {
                const float left = uv[0];
                const float right = uv[0] + uv[2];
                auto leftSnap = findNearest(left, candX, thresholdX);
                auto rightSnap = findNearest(right, candX, thresholdX);
                const float dL = leftSnap
                    ? std::abs(*leftSnap - left)
                    : std::numeric_limits<float>::infinity();
                const float dR = rightSnap
                    ? std::abs(*rightSnap - right)
                    : std::numeric_limits<float>::infinity();
                if (leftSnap && dL <= dR) {
                    uv[0] += *leftSnap - left;
                    guide.xActive = true; guide.xUV = *leftSnap;
                } else if (rightSnap) {
                    uv[0] += *rightSnap - right;
                    guide.xActive = true; guide.xUV = *rightSnap;
                }
            };
            auto snapTranslateY = [&]() {
                const float top = uv[1];
                const float bot = uv[1] + uv[3];
                auto topSnap = findNearest(top, candY, thresholdY);
                auto botSnap = findNearest(bot, candY, thresholdY);
                const float dT = topSnap
                    ? std::abs(*topSnap - top)
                    : std::numeric_limits<float>::infinity();
                const float dB = botSnap
                    ? std::abs(*botSnap - bot)
                    : std::numeric_limits<float>::infinity();
                if (topSnap && dT <= dB) {
                    uv[1] += *topSnap - top;
                    guide.yActive = true; guide.yUV = *topSnap;
                } else if (botSnap) {
                    uv[1] += *botSnap - bot;
                    guide.yActive = true; guide.yUV = *botSnap;
                }
            };
            switch (h) {
                case CanvasHandle::Body: snapTranslateX(); snapTranslateY(); break;
                case CanvasHandle::NW:   snapLeft();  snapTop(); break;
                case CanvasHandle::N:                  snapTop(); break;
                case CanvasHandle::NE:   snapRight(); snapTop(); break;
                case CanvasHandle::W:    snapLeft(); break;
                case CanvasHandle::E:    snapRight(); break;
                case CanvasHandle::SW:   snapLeft();  snapBot(); break;
                case CanvasHandle::S:                  snapBot(); break;
                case CanvasHandle::SE:   snapRight(); snapBot(); break;
            }
            uv[2] = std::max(0.01f, uv[2]);
            uv[3] = std::max(0.01f, uv[3]);

            if (pixelSnapEnabled) {
                const float sw = static_cast<float>(asset.sourceWidth);
                const float sh = static_cast<float>(asset.sourceHeight);
                auto snapPxX = [sw](float v) { return std::round(v * sw) / sw; };
                auto snapPxY = [sh](float v) { return std::round(v * sh) / sh; };
                const float minW = 1.0f / sw;
                const float minH = 1.0f / sh;
                if (h == CanvasHandle::Body) {
                    uv[0] = snapPxX(uv[0]);
                    uv[1] = snapPxY(uv[1]);
                } else {
                    struct EdgeFlags { bool L=false, R=false, T=false, B=false; };
                    EdgeFlags ef;
                    switch (h) {
                        case CanvasHandle::NW: ef = {true,  false, true,  false}; break;
                        case CanvasHandle::N:  ef = {false, false, true,  false}; break;
                        case CanvasHandle::NE: ef = {false, true,  true,  false}; break;
                        case CanvasHandle::E:  ef = {false, true,  false, false}; break;
                        case CanvasHandle::SE: ef = {false, true,  false, true};  break;
                        case CanvasHandle::S:  ef = {false, false, false, true};  break;
                        case CanvasHandle::SW: ef = {true,  false, false, true};  break;
                        case CanvasHandle::W:  ef = {true,  false, false, false}; break;
                        case CanvasHandle::Body: break;
                    }
                    float left = uv[0], right = uv[0] + uv[2];
                    if (ef.L) left  = snapPxX(left);
                    if (ef.R) right = snapPxX(right);
                    uv[0] = left;
                    uv[2] = std::max(minW, right - left);
                    float top = uv[1], bot = uv[1] + uv[3];
                    if (ef.T) top = snapPxY(top);
                    if (ef.B) bot = snapPxY(bot);
                    uv[1] = top;
                    uv[3] = std::max(minH, bot - top);
                }
            }
        }
    } else {
        dragState.regionIdx = -1;
    }

    // Snap guides — cyan line through the engaged snap target.
    if (guide.xActive) {
        const float xs = uvToScreen(guide.xUV, 0.0f).x;
        if (xs >= editorMin.x && xs <= editorMax.x) {
            dl->AddLine(ImVec2(xs, editorMin.y), ImVec2(xs, editorMax.y),
                         IM_COL32(80, 220, 255, 200), 1.0f);
        }
    }
    if (guide.yActive) {
        const float ys = uvToScreen(0.0f, guide.yUV).y;
        if (ys >= editorMin.y && ys <= editorMax.y) {
            dl->AddLine(ImVec2(editorMin.x, ys), ImVec2(editorMax.x, ys),
                         IM_COL32(80, 220, 255, 200), 1.0f);
        }
    }

    // Cursor + tooltip — suppress while panning so the cursor stays
    // as the system arrow.
    if (hovered && !middlePanning) {
        int hr = -1;
        CanvasHandle hh = CanvasHandle::Body;
        hitTest(hr, hh);
        if (hr >= 0) {
            switch (hh) {
                case CanvasHandle::Body:
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;
                case CanvasHandle::NW: case CanvasHandle::SE:
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
                case CanvasHandle::NE: case CanvasHandle::SW:
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
                case CanvasHandle::N: case CanvasHandle::S:
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
                case CanvasHandle::E: case CanvasHandle::W:
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
            }
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

    ImGui::EndChild();
}

} // namespace entity
