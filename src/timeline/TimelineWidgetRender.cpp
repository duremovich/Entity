/**
 * TimelineWidget drawing code.
 *
 * Split from TimelineWidget.cpp (Phase B #16). All render*() methods live here;
 * input/hit-testing lives in TimelineWidgetInput.cpp; core dispatch + geometry
 * helpers stay in TimelineWidget.cpp.
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/timeline/CueTag.hpp"
#include "entity/ui/MathInput.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/ScaleLock.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/LayerTrackUiState.hpp"
#include "entity/components/TrackUiState.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/effects/EffectKind.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <limits>
#include <algorithm>

namespace entity {

namespace {

// File-local mirror of the same helper in TimelineWidgetInput.cpp.
// Keep in sync with TimelineWidgetInput findClipIndices — same iteration
// order and same registry access pattern.
// Locate (trackIndex, layerIndex) for an entity in a track's layers vector.
// Returns nullopt when not found. Used by renderClipPropertyPanel for command
// dispatch; kept local because consolidation is a separate cleanup.
std::optional<std::pair<int,int>>
renderFindClipIndices(Timeline* timeline, entt::entity entity) {
    if (!timeline || entity == entt::null) return std::nullopt;
    auto& registry = timeline->getRegistry();
    const auto& tracks = timeline->getTracks();
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        auto* track = registry.try_get<TimelineTrack>(tracks[ti]);
        if (!track) continue;
        for (size_t ci = 0; ci < track->layers.size(); ++ci) {
            if (track->layers[ci] == entity) {
                return std::make_pair(static_cast<int>(ti), static_cast<int>(ci));
            }
        }
    }
    return std::nullopt;
}

bool isScaleProp(AnimatableProperty p) {
    return p == AnimatableProperty::ScaleX
        || p == AnimatableProperty::ScaleY
        || p == AnimatableProperty::ScaleZ;
}

float scaleAxis(const Transform& t, AnimatableProperty p) {
    switch (p) {
        case AnimatableProperty::ScaleX: return t.scale.x;
        case AnimatableProperty::ScaleY: return t.scale.y;
        case AnimatableProperty::ScaleZ: return t.scale.z;
        default:                         return 1.0f;
    }
}

// Uniform-scale lock state for an entity. An absent ScaleLock counts as locked —
// that's the historical default the Properties panel has always used, and it
// keeps projects saved before the component existed behaving as they did.
bool isScaleLocked(entt::registry& registry, entt::entity e) {
    const auto* lock = registry.try_get<ScaleLock>(e);
    return lock == nullptr || lock->uniform;
}

} // namespace

void TimelineWidget::drawKeyframeShape(ImDrawList* drawList,
                                       const Keyframe& kf,
                                       float cx, float cy,
                                       float size) const {
    const ImU32 fillColor    = IM_COL32(255, 200, 50, 255);
    const ImU32 outlineColor = IM_COL32(255, 255, 255, 200);

    switch (kf.interpolation) {
        case InterpolationType::Step: {
            ImVec2 sqMin(cx - size, cy - size);
            ImVec2 sqMax(cx + size, cy + size);
            drawList->AddRectFilled(sqMin, sqMax, fillColor);
            drawList->AddRect(sqMin, sqMax, outlineColor, 0.0f, 0, 1.0f);
            break;
        }

        case InterpolationType::EaseIn: {
            drawList->AddBezierQuadratic(
                ImVec2(cx - size, cy - size),
                ImVec2(cx, cy),
                ImVec2(cx - size, cy + size),
                fillColor, 3.0f);
            ImVec2 rightDiamond[3] = {
                ImVec2(cx, cy - size),
                ImVec2(cx + size, cy),
                ImVec2(cx, cy + size)
            };
            drawList->AddTriangleFilled(rightDiamond[0], rightDiamond[1], rightDiamond[2], fillColor);
            drawList->AddLine(ImVec2(cx, cy - size), ImVec2(cx + size, cy), outlineColor, 1.0f);
            drawList->AddLine(ImVec2(cx + size, cy), ImVec2(cx, cy + size), outlineColor, 1.0f);
            break;
        }

        case InterpolationType::EaseOut: {
            ImVec2 leftDiamond[3] = {
                ImVec2(cx, cy - size),
                ImVec2(cx - size, cy),
                ImVec2(cx, cy + size)
            };
            drawList->AddTriangleFilled(leftDiamond[0], leftDiamond[1], leftDiamond[2], fillColor);
            drawList->AddBezierQuadratic(
                ImVec2(cx + size, cy - size),
                ImVec2(cx, cy),
                ImVec2(cx + size, cy + size),
                fillColor, 3.0f);
            drawList->AddLine(ImVec2(cx, cy - size), ImVec2(cx - size, cy), outlineColor, 1.0f);
            drawList->AddLine(ImVec2(cx - size, cy), ImVec2(cx, cy + size), outlineColor, 1.0f);
            break;
        }

        case InterpolationType::EaseInOut: {
            drawList->AddTriangleFilled(
                ImVec2(cx - size, cy - size),
                ImVec2(cx + size, cy - size),
                ImVec2(cx, cy),
                fillColor);
            drawList->AddTriangleFilled(
                ImVec2(cx - size, cy + size),
                ImVec2(cx + size, cy + size),
                ImVec2(cx, cy),
                fillColor);
            ImVec2 hourglass[6] = {
                ImVec2(cx - size, cy - size),
                ImVec2(cx + size, cy - size),
                ImVec2(cx, cy),
                ImVec2(cx + size, cy + size),
                ImVec2(cx - size, cy + size),
                ImVec2(cx, cy)
            };
            drawList->AddPolyline(hourglass, 6, outlineColor, ImDrawFlags_Closed, 1.0f);
            break;
        }

        case InterpolationType::Linear:
        default: {
            ImVec2 diamond[4] = {
                ImVec2(cx, cy - size),
                ImVec2(cx + size, cy),
                ImVec2(cx, cy + size),
                ImVec2(cx - size, cy)
            };
            drawList->AddConvexPolyFilled(diamond, 4, fillColor);
            drawList->AddPolyline(diamond, 4, outlineColor, ImDrawFlags_Closed, 1.0f);
            break;
        }
    }
}


// Channels to show under an expanded layer. OA layers get the full 3D set
// (9 channels — Pos/Rot/Scale × X/Y/Z); Clip-backed and Generative layers
// keep the 2D set (6 channels). Shared by renderPropertyTracks (body),
// renderClipPropertyPanel (header) and findKeyframeAtPosition so they all
// stay in lockstep.
bool TimelineWidget::isGroupCollapsed(entt::entity layerEntity,
                                      const std::string& groupPath) const {
    if (!m_timeline) return false;
    auto& reg = m_timeline->getRegistry();
    if (!reg.valid(layerEntity)) return false;
    const auto* ui = reg.try_get<LayerTrackUiState>(layerEntity);
    if (!ui) return false;
    for (const auto& p : ui->collapsedGroups) {
        if (p == groupPath) return true;
    }
    return false;
}

void TimelineWidget::toggleGroupCollapsed(entt::entity layerEntity,
                                          const std::string& groupPath) {
    if (!m_timeline) return;
    auto& reg = m_timeline->getRegistry();
    if (!reg.valid(layerEntity)) return;
    auto& ui = reg.get_or_emplace<LayerTrackUiState>(layerEntity);
    for (auto it = ui.collapsedGroups.begin(); it != ui.collapsedGroups.end(); ++it) {
        if (*it == groupPath) {
            ui.collapsedGroups.erase(it);
            // Drop the component when empty so we don't leave dead
            // state on layers that never collapse anything.
            if (ui.collapsedGroups.empty()) {
                reg.remove<LayerTrackUiState>(layerEntity);
            }
            return;
        }
    }
    ui.collapsedGroups.push_back(groupPath);
}

std::vector<TimelineWidget::TimelinePropertyDef>
TimelineWidget::propertyListForEntity(entt::entity e) const {
    std::vector<TimelinePropertyDef> out;
    if (!m_timeline) return out;
    auto& registry = m_timeline->getRegistry();
    if (!registry.valid(e)) return out;

    // Local row factories — TimelinePropertyDef is a private nested
    // type so we can't put these in an anonymous namespace; lambdas
    // inside this member function have access.
    auto groupHeader = [](int depth, std::string path, std::string label) {
        TimelinePropertyDef d;
        d.source     = TimelinePropertyDef::Source::GroupHeader;
        d.depth      = depth;
        d.groupPath  = std::move(path);
        d.groupLabel = std::move(label);
        return d;
    };
    auto transformRow = [](int depth, AnimatableProperty prop,
                           std::string name, float def) {
        TimelinePropertyDef d;
        d.source       = TimelinePropertyDef::Source::Transform;
        d.depth        = depth;
        d.prop         = prop;
        d.shortName    = std::move(name);
        d.defaultValue = def;
        return d;
    };
    auto effectParamRow = [](int depth, entt::entity fxEnt,
                             const effects::ParamSchema& schema) {
        TimelinePropertyDef d;
        d.source       = TimelinePropertyDef::Source::EffectParam;
        d.depth        = depth;
        d.effectEntity = fxEnt;
        d.shortName    = schema.displayName;
        d.paramName    = schema.name;
        d.paramHash    = effects::fnv1a32(schema.name);
        d.defaultValue = schema.defaultValue.f4[0];
        return d;
    };

    // ── Transform group (Pos / Scale / Rotation / Opacity for clip+generative;
    // 9-channel set for OA layers).
    const bool isOA = registry.all_of<ObjectAnimationLayer>(e);
    const std::string transformPath = "Transform";
    out.push_back(groupHeader(0, transformPath, "Transform"));
    const bool transformCollapsed = isGroupCollapsed(e, transformPath);
    if (!transformCollapsed) {
        if (isOA) {
            out.push_back(transformRow(1, AnimatableProperty::PositionX, "Pos X",    0.0f));
            out.push_back(transformRow(1, AnimatableProperty::PositionY, "Pos Y",    0.0f));
            out.push_back(transformRow(1, AnimatableProperty::PositionZ, "Pos Z",    0.0f));
            out.push_back(transformRow(1, AnimatableProperty::RotationX, "Rot X",    0.0f));
            out.push_back(transformRow(1, AnimatableProperty::RotationY, "Rot Y",    0.0f));
            out.push_back(transformRow(1, AnimatableProperty::RotationZ, "Rot Z",    0.0f));
            out.push_back(transformRow(1, AnimatableProperty::ScaleX,    "Scale X",  1.0f));
            out.push_back(transformRow(1, AnimatableProperty::ScaleY,    "Scale Y",  1.0f));
            out.push_back(transformRow(1, AnimatableProperty::ScaleZ,    "Scale Z",  1.0f));
        } else {
            out.push_back(transformRow(1, AnimatableProperty::PositionX, "Pos X",    0.0f));
            out.push_back(transformRow(1, AnimatableProperty::PositionY, "Pos Y",    0.0f));
            out.push_back(transformRow(1, AnimatableProperty::ScaleX,    "Scale X",  1.0f));
            out.push_back(transformRow(1, AnimatableProperty::ScaleY,    "Scale Y",  1.0f));
            out.push_back(transformRow(1, AnimatableProperty::Rotation,  "Rotation", 0.0f));
            out.push_back(transformRow(1, AnimatableProperty::Opacity,   "Opacity",  1.0f));
        }
    }

    // ── Effects group (only if the entity has a non-empty EffectChain
    // AND we have a registry for kind/schema lookups).
    if (m_effectKindRegistry) {
        const auto* chain = registry.try_get<EffectChain>(e);
        if (chain && !chain->nodes.empty()) {
            const std::string effectsPath = "Effects";
            out.push_back(groupHeader(0, effectsPath, "Effects"));
            const bool effectsCollapsed = isGroupCollapsed(e, effectsPath);
            if (!effectsCollapsed) {
                for (entt::entity fxEnt : chain->nodes) {
                    if (!registry.valid(fxEnt)) continue;
                    const auto* fx = registry.try_get<Effect>(fxEnt);
                    if (!fx) continue;
                    const effects::EffectKind* kind = m_effectKindRegistry->find(fx->kindId);
                    if (!kind) continue;
                    // Sub-group path uses kind display name; entity ID
                    // appended to disambiguate same-kind duplicates on
                    // one layer.
                    std::string subPath = effectsPath + "/" + kind->displayName +
                                          "#" + std::to_string(static_cast<std::uint32_t>(fxEnt));
                    out.push_back(groupHeader(1, subPath, kind->displayName));
                    const bool subCollapsed = isGroupCollapsed(e, subPath);
                    if (!subCollapsed) {
                        for (const auto& schema : kind->params) {
                            if (schema.type != ParamValue::Type::Float) continue; // v1
                            out.push_back(effectParamRow(2, fxEnt, schema));
                        }
                    }
                }
            }
        }
    }

    return out;
}

// Public accessor — also used by the input/expand code in TimelineWidget.cpp
// to compute the expanded track height. Counts visible rows (group
// headers + non-collapsed properties) so the panel height shrinks /
// grows as the user toggles groups.
std::size_t TimelineWidget::expandedPropertyRowCount(entt::entity layerEntity) const {
    if (!m_timeline) return 0;
    return propertyListForEntity(layerEntity).size();
}

void TimelineWidget::renderTimeRuler() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Get timeline framerate
    double frameRate = m_timeline->getFrameRate();
    float durationSeconds = m_timeline->getDuration() / 1000000.0f;
    FrameNumber totalFrames = static_cast<FrameNumber>(durationSeconds * frameRate);
    const float timelinePx = durationSeconds * m_pixelsPerSecond;

    // Ruler bounds: background stretches across the FULL content width, not the
    // visible region (GetContentRegionAvail caps at window width, which makes
    // the background vanish once the user scrolls past a small offset). ImGui's
    // clip rect handles hiding the off-screen portion.
    ImVec2 rulerMin = windowPos;
    ImVec2 rulerMax = ImVec2(windowPos.x + timelinePx, windowPos.y + RULER_HEIGHT);

    // Draw ruler background
    drawList->AddRectFilled(rulerMin, rulerMax, IM_COL32(40, 40, 40, 255));

    // Tick stride = zoom-ladder value. Tick pixel spacing is fixed at TICK_PX;
    // zoom changes the time each tick represents, not the spacing. Labels only
    // appear on every 5th tick (major ticks) so the ruler doesn't get crowded;
    // minor ticks are shorter and unlabeled.
    const FrameNumber tickEvery = static_cast<FrameNumber>(framesPerTick());
    const int LABEL_EVERY_N = 5;

    // Hover-division highlight. Fills the [x, x + tickEvery]
    // band across the ruler when the mouse hovers a tick division. Drawn
    // before the tick lines so the lines remain visible on top.
    if (m_hoverDivisionIndex >= 0) {
        const float pxPerFrameHL = m_pixelsPerSecond / static_cast<float>(frameRate);
        const FrameNumber bandStartFrame = static_cast<FrameNumber>(m_hoverDivisionIndex) * tickEvery;
        const float bandX0 = windowPos.x + bandStartFrame * pxPerFrameHL;
        const float bandX1 = bandX0 + tickEvery * pxPerFrameHL;
        if (bandX1 > windowPos.x && bandX0 < rulerMax.x) {
            drawList->AddRectFilled(
                ImVec2(std::max(bandX0, rulerMin.x), rulerMin.y),
                ImVec2(std::min(bandX1, rulerMax.x), rulerMax.y),
                IM_COL32(255, 255, 255, 18)
            );
        }
    }

    // Only iterate the visible range — at extreme zoom (1f over 10 min) the
    // total frame count hits 18000. Drawing all of them would blow past ImGui's
    // default 16-bit vertex index limit and silently drop past a threshold.
    const float pxPerFrame = m_pixelsPerSecond / static_cast<float>(frameRate);
    const float visibleStartPx = m_syncScrollX - TICK_PX;
    const float visibleEndPx   = m_syncScrollX + m_lastVisibleWidth + TICK_PX;
    FrameNumber firstVisibleFrame = pxPerFrame > 0.0f
        ? std::max<FrameNumber>(0, static_cast<FrameNumber>(visibleStartPx / pxPerFrame))
        : 0;
    FrameNumber lastVisibleFrame = pxPerFrame > 0.0f
        ? std::min(totalFrames, static_cast<FrameNumber>(visibleEndPx / pxPerFrame) + 1)
        : totalFrames;
    // Snap to tick-stride grid and preserve the tickCounter modulo relative to
    // frame 0 so major/minor striping stays stable as the user scrolls.
    FrameNumber firstTick = (firstVisibleFrame / tickEvery) * tickEvery;
    int tickCounter = static_cast<int>(firstTick / tickEvery);

    for (FrameNumber frame = firstTick; frame <= lastVisibleFrame; frame += tickEvery, ++tickCounter) {
        float timeSeconds = static_cast<float>(frame) / static_cast<float>(frameRate);
        float x = windowPos.x + timeToPixel(static_cast<Timecode>(timeSeconds * 1000000.0f));

        const bool isMajor = (tickCounter % LABEL_EVERY_N == 0);
        const float tickHeight = isMajor ? 12.0f : 6.0f;
        const ImU32 tickColor = isMajor ? IM_COL32(200, 200, 200, 255)
                                        : IM_COL32(140, 140, 140, 255);
        drawList->AddRectFilled(
            ImVec2(x, rulerMax.y - tickHeight),
            ImVec2(x + 1.0f, rulerMax.y),
            tickColor
        );

        if (!isMajor) continue;

        // SMPTE timecode label HH:MM:SS:FF on every major (5th) tick.
        int totalSeconds = static_cast<int>(frame / frameRate);
        int hours = totalSeconds / 3600;
        int minutes = (totalSeconds % 3600) / 60;
        int seconds = totalSeconds % 60;
        int frames = static_cast<int>(frame % static_cast<FrameNumber>(frameRate));

        std::ostringstream oss;
        if (hours > 0) {
            oss << std::setfill('0') << std::setw(2) << hours << ":";
        }
        oss << std::setfill('0') << std::setw(2) << minutes << ":"
            << std::setfill('0') << std::setw(2) << seconds << ":"
            << std::setfill('0') << std::setw(2) << frames;

        drawList->AddText(
            ImVec2(x + 3.0f, rulerMin.y + 5.0f),
            IM_COL32(200, 200, 200, 255),
            oss.str().c_str()
        );
    }

    // Section break lines + always-on tinted band. The timeline is treated
    // as a single implicit section by default; once break-points exist they
    // split the band into sub-spans:
    //   `[0, breaks[0])`                       -> palette[0] when clips exist,
    //                                              else neutral gray
    //   `[breaks[i].breakFrame, breaks[i+1])`  -> breaks[i].color
    //   `[breaks[N-1].breakFrame, duration]`   -> breaks[N-1].color
    {
        const auto& sections = m_timeline->getSections();
        const float bandH = 4.0f;
        const float bandY = rulerMin.y + 1.0f;
        const FrameNumber currentFrame = m_timeline->getCurrentFrame();
        const bool atBreak = m_timeline->sectionAtBreak();
        const Timecode duration = m_timeline->getDuration();
        // Truly-empty timeline (no breaks AND no clips) gets the neutral
        // tint — the band has no semantic meaning yet. Once a clip lands
        // on the timeline the implicit first section adopts palette[0]
        // even before the user adds any breaks (Round-3 Phase 2 #1).
        constexpr ImU32 kDefaultSpanColor = 0xFF404040u;
        bool timelineHasClips = false;
        {
            auto& registry = m_timeline->getRegistry();
            for (entt::entity trackEntity : m_timeline->getTracks()) {
                const auto* track = registry.try_get<TimelineTrack>(trackEntity);
                if (track && !track->layers.empty()) { timelineHasClips = true; break; }
            }
        }
        const ImU32 implicitFirstColor = timelineHasClips
            ? sectionPalette::pickColor(0)
            : kDefaultSpanColor;

        auto drawSpan = [&](Timecode startT, Timecode endT, ImU32 baseColor) {
            const float xs = windowPos.x + timeToPixel(startT);
            const float xe = windowPos.x + timeToPixel(endT);
            if (xe < windowPos.x || xs > rulerMax.x) return;
            const ImU32 tint = (baseColor & 0x00FFFFFFu) | (60u << 24);
            drawList->AddRectFilled(ImVec2(xs, bandY), ImVec2(xe, bandY + bandH), tint);
        };

        if (sections.empty()) {
            drawSpan(0, duration, implicitFirstColor);
        } else {
            // Pre-first-break span IS the implicit first section. Section
            // breaks are integer frames; convert to the microsecond axis
            // drawSpan / timeToPixel work in.
            if (sections.front().breakFrame > 0) {
                drawSpan(0, m_timeline->frameToTime(sections.front().breakFrame),
                         implicitFirstColor);
            }
            for (size_t i = 0; i < sections.size(); ++i) {
                const Timecode startT = m_timeline->frameToTime(sections[i].breakFrame);
                const Timecode endT = (i + 1 < sections.size())
                                      ? m_timeline->frameToTime(sections[i + 1].breakFrame)
                                      : duration;
                drawSpan(startT, endT, sections[i].color);
            }
        }

        // Break lines.
        for (const auto& sec : sections) {
            const float x = windowPos.x + frameToPixel(sec.breakFrame);
            if (x < windowPos.x - 4.0f || x > rulerMax.x + 4.0f) continue;

            const bool playheadAtThisBreak =
                atBreak && currentFrame == sec.breakFrame;

            const ImU32 lineColor = playheadAtThisBreak
                ? IM_COL32(255, 240, 120, 255)
                : sec.color;
            drawList->AddLine(ImVec2(x, rulerMin.y), ImVec2(x, rulerMax.y),
                              lineColor, 2.0f);
        }
    }

    // Cue lane is now drawn separately, ABOVE the ruler band, by
    // render() before renderTimeRuler() runs. See renderCueLane().

    // Range-selection overlay on the ruler — stronger tint than the tracks
    // band so the endpoints are obviously interactive. While the user is
    // actively shift+dragging, show the In/Out/Δ frame readout near the
    // mouse so they can land an exact span without fishing.
    if (m_range.active && m_range.end > m_range.start) {
        const float xStart = windowPos.x + timeToPixel(m_range.start);
        const float xEnd   = windowPos.x + timeToPixel(m_range.end);
        drawList->AddRectFilled(
            ImVec2(xStart, rulerMin.y), ImVec2(xEnd, rulerMax.y),
            IM_COL32(80, 160, 255, 80));
        drawList->AddLine(ImVec2(xStart, rulerMin.y), ImVec2(xStart, rulerMax.y),
            IM_COL32(160, 220, 255, 255), 1.5f);
        drawList->AddLine(ImVec2(xEnd, rulerMin.y), ImVec2(xEnd, rulerMax.y),
            IM_COL32(160, 220, 255, 255), 1.5f);

        if (m_isCreatingRange) {
            FrameNumber inF  = m_timeline->timeToFrame(m_range.start);
            FrameNumber outF = m_timeline->timeToFrame(m_range.end);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "In F%lld | Out F%lld | %lldf",
                static_cast<long long>(inF),
                static_cast<long long>(outF),
                static_cast<long long>(outF - inF));
            const float labelX = std::min(xEnd + 6.0f, rulerMax.x - 200.0f);
            drawList->AddText(ImVec2(labelX, rulerMin.y + 5.0f),
                IM_COL32(220, 240, 255, 255), buf);
        }
    }
}

void TimelineWidget::renderTracks() {
    if (!m_timeline) return;

    // Get base window position ONCE at the start
    // This prevents cursor drift from InvisibleButtons affecting subsequent tracks
    ImVec2 baseWindowPos = ImGui::GetCursorScreenPos();

    // Range-selection band across all tracks. Drawn before the track loop
    // so track bg fills can sit on top — the band tint mixes with the bg.
    // We re-draw the endpoint accent lines AFTER the track loop so they
    // remain crisp on top of clips.
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float visibleH = ImGui::GetWindowHeight();
        if (m_range.active && m_range.end > m_range.start) {
            // baseWindowPos.x already includes -Scroll (see the grid-line note
            // below) — draw at the absolute content pixel, no scroll subtract.
            const float xStart = baseWindowPos.x + timeToPixel(m_range.start);
            const float xEnd   = baseWindowPos.x + timeToPixel(m_range.end);
            const float yTop = baseWindowPos.y;
            const float yBot = baseWindowPos.y + visibleH;
            drawList->AddRectFilled(ImVec2(xStart, yTop), ImVec2(xEnd, yBot),
                IM_COL32(80, 160, 255, 36));
        }
    }

    // Iterate the per-frame row cache (Phase 9). Hidden (shy) tracks are
    // already excluded; each row carries its content-space Y and full height.
    // cumulativeY for the grid below is the bottom of the last row.
    float cumulativeY = 0.0f;
    for (const auto& row : m_trackRows) {
        const float trackY = baseWindowPos.y + row.y;
        ImVec2 trackBasePos(baseWindowPos.x, trackY);

        // renderTrack draws only the base TRACK_HEIGHT band; the expanded
        // property panel (keyframe diamonds) is drawn separately below it.
        renderTrack(row.track, row.modelIndex, trackBasePos, TRACK_HEIGHT);

        if (row.clipAtPlayhead != entt::null) {
            const float propY = baseWindowPos.y + row.y + TRACK_HEIGHT;
            renderPropertyTracks(row.clipAtPlayhead, row.modelIndex, baseWindowPos, propY);
        }

        cumulativeY = row.y + row.height + TRACK_PADDING;
    }

    // Tick gridlines + range endpoint accents — drawn AFTER the track loop
    // so they sit on top of the (opaque) track-bg fills and clip rects.
    // Iteration is clamped to the visible viewport so a 24h timeline at
    // 1f tick spacing doesn't enumerate millions of off-screen frames.
    //
    // gridBot extends to whichever is larger of:
    //   (a) cumulativeY — the painted track-content area (so lines reach
    //       every track even when the pane is shorter than total content
    //       and the user scrolls), and
    //   (b) the child window's visible height — so lines also extend
    //       through the empty canvas BELOW the last track when the pane
    //       is taller than total content. Without this, the grid stops
    //       partway down the visible pane and looks like it's bound to
    //       the pane rather than the timeline.
    // The actual draw is scissor-clipped by ImGui to the child's visible
    // bounds, so overdrawing past the bottom is free.
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float visibleW = ImGui::GetWindowWidth();
        const float visibleH = ImGui::GetWindowHeight();
        // Both axes read the tracks child's live committed scroll. Post-Phase-3
        // m_syncScrollX equals GetScrollX() this frame (it's written right after
        // the tracks Begin), so the two were equivalent — using GetScroll*() for
        // both keeps the source consistent and free of that write-order coupling.
        const float scrollY = ImGui::GetScrollY();
        const float scrollX = ImGui::GetScrollX();
        const double frameRate = m_timeline->getFrameRate();
        const FrameNumber tickEvery = static_cast<FrameNumber>(framesPerTick());
        const float pxPerFrame = m_pixelsPerSecond / static_cast<float>(frameRate);

        // Visible bottom in screen coords (window-y baseline + visible
        // height). baseWindowPos.y already includes -scrollY, so add it
        // back to translate cumulativeY's content-space bottom into the
        // same screen-coord reference as the visible bottom.
        const float gridTop = baseWindowPos.y;
        const float contentBot = baseWindowPos.y + cumulativeY;
        const float visibleBot = baseWindowPos.y + scrollY + visibleH;
        const float gridBot = std::max(contentBot, visibleBot);

        if (pxPerFrame > 0.0f && tickEvery > 0) {
            FrameNumber firstFrame = static_cast<FrameNumber>(std::floor(scrollX / pxPerFrame));
            FrameNumber lastFrame  = static_cast<FrameNumber>(std::ceil((scrollX + visibleW) / pxPerFrame));
            firstFrame = std::max<FrameNumber>(0, (firstFrame / tickEvery) * tickEvery);

            for (FrameNumber f = firstFrame; f <= lastFrame; f += tickEvery) {
                // Cull on xRel (content offset from scroll — tells us if this
                // frame is in the viewport) but draw at absolute content pos:
                // baseWindowPos.x already includes -Scroll (ImGui sets
                // CursorStartPos = Pos + Padding - Scroll), so adding xRel
                // would subtract scroll a second time and push the line off
                // screen left. Use f*pxPerFrame directly.
                const float xRel = f * pxPerFrame - scrollX;
                if (xRel < 0 || xRel > visibleW) continue;
                const float x = baseWindowPos.x + f * pxPerFrame;
                drawList->AddRectFilled(
                    ImVec2(x, gridTop),
                    ImVec2(x + 1.0f, gridBot),
                    IM_COL32(255, 255, 255, 56)
                );
            }
        }

        // Range endpoint accent lines on top of clips so the boundary stays
        // visible even when a clip is colored similar to the band tint.
        if (m_range.active && m_range.end > m_range.start) {
            // baseWindowPos.x already includes -Scroll — same as the grid
            // lines above; draw at the absolute content pixel.
            const float xStart = baseWindowPos.x + timeToPixel(m_range.start);
            const float xEnd   = baseWindowPos.x + timeToPixel(m_range.end);
            drawList->AddLine(ImVec2(xStart, gridTop), ImVec2(xStart, gridBot),
                IM_COL32(120, 200, 255, 220), 1.0f);
            drawList->AddLine(ImVec2(xEnd, gridTop), ImVec2(xEnd, gridBot),
                IM_COL32(120, 200, 255, 220), 1.0f);
        }
    }
}

void TimelineWidget::renderTrack(entt::entity trackEntity, int trackIndex, ImVec2 baseWindowPos, float trackHeight) {
    if (!m_timeline) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return;

    // Track rect spans the full timeline content width, not GetContentRegionAvail
    // (which caps at window width and makes the background vanish past a small
    // scroll offset). ImGui's clip rect handles hiding the off-screen portion.
    const float durationSec = m_timeline->getDuration() / 1000000.0f;
    const float timelinePx = durationSec * m_pixelsPerSecond;
    float trackY = baseWindowPos.y;
    ImVec2 trackMin = ImVec2(baseWindowPos.x, trackY);
    ImVec2 trackMax = ImVec2(baseWindowPos.x + timelinePx, trackY + trackHeight);

    // Draw track background (alternating colors)
    ImU32 trackColor = (trackIndex % 2 == 0)
        ? IM_COL32(50, 50, 50, 255)
        : IM_COL32(45, 45, 45, 255);
    drawList->AddRectFilled(trackMin, trackMax, trackColor);

    // Draw track border
    drawList->AddRect(trackMin, trackMax, IM_COL32(70, 70, 70, 255));

    // Draw track label
    std::ostringstream labelStream;
    labelStream << "Track " << (trackIndex + 1);
    drawList->AddText(
        ImVec2(trackMin.x + 10.0f, trackMin.y + TRACK_HEIGHT / 2.0f - 7.0f),
        IM_COL32(180, 180, 180, 255),
        labelStream.str().c_str()
    );

    // Render clips in this track FIRST (so we can handle clip clicks before drag-drop)
    for (entt::entity clipEntity : track->layers) {
        renderClip(clipEntity, trackIndex, baseWindowPos);
    }

    // Handle drag-drop target for this track AFTER rendering clips
    // Use InvisibleButton with AllowOverlap to not block clip interaction
    ImGui::SetCursorScreenPos(trackMin);
    std::ostringstream dropTargetId;
    dropTargetId << "##trackDropTarget" << trackIndex;

    // Allow this button to overlap with other items (clip rendering uses draw list, not ImGui widgets)
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(dropTargetId.str().c_str(), ImVec2(trackMax.x - trackMin.x, trackHeight));

    if (ImGui::BeginDragDropTarget()) {
        ImVec2 mousePos = ImGui::GetMousePos();
        // baseWindowPos.x already includes -Scroll — mousePos.x - baseWindowPos.x
        // is the content pixel directly; do not add scroll back.
        float relativeX = mousePos.x - baseWindowPos.x;
        Timecode rawDropTime = pixelToTime(relativeX);
        if (rawDropTime < 0) rawDropTime = 0;

        const double fps = m_timeline ? m_timeline->getFrameRate() : 0.0;

        // Peek payload (without consuming) so we can populate the drop
        // ghost while the user is still hovering. AcceptDragDropPayload
        // below only fires on actual release.
        const ImGuiPayload* peek = ImGui::GetDragDropPayload();
        if (peek && m_timeline && fps > 0.0) {
            std::string type(peek->DataType ? peek->DataType : "");

            Timecode ghostDur = 0;
            std::string ghostLabel;

            if (type == "MEDIA_FILE") {
                std::string path(static_cast<const char*>(peek->Data));
                double durSec = (m_mediaDurationLookup ? m_mediaDurationLookup(path) : 0.0);
                if (durSec <= 0.0) durSec = 10.0;  // probe-cache miss → placeholder
                ghostDur = static_cast<Timecode>(durSec * 1000000.0);
                // ImGui default font is ASCII-only — strip directory only.
                auto slash = path.find_last_of("/\\");
                ghostLabel = (slash == std::string::npos) ? path : path.substr(slash + 1);
            } else if (type == "LAYER_KIND") {
                ghostDur = static_cast<Timecode>(10.0 * 1000000.0);  // 10s default, matches callback
                const uint8_t* data    = static_cast<const uint8_t*>(peek->Data);
                const uint8_t  kindByte = data[0];
                const uint8_t  subKind  = (peek->DataSize >= 2) ? data[1] : 0;
                switch (static_cast<Layer::Kind>(kindByte)) {
                    case Layer::Kind::ObjectAnimation: ghostLabel = "OA Layer"; break;
                    case Layer::Kind::Clip:            ghostLabel = "Clip Layer"; break;
                    case Layer::Kind::Generative:
                        ghostLabel = (subKind == 1) ? "Text Layer" : "Muncher Layer";
                        break;
                    case Layer::Kind::Signal:          ghostLabel = "Signal Layer"; break;
                    default: ghostLabel = "Layer"; break;
                }
            }

            if (ghostDur > 0) {
                Timecode ghostStart = snapDropPosition(rawDropTime, trackIndex);
                m_ghost.active     = true;
                m_ghost.trackIndex = trackIndex;
                m_ghost.startTime  = ghostStart;
                m_ghost.duration   = ghostDur;
                m_ghost.valid      = !wouldOverlapAnyClip(trackIndex, ghostStart,
                                                         ghostDur, entt::null);
                m_ghost.label      = ghostLabel;
            }
        }

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_FILE")) {
            const char* droppedPath = static_cast<const char*>(payload->Data);
            std::string filepath(droppedPath);

            // Snap drop time to match the ghost the user saw.
            Timecode dropTime = snapDropPosition(rawDropTime, trackIndex);

            if (m_mediaDropCallback) {
                m_mediaDropCallback(filepath, trackIndex, dropTime);
            }
            m_ghost.active = false;
        }

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LAYER_KIND")) {
            const uint8_t* data    = static_cast<const uint8_t*>(payload->Data);
            const uint8_t  kind    = data[0];
            const uint8_t  subKind = (payload->DataSize >= 2) ? data[1] : 0;

            if (m_timeline) {
                // Snap drop time to match the ghost the user saw.
                Timecode dropTime = snapDropPosition(rawDropTime, trackIndex);

                FrameNumber startFrame = static_cast<FrameNumber>(dropTime / 1000000.0 * fps + 0.5);
                // Default duration: 10 seconds worth of timeline frames
                FrameNumber defaultDuration = static_cast<FrameNumber>(10.0 * fps);

                if (static_cast<Layer::Kind>(kind) == Layer::Kind::ObjectAnimation) {
                    if (m_oaLayerDropCallback) {
                        m_oaLayerDropCallback(trackIndex, startFrame, defaultDuration);
                    }
                } else if (static_cast<Layer::Kind>(kind) == Layer::Kind::Clip) {
                    if (m_clipLayerDropCallback) {
                        m_clipLayerDropCallback(trackIndex, startFrame, defaultDuration);
                    }
                } else if (static_cast<Layer::Kind>(kind) == Layer::Kind::Generative) {
                    if (subKind == 1) {
                        if (m_textLayerDropCallback) {
                            m_textLayerDropCallback(trackIndex, startFrame, defaultDuration);
                        }
                    } else {
                        if (m_generativeLayerDropCallback) {
                            m_generativeLayerDropCallback(trackIndex, startFrame, defaultDuration);
                        }
                    }
                } else if (static_cast<Layer::Kind>(kind) == Layer::Kind::Signal) {
                    if (m_signalLayerDropCallback) {
                        m_signalLayerDropCallback(trackIndex, startFrame, defaultDuration);
                    }
                }
            }
            m_ghost.active = false;
        }

        ImGui::EndDragDropTarget();
    }

    // Drop ghost overlay — draws on top of clips in this track when this
    // is the target track for an active drag (cross-track existing clip
    // drag or fresh MediaBin / LayersWindow drag-drop). Per-track render
    // keeps Y math local to the track's known trackMin/trackMax.
    if (m_ghost.active && m_ghost.trackIndex == trackIndex && m_timeline) {
        const float x0 = trackMin.x + timeToPixel(m_ghost.startTime);
        const float x1 = trackMin.x + timeToPixel(m_ghost.startTime + m_ghost.duration);
        const float y0 = trackMin.y + 2.0f;
        const float y1 = trackMax.y - 2.0f;

        const ImU32 fillCol = m_ghost.valid ? IM_COL32(120, 180, 255,  64)
                                            : IM_COL32(220, 100, 100,  64);
        const ImU32 borderCol = m_ghost.valid ? IM_COL32(120, 180, 255, 220)
                                              : IM_COL32(220, 100, 100, 220);

        drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fillCol, 3.0f);
        drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), borderCol, 3.0f, 0, 2.0f);

        if (!m_ghost.label.empty() && (x1 - x0) > 60.0f) {
            const ImU32 textCol = IM_COL32(240, 240, 240, 220);
            const ImVec2 labelSize = ImGui::CalcTextSize(m_ghost.label.c_str());
            const float labelX = x0 + 6.0f;
            const float labelY = y0 + ((y1 - y0) - labelSize.y) * 0.5f;
            drawList->AddText(ImVec2(labelX, labelY), textCol, m_ghost.label.c_str());
        }
    }
}

float TimelineWidget::renderClip(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos) {
    (void)trackIndex;  // trackIndex no longer used for Y calculation
    if (!m_timeline) return TRACK_HEIGHT;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(clipEntity);

    // Layer-only path: no Clip component, just Layer + a discriminator
    // component (ObjectAnimationLayer or GenerativeLayer). Renders a
    // colored rectangle using Layer::color + Layer::name. Same hit-test
    // and selection as Clip rendering.
    const auto* layer  = registry.try_get<Layer>(clipEntity);
    const auto* oal    = registry.try_get<ObjectAnimationLayer>(clipEntity);
    const auto* gen    = registry.try_get<GenerativeLayer>(clipEntity);
    const auto* sigLay = registry.try_get<SignalLayer>(clipEntity);

    // Signal Output Layer: momentary renders as a diamond marker at startFrame;
    // continuous renders as a colored span block (same as OA/Generative).
    if (!clip && layer && sigLay) {
        const double fps = m_timeline->getFrameRate();
        const float trackY = baseWindowPos.y;
        const float startX = baseWindowPos.x
            + static_cast<float>(layer->startFrame) / static_cast<float>(fps)
            * m_pixelsPerSecond;
        const bool isMomentary = (sigLay->mode == SignalLayer::Mode::Momentary);
        const bool isSelected = m_timeline->isSelected(clipEntity);  // Phase 12: set membership

        const auto& c = layer->color;
        const ImU32 fillCol = isSelected
            ? IM_COL32(static_cast<int>(std::min(c[0] * 1.35f, 1.0f) * 255),
                       static_cast<int>(std::min(c[1] * 1.35f, 1.0f) * 255),
                       static_cast<int>(std::min(c[2] * 1.35f, 1.0f) * 255), 255)
            : IM_COL32(static_cast<int>(c[0] * 255), static_cast<int>(c[1] * 255),
                       static_cast<int>(c[2] * 255), 255);
        const ImU32 borderCol = isSelected
            ? IM_COL32(static_cast<int>(std::min(c[0] * 1.6f, 1.0f) * 255),
                       static_cast<int>(std::min(c[1] * 1.6f, 1.0f) * 255),
                       static_cast<int>(std::min(c[2] * 1.6f, 1.0f) * 255), 255)
            : IM_COL32(static_cast<int>(std::min(c[0] * 1.25f, 1.0f) * 255),
                       static_cast<int>(std::min(c[1] * 1.25f, 1.0f) * 255),
                       static_cast<int>(std::min(c[2] * 1.25f, 1.0f) * 255), 255);

        if (isMomentary) {
            // Diamond marker centered at startFrame. Half-size based on track height.
            const float cx = startX;
            const float cy = trackY + TRACK_HEIGHT * 0.5f;
            const float r  = TRACK_HEIGHT * 0.3f;
            drawList->AddQuadFilled(
                ImVec2(cx,     cy - r),   // top
                ImVec2(cx + r, cy),       // right
                ImVec2(cx,     cy + r),   // bottom
                ImVec2(cx - r, cy),       // left
                fillCol);
            drawList->AddQuad(
                ImVec2(cx,     cy - r),
                ImVec2(cx + r, cy),
                ImVec2(cx,     cy + r),
                ImVec2(cx - r, cy),
                borderCol, 1.5f);
            // Hit area: invisible button centered on the diamond.
            const ImVec2 btnMin(cx - r, trackY + CLIP_PADDING);
            const ImVec2 btnMax(cx + r, trackY + TRACK_HEIGHT - CLIP_PADDING);
            ImGui::SetCursorScreenPos(btnMin);
            char hitId[32];
            std::snprintf(hitId, sizeof(hitId), "##signal%u",
                          static_cast<uint32_t>(clipEntity));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton(hitId, ImVec2(btnMax.x - btnMin.x,
                                                     btnMax.y - btnMin.y))) {
                m_selectedClip = clipEntity;
                m_timeline->setSelectedClip(clipEntity);
            }
        } else {
            // Continuous: span block identical to OA/Generative path.
            const float durationX = static_cast<float>(layer->duration)
                / static_cast<float>(fps) * m_pixelsPerSecond;
            const ImVec2 clipMin(startX, trackY + CLIP_PADDING);
            const ImVec2 clipMax(startX + durationX, trackY + TRACK_HEIGHT - CLIP_PADDING);
            drawList->AddRectFilled(clipMin, clipMax, fillCol, 3.0f);
            drawList->AddRect(clipMin, clipMax, borderCol, 3.0f, 0, 2.0f);
            // Label
            const std::string& lbl = layer->name.empty() ? std::string("Signal") : layer->name;
            constexpr float kPad = 5.0f;
            const float availW = (clipMax.x - clipMin.x) - kPad * 2.0f;
            if (availW > 0.0f) {
                std::string disp = lbl;
                ImVec2 sz = ImGui::CalcTextSize(lbl.c_str());
                if (sz.x > availW) {
                    while (!disp.empty() &&
                           ImGui::CalcTextSize((disp + "...").c_str()).x > availW)
                        disp.pop_back();
                    disp = disp.empty() ? std::string() : disp + "...";
                }
                if (!disp.empty())
                    drawList->AddText(ImVec2(clipMin.x + kPad, clipMin.y + 4.0f),
                                      IM_COL32(255, 255, 255, 255), disp.c_str());
            }
            // Hit area
            ImGui::SetCursorScreenPos(clipMin);
            char hitId[32];
            std::snprintf(hitId, sizeof(hitId), "##signal%u",
                          static_cast<uint32_t>(clipEntity));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton(hitId, ImVec2(durationX,
                                                     TRACK_HEIGHT - CLIP_PADDING * 2))) {
                m_selectedClip = clipEntity;
                m_timeline->setSelectedClip(clipEntity);
            }
        }
        return TRACK_HEIGHT;
    }

    if (!clip && layer && (oal || gen)) {
        float trackY = baseWindowPos.y;
        double timelineFrameRate = m_timeline->getFrameRate();
        float startSeconds   = static_cast<float>(layer->startFrame) / static_cast<float>(timelineFrameRate);
        float durationSeconds = static_cast<float>(layer->duration) / static_cast<float>(timelineFrameRate);
        Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
        Timecode endTime   = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

        float clipX     = baseWindowPos.x + timeToPixel(startTime);
        float clipWidth = timeToPixel(endTime - startTime);

        ImVec2 clipMin = ImVec2(clipX, trackY + CLIP_PADDING);
        ImVec2 clipMax = ImVec2(clipX + clipWidth, trackY + TRACK_HEIGHT - CLIP_PADDING);

        bool isSelected = m_timeline->isSelected(clipEntity);  // Phase 12: set membership

        // Color swatch from Layer::color; brighten on selection. Border tints
        // from the same color (lightened) so OA / Generative / future kinds
        // each get a coherent palette without hard-coding a per-kind border.
        const auto& c = layer->color;
        ImU32 fillColor = isSelected
            ? IM_COL32(static_cast<int>(std::min(c[0] * 1.35f, 1.0f) * 255),
                       static_cast<int>(std::min(c[1] * 1.35f, 1.0f) * 255),
                       static_cast<int>(std::min(c[2] * 1.35f, 1.0f) * 255), 255)
            : IM_COL32(static_cast<int>(c[0] * 255), static_cast<int>(c[1] * 255),
                       static_cast<int>(c[2] * 255), 255);
        ImU32 borderColor = isSelected
            ? IM_COL32(static_cast<int>(std::min(c[0] * 1.6f, 1.0f) * 255),
                       static_cast<int>(std::min(c[1] * 1.6f, 1.0f) * 255),
                       static_cast<int>(std::min(c[2] * 1.6f, 1.0f) * 255), 255)
            : IM_COL32(static_cast<int>(std::min(c[0] * 1.25f, 1.0f) * 255),
                       static_cast<int>(std::min(c[1] * 1.25f, 1.0f) * 255),
                       static_cast<int>(std::min(c[2] * 1.25f, 1.0f) * 255), 255);

        drawList->AddRectFilled(clipMin, clipMax, fillColor, 3.0f);
        drawList->AddRect(clipMin, clipMax, borderColor, 3.0f, 0, 2.0f);

        // Label: Layer::name if set, otherwise a kind-default fallback.
        const char* defaultLabel = oal ? "Object Animation" : "Generative";
        const std::string& label = layer->name.empty() ? std::string(defaultLabel) : layer->name;
        constexpr float kPad = 5.0f;
        float availW = (clipMax.x - clipMin.x) - kPad * 2.0f;
        if (availW > 0.0f) {
            std::string displayLabel = label;
            ImVec2 fullSize = ImGui::CalcTextSize(label.c_str());
            if (fullSize.x > availW) {
                while (!displayLabel.empty() &&
                       ImGui::CalcTextSize((displayLabel + "...").c_str()).x > availW) {
                    displayLabel.pop_back();
                }
                displayLabel = displayLabel.empty() ? std::string() : displayLabel + "...";
            }
            if (!displayLabel.empty()) {
                drawList->AddText(ImVec2(clipMin.x + kPad, clipMin.y + 4.0f),
                                  IM_COL32(255, 255, 255, 255), displayLabel.c_str());
            }
        }

        // Hit-test + selection (reuse the same invisible button pattern as Clip)
        ImGui::SetCursorScreenPos(clipMin);
        char hitId[32];
        std::snprintf(hitId, sizeof(hitId), "##layeronly%u", static_cast<uint32_t>(clipEntity));
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton(hitId, ImVec2(clipWidth, TRACK_HEIGHT - CLIP_PADDING * 2))) {
            m_selectedClip = clipEntity;
            m_timeline->setSelectedClip(clipEntity);
        }

        return TRACK_HEIGHT;
    }

    if (!clip) return TRACK_HEIGHT;

    // Use baseWindowPos.y directly - it already includes cumulative offset from renderTracks
    float trackY = baseWindowPos.y;

    // Convert frame timing to microseconds (Timecode units)
    // Use timeline frame rate (not clip source rate) since startFrame and duration are in timeline frames
    double timelineFrameRate = m_timeline->getFrameRate();
    float startSeconds = static_cast<float>(clip->startFrame) / static_cast<float>(timelineFrameRate);
    float durationSeconds = static_cast<float>(clip->duration) / static_cast<float>(timelineFrameRate);
    Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
    Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

    float clipX = baseWindowPos.x + timeToPixel(startTime);
    float clipWidth = timeToPixel(endTime - startTime);

    ImVec2 clipMin = ImVec2(clipX, trackY + CLIP_PADDING);
    ImVec2 clipMax = ImVec2(clipX + clipWidth, trackY + TRACK_HEIGHT - CLIP_PADDING);

    // Choose clip color based on selection. Phase 12: membership comes from
    // the Timeline multi-select set; the PRIMARY (last-clicked) clip gets a
    // brighter fill + accent border so the group's anchor is glanceable.
    const bool isSelected = m_timeline->isSelected(clipEntity);
    const bool isPrimary  = (clipEntity == m_timeline->getSelectedClip());
    ImU32 clipColor = isPrimary
        ? IM_COL32(130, 175, 255, 255)
        : (isSelected ? IM_COL32(100, 150, 255, 255)
                      : IM_COL32(80, 120, 180, 255));

    // Draw clip rectangle
    drawList->AddRectFilled(clipMin, clipMax, clipColor, 3.0f);
    const ImU32 borderCol = isPrimary  ? IM_COL32(210, 230, 255, 255)
                          : isSelected ? IM_COL32(150, 190, 245, 255)
                                       : IM_COL32(120, 160, 220, 255);
    drawList->AddRect(clipMin, clipMax, borderCol, 3.0f, 0, 2.0f);

    // Phase C — Locked clips get a muted-red outline so the section-behavior
    // policy is glanceable on the timeline. Drawn over the default border so
    // the tint always wins. Cheap (one extra rect per Locked clip).
    if (clip->sectionBehavior == SectionBehavior::Locked) {
        drawList->AddRect(clipMin, clipMax,
                          IM_COL32(200, 80, 60, 220),
                          3.0f, 0, 4.0f);
    }

    // Draw clip label — full basename, truncated with ellipsis only when the
    // current zoom × clip width can't fit it. The previous implementation
    // hard-capped at 20 chars regardless of available space.
    size_t lastSlash = clip->filepath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos)
        ? clip->filepath.substr(lastSlash + 1)
        : clip->filepath;

    constexpr float kLabelPaddingPx = 5.0f;
    constexpr float kLabelRightPadPx = 4.0f;
    float availableWidth = (clipMax.x - clipMin.x) - kLabelPaddingPx - kLabelRightPadPx;
    if (availableWidth > 0.0f) {
        ImVec2 fullSize = ImGui::CalcTextSize(filename.c_str());
        if (fullSize.x > availableWidth) {
            // Trim chars off the end until "<trimmed>..." fits, then commit.
            std::string trimmed = filename;
            const char* ell = "...";
            while (!trimmed.empty() &&
                   ImGui::CalcTextSize((trimmed + ell).c_str()).x > availableWidth) {
                trimmed.pop_back();
            }
            filename = trimmed.empty() ? std::string() : trimmed + ell;
        }
        if (!filename.empty()) {
            drawList->AddText(
                ImVec2(clipMin.x + kLabelPaddingPx, clipMin.y + 4.0f),
                IM_COL32(255, 255, 255, 255),
                filename.c_str()
            );
        }
    }

    // Per-clip buffer-fill bar removed in Phase C.10 — frames now live in
    // an engine-global FrameCache, so per-clip fill % is no longer a
    // meaningful concept. A "this clip's working set is hot" indicator
    // could come back later by walking cache.entryCount per clip.

    // Draw keyframe markers if clip has animated properties. Iterate per
    // track / per keyframe so each marker reflects its own interpolation
    // shape and right-clicking targets the specific keyframe. Position
    // uses timelineFrameRate (kf.frame is layer-local in timeline frames,
    // matching renderPropertyTracks below).
    const auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);
    if (animProps && animProps->hasAnyKeyframes()) {
        const float size = 4.0f;
        const float keyframeY = clipMin.y + 8.0f;
        // Suppress redundant overdraw when multiple tracks share both
        // frame and shape — purely visual; data integrity is unaffected.
        std::set<std::pair<FrameNumber, int>> drawnShapes;

        for (const auto& trk : animProps->tracks) {
            for (const auto& kf : trk.keyframes) {
                const float kfSeconds =
                    static_cast<float>(kf.frame) /
                    static_cast<float>(timelineFrameRate);
                const float kfX = clipMin.x + (kfSeconds * m_pixelsPerSecond);
                if (kfX < clipMin.x || kfX > clipMax.x) continue;

                auto shapeKey = std::make_pair(
                    kf.frame, static_cast<int>(kf.interpolation));
                if (drawnShapes.insert(shapeKey).second) {
                    drawKeyframeShape(drawList, kf, kfX, keyframeY, size);
                }

                // Per-keyframe right-click → existing interpolation menu in
                // TimelineWidgetInput.cpp. The hit zone is wider than the
                // glyph to make small diamonds easier to grab.
                if (ImGui::IsMouseClicked(1)) {
                    const ImVec2 mousePos = ImGui::GetMousePos();
                    if (mousePos.x >= kfX - size - 3 && mousePos.x <= kfX + size + 3 &&
                        mousePos.y >= keyframeY - size - 3 && mousePos.y <= keyframeY + size + 3) {
                        m_keyframeEditClip = clipEntity;
                        m_keyframeEditProperty = trk.property;
                        m_keyframeEditFrame = kf.frame;
                        m_showKeyframeContextMenu = true;
                    }
                }
            }
        }
    }

    // Draw trim handles at clip edges
    // Show handles when clip is selected or when actively trimming this clip
    bool showHandles = isSelected || (m_isTrimmingClip && m_trimClip == clipEntity);

    if (showHandles && clipWidth > TRIM_EDGE_WIDTH * 4) {
        // Handle dimensions
        float handleWidth = TRIM_EDGE_WIDTH - 2.0f;
        float handleHeight = TRACK_HEIGHT - CLIP_PADDING * 4;
        float handleY = clipMin.y + (TRACK_HEIGHT - CLIP_PADDING * 2 - handleHeight) / 2;

        // Left trim handle
        ImVec2 leftHandleMin(clipMin.x + 1.0f, handleY);
        ImVec2 leftHandleMax(clipMin.x + 1.0f + handleWidth, handleY + handleHeight);

        // Highlight if actively trimming this edge
        ImU32 leftHandleColor = (m_isTrimmingClip && m_trimClip == clipEntity && m_trimEdge == ClipEdge::Left)
            ? IM_COL32(255, 200, 100, 255)   // Orange when trimming
            : IM_COL32(200, 200, 200, 180);  // Gray normally

        drawList->AddRectFilled(leftHandleMin, leftHandleMax, leftHandleColor, 2.0f);
        drawList->AddRect(leftHandleMin, leftHandleMax, IM_COL32(255, 255, 255, 200), 2.0f);

        // Right trim handle
        ImVec2 rightHandleMin(clipMax.x - 1.0f - handleWidth, handleY);
        ImVec2 rightHandleMax(clipMax.x - 1.0f, handleY + handleHeight);

        ImU32 rightHandleColor = (m_isTrimmingClip && m_trimClip == clipEntity && m_trimEdge == ClipEdge::Right)
            ? IM_COL32(255, 200, 100, 255)   // Orange when trimming
            : IM_COL32(200, 200, 200, 180);  // Gray normally

        drawList->AddRectFilled(rightHandleMin, rightHandleMax, rightHandleColor, 2.0f);
        drawList->AddRect(rightHandleMin, rightHandleMax, IM_COL32(255, 255, 255, 200), 2.0f);
    }

    // Property tracks are now rendered from renderTracks() to align with header panel
    return TRACK_HEIGHT;
}

float TimelineWidget::renderPropertyTracks(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos, float startY) {
    (void)trackIndex;  // Unused parameter
    if (!m_timeline) return 0.0f;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    auto& registry = m_timeline->getRegistry();

    // Read placement uniformly for Clip-backed, OA-layer and Generative-layer
    // entities. startFrame / duration live on Clip for clips, on Layer for
    // OA / Generative. Anything else is not a valid expand target.
    FrameNumber startFrame = 0;
    FrameNumber durationFrames = 0;
    if (const auto* clip = registry.try_get<Clip>(clipEntity)) {
        startFrame     = clip->startFrame;
        durationFrames = clip->duration;
    } else if (const auto* lay = registry.try_get<Layer>(clipEntity);
               lay && (registry.all_of<ObjectAnimationLayer>(clipEntity) ||
                       registry.all_of<GenerativeLayer>(clipEntity))) {
        startFrame     = lay->startFrame;
        durationFrames = lay->duration;
    } else {
        return 0.0f;
    }

    // Get AnimatedProperties for keyframe data
    auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);

    // Calculate layer timing for positioning keyframes — startFrame/duration
    // are timeline frames for both archetypes, so timeline framerate is the
    // correct denominator.
    double timelineFrameRate = m_timeline->getFrameRate();
    float startSeconds    = static_cast<float>(startFrame) / static_cast<float>(timelineFrameRate);
    float durationSeconds = static_cast<float>(durationFrames) / static_cast<float>(timelineFrameRate);
    Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
    float clipX = baseWindowPos.x + timeToPixel(startTime);
    float clipWidth = timeToPixel(static_cast<Timecode>(durationSeconds * 1000000.0f));

    // Property list varies by archetype (6 for Clip / Generative, 9 for OA).
    const std::vector<TimelinePropertyDef> properties =
        propertyListForEntity(clipEntity);
    const int numProperties = static_cast<int>(properties.size());

    for (int i = 0; i < numProperties; i++) {
        const TimelinePropertyDef& row = properties[i];
        float rowY = startY + i * PROPERTY_ROW_HEIGHT;

        // Row background — slightly darker for group headers so the
        // hierarchy reads at a glance.
        ImVec2 rowMin(baseWindowPos.x, rowY);
        ImVec2 rowMax(baseWindowPos.x + 4000.0f, rowY + PROPERTY_ROW_HEIGHT);
        drawList->AddRectFilled(rowMin, rowMax,
            row.isGroupHeader() ? IM_COL32(25, 28, 36, 255)
                                : IM_COL32(35, 40, 50, 255));

        // Group-header rows are visual separators only — no clip-area
        // lighter background, no diamonds, no track area.
        if (row.isGroupHeader()) continue;

        // Property-track area background under clip (slightly lighter)
        ImVec2 trackMin(clipX, rowY);
        ImVec2 trackMax(clipX + clipWidth, rowY + PROPERTY_ROW_HEIGHT);
        drawList->AddRectFilled(trackMin, trackMax, IM_COL32(50, 55, 65, 255));

        // Resolve the keyframe vector for this row. Transform rows read
        // AnimatedProperties (enum-keyed); EffectParam rows read
        // EffectAnimatedParameters (hash-keyed). drawKeyframeShape +
        // selection-ring emphasis logic is identical past that point.
        const std::vector<Keyframe>* kfs = nullptr;
        if (row.source == TimelinePropertyDef::Source::Transform) {
            const KeyframeTrack* track = animProps
                ? animProps->getTrack(row.prop) : nullptr;
            if (track && track->hasKeyframes()) kfs = &track->keyframes;
        } else { // EffectParam
            if (registry.valid(row.effectEntity)) {
                if (const auto* eap =
                        registry.try_get<EffectAnimatedParameters>(row.effectEntity))
                {
                    for (const auto& nt : eap->tracks) {
                        if (nt.paramKeyHash == row.paramHash) {
                            if (!nt.keyframes.empty()) kfs = &nt.keyframes;
                            break;
                        }
                    }
                }
            }
        }
        if (!kfs) continue;

        const float size = 5.0f;
        const float keyframeY = rowY + PROPERTY_ROW_HEIGHT / 2.0f;

        // Identity of this row, shared by every keyframe drawn on it.
        KeyframeRef rowRef;
        rowRef.clip     = clipEntity;
        rowRef.isEffect = (row.source == TimelinePropertyDef::Source::EffectParam);
        if (rowRef.isEffect) {
            rowRef.effectEntity = row.effectEntity;
            rowRef.paramHash    = row.paramHash;
        } else {
            rowRef.prop = row.prop;
        }

        for (const auto& kf : *kfs) {
            KeyframeRef ref = rowRef;
            ref.frame = kf.frame;

            const bool isSelected = isKeyframeSelected(ref);

            // A drag moves the WHOLE selection by one delta, so every selected
            // keyframe previews at frame + delta.
            const bool isDragged = m_isDraggingKeyframe && isSelected;
            const FrameNumber drawFrame =
                isDragged ? kf.frame + m_dragDelta : kf.frame;

            const float kfSeconds =
                static_cast<float>(drawFrame) / static_cast<float>(timelineFrameRate);
            const float kfX = clipX + (kfSeconds * m_pixelsPerSecond);

            if (kfX >= clipX && kfX <= clipX + clipWidth) {
                drawKeyframeShape(drawList, kf, kfX, keyframeY, size);

                if (isSelected || isDragged) {
                    drawList->AddCircle(ImVec2(kfX, keyframeY), size + 3.0f,
                                        IM_COL32(255, 255, 255, 255), 0, 2.0f);
                }

                // Feed the per-frame glyph cache. The hit-test and box-select read
                // this instead of re-deriving glyph geometry, so what you can click
                // is exactly what was drawn. Cache the keyframe's REAL frame, not
                // the drag preview — a drag is uncommitted.
                m_keyframeGlyphs.push_back(
                    KeyframeGlyph{ref, ImVec2(kfX, keyframeY), size});
            }
        }
    }

    return numProperties * PROPERTY_ROW_HEIGHT;
}

void TimelineWidget::renderPlayhead() {
    if (!m_timeline) return;

    // Use foreground draw list to render on top of child windows
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    // Playhead X in content pixels. m_rulerScreenPos / m_tracksScreenPos are
    // captured via GetCursorScreenPos() INSIDE the scrolled child windows, so
    // they already include the scroll offset. Do NOT subtract m_syncScrollX
    // here — that double-counts scroll and the playhead drifts left of its
    // true position, off-screen entirely at large scroll / high zoom.
    Timecode currentTime = m_timeline->getCurrentTime();
    float playheadPixel = timeToPixel(currentTime);

    // Playhead X in ruler space
    float playheadXRuler = m_rulerScreenPos.x + playheadPixel;

    // Playhead X in tracks space
    float playheadXTracks = m_tracksScreenPos.x + playheadPixel;

    // Draw playhead line spanning from ruler through tracks
    drawList->AddLine(
        ImVec2(playheadXRuler, m_rulerScreenPos.y),
        ImVec2(playheadXTracks, m_tracksScreenPos.y + m_tracksHeight),
        IM_COL32(255, 100, 100, 255),
        2.0f
    );

    // Draw playhead triangle at top of ruler
    ImVec2 trianglePoints[3] = {
        ImVec2(playheadXRuler, m_rulerScreenPos.y),
        ImVec2(playheadXRuler - 6.0f, m_rulerScreenPos.y + 10.0f),
        ImVec2(playheadXRuler + 6.0f, m_rulerScreenPos.y + 10.0f)
    };
    drawList->AddTriangleFilled(trianglePoints[0], trianglePoints[1], trianglePoints[2],
                                IM_COL32(255, 100, 100, 255));

    // Draw snap indicator when actively snapping
    if (m_isSnapping) {
        float snapPixel = timeToPixel(m_snapTargetTime);
        float snapXRuler = m_rulerScreenPos.x + snapPixel;
        float snapXTracks = m_tracksScreenPos.x + snapPixel;

        // Draw bright cyan snap line (wider than playhead)
        drawList->AddLine(
            ImVec2(snapXRuler, m_rulerScreenPos.y),
            ImVec2(snapXTracks, m_tracksScreenPos.y + m_tracksHeight),
            IM_COL32(0, 255, 255, 200),  // Cyan color
            3.0f
        );

        // Draw small diamonds at top and bottom of snap line
        float diamondSize = 5.0f;
        ImVec2 topDiamond[4] = {
            ImVec2(snapXRuler, m_rulerScreenPos.y - diamondSize),
            ImVec2(snapXRuler - diamondSize, m_rulerScreenPos.y),
            ImVec2(snapXRuler, m_rulerScreenPos.y + diamondSize),
            ImVec2(snapXRuler + diamondSize, m_rulerScreenPos.y)
        };
        drawList->AddQuadFilled(topDiamond[0], topDiamond[1], topDiamond[2], topDiamond[3],
                                IM_COL32(0, 255, 255, 255));
    }
}

void TimelineWidget::renderTrackHeaderPanel(float panelHeight) {
    if (!m_timeline) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    // Draw panel background
    ImVec2 panelMin = windowPos;
    ImVec2 panelMax(windowPos.x + TRACK_HEADER_WIDTH - 4, windowPos.y + panelHeight + 100);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(40, 42, 48, 255));

    // Iterate the per-frame row cache so the header panel stays pixel-locked to
    // the lanes (same visible set, same heights) and shy tracks are skipped.
    // Each header row is positioned at the row's content-space Y relative to
    // the header inner child's origin (windowPos).
    for (const auto& row : m_trackRows) {
        renderTrackHeaderRow(row.track, row.modelIndex, windowPos.y + row.y);
    }
}

float TimelineWidget::renderTrackHeaderRow(entt::entity trackEntity, int trackIndex, float rowY) {
    if (!m_timeline) return TRACK_HEIGHT + TRACK_PADDING;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    auto& registry = m_timeline->getRegistry();

    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return TRACK_HEIGHT + TRACK_PADDING;

    uint32_t trackId = static_cast<uint32_t>(trackEntity);
    bool isExpanded = m_expandedTracks.count(trackId) > 0 || m_timeline->isTrackExpanded(trackEntity);

    // Track header background (matches track height)
    float headerX = windowPos.x + 2;
    ImVec2 headerMin(headerX, rowY);
    ImVec2 headerMax(headerX + TRACK_HEADER_WIDTH - 8, rowY + TRACK_HEIGHT);

    // Alternating background colors
    ImU32 bgColor = (trackIndex % 2 == 0)
        ? IM_COL32(50, 52, 58, 255)
        : IM_COL32(45, 47, 53, 255);
    drawList->AddRectFilled(headerMin, headerMax, bgColor);

    // Twirl-down triangle
    float triX = headerX + 12.0f;
    float triY = rowY + TRACK_HEIGHT / 2.0f;
    float triSize = 5.0f;

    ImVec2 triPoints[3];
    if (isExpanded) {
        // Down-pointing triangle (expanded)
        triPoints[0] = ImVec2(triX - triSize, triY - triSize / 2);
        triPoints[1] = ImVec2(triX + triSize, triY - triSize / 2);
        triPoints[2] = ImVec2(triX, triY + triSize / 2);
    } else {
        // Right-pointing triangle (collapsed)
        triPoints[0] = ImVec2(triX - triSize / 2, triY - triSize);
        triPoints[1] = ImVec2(triX + triSize / 2, triY);
        triPoints[2] = ImVec2(triX - triSize / 2, triY + triSize);
    }

    // Only show triangle if track has clips
    bool hasClips = !track->layers.empty();
    if (hasClips) {
        drawList->AddTriangleFilled(triPoints[0], triPoints[1], triPoints[2], IM_COL32(180, 180, 180, 255));
    }

    // Track name — use TrackUiState.name when set, fall back to "Track N".
    const auto* trackUiState = registry.try_get<TrackUiState>(trackEntity);
    std::string displayName;
    if (trackUiState && !trackUiState->name.empty()) {
        displayName = trackUiState->name;
    } else {
        displayName = "Track " + std::to_string(trackIndex + 1);
    }

    // Name hit rect (used for double-click detection and InputText overlay).
    // Right side of the header row is laid out as three non-overlapping cells
    // (Phase 9): name rect | clip-count badge | shy glyph. The name rect ends
    // before the badge; the badge sits left of the glyph; the glyph owns the
    // far-right ~20px. Keep these three bounds in sync if you move any of them.
    constexpr float kShyGlyphCellW = 22.0f;  // far-right cell for the shy "S" glyph
    constexpr float kBadgeCellW    = 34.0f;  // clip-count badge cell, left of the glyph
    float nameX = headerX + 26;
    float nameY = rowY + TRACK_HEIGHT / 2.0f - 7.0f;
    ImVec2 nameMin(nameX, rowY + 2);
    ImVec2 nameMax(headerX + TRACK_HEADER_WIDTH - kShyGlyphCellW - kBadgeCellW,
                   rowY + TRACK_HEIGHT - 2);

    if (m_renamingTrackIndex == trackIndex) {
        // Stale-index guard: if the track count changed since the rename opened
        // (insert/delete while overlay is up), cancel silently — the positional
        // index no longer maps to the intended track.
        if (m_timeline->getTrackCount() != m_renameTrackCount) {
            m_renamingTrackIndex = -1;
            m_renameFocusPending = false;
            m_renameWasActive    = false;
        }
    }

    if (m_renamingTrackIndex == trackIndex) {
        // Inline rename overlay. Focus/commit protocol mirrors MathInput.cpp's
        // justEntered/wasActive shape to avoid the first-frame false-commit:
        //
        //   Frame 0 (entry): m_renameFocusPending==true
        //     → call SetKeyboardFocusHere() before InputText (queues focus)
        //     → clear m_renameFocusPending; set m_renameWasActive=false
        //     → SKIP commit/cancel check — the widget hasn't been active yet
        //       so IsItemActive()==false would fire an instant spurious commit
        //
        //   Frame 1+: m_renameFocusPending==false, widget has been activated
        //     → track m_renameWasActive via IsItemActive()
        //     → commit on EnterReturnsTrue or IsItemDeactivatedAfterEdit()
        //       (IsItemDeactivatedAfterEdit requires prior activation, so it
        //       won't fire on a widget that was never focused)
        //     → Esc cancels without enqueueing a command
        ImGui::SetCursorScreenPos(nameMin);
        ImGui::SetNextItemWidth(nameMax.x - nameMin.x);
        char inputId[32];
        std::snprintf(inputId, sizeof(inputId), "##trackRename%d", trackIndex);

        const bool focusPendingThisFrame = m_renameFocusPending;
        if (focusPendingThisFrame) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocusPending = false;
            m_renameWasActive    = false;
        }

        bool enterPressed = ImGui::InputText(inputId, m_renameTrackBuf,
                                             sizeof(m_renameTrackBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue |
                                             ImGuiInputTextFlags_AutoSelectAll);
        const bool isActive         = ImGui::IsItemActive();
        const bool deactAfterEdit   = ImGui::IsItemDeactivatedAfterEdit();

        if (!focusPendingThisFrame) {
            // Update wasActive state so focus-loss detection is reliable.
            if (isActive) m_renameWasActive = true;

            // Esc cancels — check before commit so they don't both fire.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                m_renamingTrackIndex = -1;
                m_renameWasActive    = false;
            } else if (enterPressed || (m_renameWasActive && deactAfterEdit)) {
                // Enter pressed, or focus lost after the widget was active.
                if (m_commandDispatcher) {
                    std::string newName(m_renameTrackBuf);
                    m_commandDispatcher->enqueue(
                        std::make_unique<RenameTrackCommand>(trackIndex,
                                                            std::move(newName)));
                }
                m_renamingTrackIndex = -1;
                m_renameWasActive    = false;
            }
        }
    } else {
        // Normal display: draw name text, handle double-click to enter rename.
        drawList->AddText(ImVec2(nameX, nameY),
                          IM_COL32(220, 220, 220, 255), displayName.c_str());

        // Invisible button over the name rect to catch double-click.
        ImGui::SetCursorScreenPos(nameMin);
        std::string btnId = "##trackNameBtn" + std::to_string(trackIndex);
        ImGui::InvisibleButton(btnId.c_str(),
                               ImVec2(nameMax.x - nameMin.x, nameMax.y - nameMin.y));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            m_renamingTrackIndex = trackIndex;
            m_renameFocusPending = true;
            m_renameWasActive    = false;
            m_renameTrackCount   = m_timeline->getTrackCount();
            std::strncpy(m_renameTrackBuf,
                         (trackUiState ? trackUiState->name.c_str() : ""),
                         sizeof(m_renameTrackBuf) - 1);
            m_renameTrackBuf[sizeof(m_renameTrackBuf) - 1] = '\0';
        }
    }

    // Clip count badge — in its own cell, left of the shy-glyph cell.
    if (hasClips) {
        std::ostringstream clipCount;
        clipCount << "(" << track->layers.size() << ")";
        const float badgeX = headerX + TRACK_HEADER_WIDTH - kShyGlyphCellW - kBadgeCellW + 2.0f;
        drawList->AddText(ImVec2(badgeX, rowY + TRACK_HEIGHT / 2 - 7),
                          IM_COL32(120, 120, 120, 255), clipCount.str().c_str());
    }

    // Per-track shy toggle glyph (Phase 9). Small "S" badge at the far right of
    // the header row: dim outline when not shy, filled accent when shy. Click
    // toggles TrackUiState::shy (direct view-state write, not undoable). When
    // the global hide toggle is on, flagging a track shy drops it from the row
    // cache next frame; with global hide off, the badge just marks intent.
    {
        const auto* uiShy = registry.try_get<TrackUiState>(trackEntity);
        const bool isShy = uiShy && uiShy->shy;
        // Centered in the dedicated far-right glyph cell (no overlap with the
        // clip-count badge or the name/rename rect, all of which end before it).
        const float glyphCx = headerX + TRACK_HEADER_WIDTH - kShyGlyphCellW * 0.5f;
        const float glyphCy = rowY + TRACK_HEIGHT / 2.0f;
        const float glyphR  = 7.0f;
        const ImU32 shyCol = isShy ? IM_COL32(230, 180, 90, 255)   // amber when shy
                                   : IM_COL32(110, 110, 120, 255); // dim when not
        drawList->AddCircle(ImVec2(glyphCx, glyphCy), glyphR, shyCol, 0, 1.5f);
        if (isShy) {
            drawList->AddCircleFilled(ImVec2(glyphCx, glyphCy), glyphR - 2.5f, shyCol);
        }
        const ImVec2 sSize = ImGui::CalcTextSize("S");
        drawList->AddText(ImVec2(glyphCx - sSize.x * 0.5f, glyphCy - sSize.y * 0.5f),
                          isShy ? IM_COL32(40, 40, 45, 255) : shyCol, "S");

        if (ImGui::IsMouseClicked(0)) {
            const ImVec2 mp = ImGui::GetMousePos();
            const float dx = mp.x - glyphCx, dy = mp.y - glyphCy;
            if (dx * dx + dy * dy <= (glyphR + 2.0f) * (glyphR + 2.0f)) {
                auto& uiState = registry.get_or_emplace<TrackUiState>(trackEntity);
                uiState.shy = !uiState.shy;
            }
        }
    }

    // Handle click on twirl-down triangle
    if (hasClips && ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 triHitMin(triX - triSize - 6, triY - triSize - 6);
        ImVec2 triHitMax(triX + triSize + 6, triY + triSize + 6);

        if (mousePos.x >= triHitMin.x && mousePos.x <= triHitMax.x &&
            mousePos.y >= triHitMin.y && mousePos.y <= triHitMax.y) {
            if (isExpanded) {
                m_expandedTracks.erase(trackId);
            } else {
                m_expandedTracks.insert(trackId);
            }
        }
    }

    float totalHeight = TRACK_HEIGHT + TRACK_PADDING;

    // Track-expanded: render the property panel (per-property name + nav
    // arrows + keyframe diamond + value) for the clip at the playhead. The
    // body side draws the keyframe-diamond rows with matching Y positions.
    if (isExpanded) {
        entt::entity clipAtPlayhead = findClipAtPlayhead(trackEntity);
        if (clipAtPlayhead != entt::null) {
            float propsHeight = renderClipPropertyPanel(clipAtPlayhead, rowY + TRACK_HEIGHT);
            totalHeight += propsHeight;
        }
    }

    return totalHeight;
}

float TimelineWidget::renderClipPropertyPanel(entt::entity clipEntity, float rowY) {
    if (!m_timeline) return 0.0f;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    auto& registry = m_timeline->getRegistry();

    // Resolve layer placement (startFrame) for all archetypes — OA / Generative
    // read from Layer, Clip from Clip. Returns 0 if no archetype matches.
    FrameNumber layerStart = 0;
    if (const auto* clip = registry.try_get<Clip>(clipEntity)) {
        layerStart = clip->startFrame;
    } else if (const auto* lay = registry.try_get<Layer>(clipEntity);
               lay && (registry.all_of<ObjectAnimationLayer>(clipEntity) ||
                       registry.all_of<GenerativeLayer>(clipEntity))) {
        layerStart = lay->startFrame;
    } else {
        return 0.0f;
    }

    // Render rows directly under the track, one INDENT_WIDTH in. AE-style
    // hierarchy: group-header rows are clickable to collapse/expand;
    // property rows render the keyframe controls at their right edge.
    {
        float propY = rowY;
        const float baseX = windowPos.x + 2 + INDENT_WIDTH;
        const float baseWidth = TRACK_HEADER_WIDTH - 8 - INDENT_WIDTH;
        constexpr float kDepthIndent = 12.0f;  // px per depth level

        auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);
        const bool isOA = registry.all_of<ObjectAnimationLayer>(clipEntity);
        const FrameNumber currentFrame = m_timeline->getCurrentFrame();
        const FrameNumber localFrame = currentFrame - layerStart;

        const std::vector<TimelinePropertyDef> rows =
            propertyListForEntity(clipEntity);

        for (const TimelinePropertyDef& row : rows) {
            const float depthIndent = static_cast<float>(row.depth) * kDepthIndent;
            const float propX = baseX + depthIndent;
            const float propWidth = baseWidth - depthIndent;
            const ImVec2 propMin(propX, propY);
            const ImVec2 propMax(propX + propWidth, propY + PROPERTY_ROW_HEIGHT);
            const float centerY = propY + PROPERTY_ROW_HEIGHT / 2.0f;

            if (row.source == TimelinePropertyDef::Source::GroupHeader) {
                // Group header: clickable row with twirl arrow + label.
                drawList->AddRectFilled(propMin, propMax, IM_COL32(30, 32, 38, 255));
                const bool collapsed = isGroupCollapsed(clipEntity, row.groupPath);
                // Twirl arrow (right when collapsed, down when expanded).
                const float ax = propX + 4.0f;
                const float ay = centerY;
                const float as = 4.0f;
                ImVec2 tri[3];
                if (collapsed) {
                    tri[0] = ImVec2(ax,        ay - as);
                    tri[1] = ImVec2(ax + as,   ay);
                    tri[2] = ImVec2(ax,        ay + as);
                } else {
                    tri[0] = ImVec2(ax - as,   ay - as / 2.0f);
                    tri[1] = ImVec2(ax + as,   ay - as / 2.0f);
                    tri[2] = ImVec2(ax,        ay + as);
                }
                drawList->AddTriangleFilled(tri[0], tri[1], tri[2],
                                            IM_COL32(190, 190, 190, 255));
                drawList->AddText(ImVec2(propX + 18.0f, propY + 2),
                                  IM_COL32(210, 210, 210, 255),
                                  row.groupLabel.c_str());

                // Click anywhere on the row toggles collapse.
                if (ImGui::IsMouseClicked(0)) {
                    const ImVec2 mp = ImGui::GetMousePos();
                    if (mp.x >= propX && mp.x <= propX + propWidth &&
                        mp.y >= propY && mp.y <= propY + PROPERTY_ROW_HEIGHT)
                    {
                        toggleGroupCollapsed(clipEntity, row.groupPath);
                    }
                }

                propY += PROPERTY_ROW_HEIGHT;
                continue;
            }

            // ── Property row: Transform or EffectParam ─────────────────────
            drawList->AddRectFilled(propMin, propMax, IM_COL32(42, 44, 50, 255));

            // Resolve keyframe state — different source for Transform vs
            // EffectParam, same shape of output (hasKeyframes / atKeyframe /
            // currentValue).
            bool hasKeyframes = false;
            bool atKeyframe   = false;
            float currentValue = row.defaultValue;
            // For Transform rows, neighbour-keyframe lookup walks
            // AnimatedProperties. For EffectParam rows, we walk the
            // matching NamedTrack on EffectAnimatedParameters.
            std::vector<Keyframe> kfSnapshot;  // small, hot-path-ok copy

            if (row.source == TimelinePropertyDef::Source::Transform) {
                const KeyframeTrack* track = animProps
                    ? animProps->getTrack(row.prop) : nullptr;
                if (track && track->hasKeyframes()) {
                    hasKeyframes = true;
                    atKeyframe = track->getKeyframeAt(localFrame) != nullptr;
                    kfSnapshot = track->keyframes;
                    // Keyframed: evaluated value is the interpolated result.
                    currentValue = animProps->evaluate(row.prop, localFrame);
                } else if (isOA) {
                    // OA with no keyframes yet: default (0 for pos/rot, 1 for
                    // scale) mirrors renderOAValueRow:1450-1455.
                    const bool isScale = (row.prop == AnimatableProperty::ScaleX ||
                                          row.prop == AnimatableProperty::ScaleY ||
                                          row.prop == AnimatableProperty::ScaleZ);
                    currentValue = isScale ? 1.0f : 0.0f;
                } else {
                    // Non-OA, non-keyframed: read the live component so the
                    // DragFloat shows and scrubs the actual current value.
                    // Mirrors PropertyWindow.cpp:310-312 / 330-334 / 395-396 / 457.
                    const auto* transform = registry.try_get<Transform>(clipEntity);
                    const auto* ml        = registry.try_get<MediaLayer>(clipEntity);
                    switch (row.prop) {
                        case AnimatableProperty::PositionX:
                            if (transform) currentValue = transform->position.x; break;
                        case AnimatableProperty::PositionY:
                            if (transform) currentValue = transform->position.y; break;
                        case AnimatableProperty::PositionZ:
                            if (transform) currentValue = transform->position.z; break;
                        case AnimatableProperty::Rotation:
                            if (transform) currentValue = transform->rotation.z; break;
                        case AnimatableProperty::RotationX:
                            if (transform) currentValue = transform->rotation.x; break;
                        case AnimatableProperty::RotationY:
                            if (transform) currentValue = transform->rotation.y; break;
                        case AnimatableProperty::RotationZ:
                            if (transform) currentValue = transform->rotation.z; break;
                        case AnimatableProperty::ScaleX:
                            if (transform) currentValue = transform->scale.x; break;
                        case AnimatableProperty::ScaleY:
                            if (transform) currentValue = transform->scale.y; break;
                        case AnimatableProperty::ScaleZ:
                            if (transform) currentValue = transform->scale.z; break;
                        case AnimatableProperty::Opacity:
                            if (ml) currentValue = ml->opacity; break;
                        default: break;
                    }
                }
            } else { // EffectParam
                if (registry.valid(row.effectEntity)) {
                    if (const auto* eap =
                            registry.try_get<EffectAnimatedParameters>(row.effectEntity))
                    {
                        for (const auto& nt : eap->tracks) {
                            if (nt.paramKeyHash == row.paramHash) {
                                if (!nt.keyframes.empty()) {
                                    hasKeyframes = true;
                                    kfSnapshot = nt.keyframes;
                                    for (const auto& kf : nt.keyframes) {
                                        if (kf.frame == localFrame) {
                                            atKeyframe = true; break;
                                        }
                                    }
                                    currentValue = evaluateKeyframes(
                                        nt.keyframes, localFrame, row.defaultValue);
                                }
                                break;
                            }
                        }
                    }
                    // Fallback to the static EffectParameters slot value
                    // if no keyframes (so the panel mirrors what
                    // PropertyWindow shows).
                    if (!hasKeyframes) {
                        if (const auto* ep =
                                registry.try_get<EffectParameters>(row.effectEntity))
                        {
                            // Match the slot by re-hashing param names
                            // against the registry — we don't store slot
                            // index on TimelinePropertyDef.
                            if (m_effectKindRegistry) {
                                if (const auto* fx =
                                        registry.try_get<Effect>(row.effectEntity))
                                {
                                    const effects::EffectKind* kind =
                                        m_effectKindRegistry->find(fx->kindId);
                                    if (kind) {
                                        for (std::size_t i = 0;
                                             i < kind->params.size(); ++i)
                                        {
                                            if (effects::fnv1a32(kind->params[i].name) ==
                                                row.paramHash)
                                            {
                                                if (i < ep->values.size() &&
                                                    ep->values[i].type ==
                                                    ParamValue::Type::Float)
                                                {
                                                    currentValue = ep->values[i].f4[0];
                                                }
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // === PROPERTY NAME ===
            drawList->AddText(ImVec2(propX + 4, propY + 2),
                              IM_COL32(140, 140, 140, 255), row.shortName.c_str());

            // === KEYFRAME CONTROLS (right side of row) ===
            const float controlsX = propX + propWidth - 70;
            const float arrowSize = 3.0f;
            const float diamondSize = 4.0f;

            // Stopwatch (AE-style animation toggle), left of the prev-keyframe arrow.
            // Lit == this property is animated. Clicking it OFF deletes every keyframe
            // on the property; clicking it ON starts animating from the current value.
            // The diamond next to it is still the single-keyframe add/remove.
            const float stopwatchX = controlsX - 14.0f;
            const float stopwatchR = 4.5f;
            const ImU32 stopwatchCol = hasKeyframes
                ? IM_COL32(255, 200, 50, 255) : IM_COL32(90, 90, 90, 255);
            drawList->AddCircle(ImVec2(stopwatchX, centerY), stopwatchR, stopwatchCol, 10, 1.2f);
            if (hasKeyframes) {
                drawList->AddCircleFilled(ImVec2(stopwatchX, centerY), 1.8f, stopwatchCol, 8);
            }
            // The winding stem, so it reads as a stopwatch and not just a dot.
            drawList->AddLine(ImVec2(stopwatchX, centerY - stopwatchR - 2.0f),
                              ImVec2(stopwatchX, centerY - stopwatchR),
                              stopwatchCol, 1.2f);

            const float leftArrowX = controlsX;
            ImVec2 leftArrow[3] = {
                ImVec2(leftArrowX - arrowSize, centerY),
                ImVec2(leftArrowX + arrowSize, centerY - arrowSize),
                ImVec2(leftArrowX + arrowSize, centerY + arrowSize)
            };
            const ImU32 arrowColor = hasKeyframes
                ? IM_COL32(180, 180, 180, 255) : IM_COL32(80, 80, 80, 255);
            drawList->AddTriangleFilled(leftArrow[0], leftArrow[1], leftArrow[2], arrowColor);

            const float diamondX = controlsX + 14;
            ImVec2 diamond[4] = {
                ImVec2(diamondX, centerY - diamondSize),
                ImVec2(diamondX + diamondSize, centerY),
                ImVec2(diamondX, centerY + diamondSize),
                ImVec2(diamondX - diamondSize, centerY)
            };
            if (atKeyframe) {
                drawList->AddConvexPolyFilled(diamond, 4, IM_COL32(255, 200, 50, 255));
            } else if (hasKeyframes) {
                drawList->AddPolyline(diamond, 4, IM_COL32(180, 180, 180, 255),
                                      ImDrawFlags_Closed, 1.0f);
            } else {
                drawList->AddPolyline(diamond, 4, IM_COL32(80, 80, 80, 255),
                                      ImDrawFlags_Closed, 1.0f);
            }

            const float rightArrowX = controlsX + 28;
            ImVec2 rightArrow[3] = {
                ImVec2(rightArrowX + arrowSize, centerY),
                ImVec2(rightArrowX - arrowSize, centerY - arrowSize),
                ImVec2(rightArrowX - arrowSize, centerY + arrowSize)
            };
            drawList->AddTriangleFilled(rightArrow[0], rightArrow[1], rightArrow[2], arrowColor);

            // === CURRENT VALUE (editable DragFloat) ===
            // Placed at the right of the keyframe controls (arrows+diamond
            // end at controlsX+35; value rect starts at controlsX+38).
            // Unique ImGui ID: PushID(entity uint32)+row index so rows on
            // different entities don't share widget state.
            //
            // Commit semantics mirror PropertyWindow.cpp ~404-450:
            //   IsItemActivated -> capture pre-drag scalar + keyframe state
            //   drag changes   -> live component write + keyframe upsert when animated
            //   IsItemDeactivatedAfterEdit -> enqueue undoable command
            //
            // Opacity: displayed as 0-100 (stored 0-1); DragFloat works on a
            // local scaled copy and rescales on write.
            // Position / Scale: live-write-only (no scalar command — matches
            // PropertyWindow's current shape for those two channels).
            // Rotation: emits SetClipRotationCommand when unKeyframed,
            //   UpsertKeyframeCommand when keyframed.

            const float valX    = controlsX + 38.0f;
            const float valW    = (propX + propWidth) - valX;

            // Build per-row value in display space (opacity 0-100, others raw).
            const bool isOpacity = (row.source == TimelinePropertyDef::Source::Transform &&
                                    row.prop == AnimatableProperty::Opacity);
            float dragVal = isOpacity ? currentValue * 100.0f : currentValue;

            // DragFloat drag params per property.
            float dragSpeed  = 0.5f;
            float dragMin    = 0.0f;
            float dragMax    = 0.0f;  // 0,0 = unclamped
            const char* fmt  = "%.2f";
            if (isOpacity) {
                dragSpeed = 0.5f; dragMin = 0.0f; dragMax = 100.0f; fmt = "%.0f%%";
            } else if (row.source == TimelinePropertyDef::Source::Transform) {
                switch (row.prop) {
                    case AnimatableProperty::Rotation:
                    case AnimatableProperty::RotationX:
                    case AnimatableProperty::RotationY:
                    case AnimatableProperty::RotationZ:
                        dragSpeed = 0.5f; fmt = "%.1f"; break;
                    case AnimatableProperty::ScaleX:
                    case AnimatableProperty::ScaleY:
                    case AnimatableProperty::ScaleZ:
                        dragSpeed = 0.01f; dragMin = 0.01f; fmt = "%.3f"; break;
                    default:  // PositionX/Y/Z
                        dragSpeed = 1.0f; fmt = "%.1f"; break;
                }
            } else {  // EffectParam
                dragSpeed = 0.01f; fmt = "%.3f";
            }

            ImGui::PushID(static_cast<int>(static_cast<uint32_t>(clipEntity)));
            ImGui::PushID(static_cast<int>(&row - rows.data()));
            ImGui::SetCursorScreenPos(ImVec2(valX, propY + 1));
            ImGui::SetNextItemWidth(valW);
            const bool dragChanged = ImGui::DragFloat("##propVal", &dragVal,
                                                      dragSpeed, dragMin, dragMax, fmt);

            // --- IsItemActivated: capture pre-drag state ---
            if (ImGui::IsItemActivated()) {
                m_twirldownPreEdit.entity       = clipEntity;
                m_twirldownPreEdit.isEffect     = (row.source == TimelinePropertyDef::Source::EffectParam);
                m_twirldownPreEdit.propKey      = m_twirldownPreEdit.isEffect
                    ? static_cast<int>(row.paramHash)
                    : static_cast<int>(row.prop);
                m_twirldownPreEdit.scalarValue  = currentValue;
                m_twirldownPreEdit.wasKeyframed = false;
                m_twirldownPreEdit.keyframeValue.reset();
                m_twirldownPreEdit.linkedScaleAxes.clear();

                // A locked scale drag writes the other two axes as well, so snapshot
                // their pre-edit keyframe state now — the commit below emits one
                // UpsertKeyframeCommand per animated axis.
                if (!isOA && row.source == TimelinePropertyDef::Source::Transform &&
                    isScaleProp(row.prop) && localFrame >= 0 &&
                    isScaleLocked(registry, clipEntity))
                {
                    for (AnimatableProperty axis : {AnimatableProperty::ScaleX,
                                                    AnimatableProperty::ScaleY,
                                                    AnimatableProperty::ScaleZ})
                    {
                        if (axis == row.prop) continue;
                        const KeyframeTrack* t = animProps ? animProps->getTrack(axis) : nullptr;
                        if (!t || !t->hasKeyframes()) continue;  // static axis — no kf to undo

                        TwirldownPreEdit::LinkedAxis linked;
                        linked.prop         = axis;
                        linked.wasKeyframed = true;
                        const Keyframe* existing = t->getKeyframeAt(localFrame);
                        linked.keyframeValue =
                            existing ? std::optional<float>(existing->value) : std::nullopt;
                        m_twirldownPreEdit.linkedScaleAxes.push_back(linked);
                    }
                }

                if (isOA && row.source == TimelinePropertyDef::Source::Transform) {
                    // OA: every edit is a keyframe upsert regardless of whether
                    // a keyframe already exists at this frame. Capture the frame
                    // and the existing kf value (nullopt when none), so undo can
                    // remove a freshly-created keyframe. Mirrors renderOAValueRow
                    // (PropertyWindow.cpp:1469-1478).
                    m_twirldownPreEdit.wasKeyframed  = true;
                    m_twirldownPreEdit.keyframeFrame = localFrame;
                    if (animProps) {
                        const KeyframeTrack* kfTrack = animProps->getTrack(row.prop);
                        if (kfTrack) {
                            const Keyframe* existing = kfTrack->getKeyframeAt(localFrame);
                            m_twirldownPreEdit.keyframeValue =
                                existing ? std::optional<float>(existing->value) : std::nullopt;
                        }
                        // else: nullopt already set — no kf existed, undo removes it
                    }
                } else if (hasKeyframes && localFrame >= 0) {
                    m_twirldownPreEdit.wasKeyframed    = true;
                    m_twirldownPreEdit.keyframeFrame   = localFrame;
                    if (row.source == TimelinePropertyDef::Source::Transform && animProps) {
                        const KeyframeTrack* kfTrack = animProps->getTrack(row.prop);
                        if (kfTrack) {
                            const Keyframe* existing = kfTrack->getKeyframeAt(localFrame);
                            m_twirldownPreEdit.keyframeValue =
                                existing ? std::optional<float>(existing->value) : std::nullopt;
                        }
                    } else if (row.source == TimelinePropertyDef::Source::EffectParam &&
                               registry.valid(row.effectEntity)) {
                        if (const auto* eap =
                                registry.try_get<EffectAnimatedParameters>(row.effectEntity))
                        {
                            for (const auto& nt : eap->tracks) {
                                if (nt.paramKeyHash == row.paramHash) {
                                    const Keyframe* existing = nullptr;
                                    for (const auto& kf : nt.keyframes)
                                        if (kf.frame == localFrame) { existing = &kf; break; }
                                    m_twirldownPreEdit.keyframeValue =
                                        existing ? std::optional<float>(existing->value)
                                                 : std::nullopt;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // --- Live change: write component + upsert keyframe when animated ---
            if (dragChanged) {
                const float writtenVal = isOpacity ? dragVal / 100.0f : dragVal;
                if (row.source == TimelinePropertyDef::Source::Transform) {
                    if (isOA) {
                        // OA layers drive a target via keyframes only (ADR-0020);
                        // they have no own Transform to write. Always upsert a
                        // keyframe at the current layer-local frame, mirroring
                        // renderOAValueRow (PropertyWindow.cpp:1481-1487).
                        auto& animPropsMut =
                            registry.get_or_emplace<AnimatedProperties>(clipEntity);
                        KeyframeTrack& mutableTrack =
                            animPropsMut.getOrCreateTrack(row.prop);
                        mutableTrack.addKeyframe(localFrame, writtenVal);
                    } else {
                        auto* transform = registry.try_get<Transform>(clipEntity);
                        auto* ml        = registry.try_get<MediaLayer>(clipEntity);

                        // Captured before the switch overwrites the axis — the
                        // uniform-scale ratio below needs the pre-drag value.
                        const float prevAxis =
                            (transform && isScaleProp(row.prop))
                                ? scaleAxis(*transform, row.prop) : 0.0f;

                        switch (row.prop) {
                            case AnimatableProperty::PositionX:
                                if (transform) transform->setPosition(
                                    glm::vec3(writtenVal, transform->position.y, transform->position.z));
                                break;
                            case AnimatableProperty::PositionY:
                                if (transform) transform->setPosition(
                                    glm::vec3(transform->position.x, writtenVal, transform->position.z));
                                break;
                            case AnimatableProperty::PositionZ:
                                if (transform) transform->setPosition(
                                    glm::vec3(transform->position.x, transform->position.y, writtenVal));
                                break;
                            case AnimatableProperty::Rotation:
                                if (transform) transform->setRotation(
                                    glm::vec3(transform->rotation.x, transform->rotation.y, writtenVal));
                                break;
                            case AnimatableProperty::RotationX:
                                if (transform) transform->setRotation(
                                    glm::vec3(writtenVal, transform->rotation.y, transform->rotation.z));
                                break;
                            case AnimatableProperty::RotationY:
                                if (transform) transform->setRotation(
                                    glm::vec3(transform->rotation.x, writtenVal, transform->rotation.z));
                                break;
                            case AnimatableProperty::RotationZ:
                                if (transform) transform->setRotation(
                                    glm::vec3(transform->rotation.x, transform->rotation.y, writtenVal));
                                break;
                            case AnimatableProperty::ScaleX:
                                if (transform) transform->setScale(
                                    glm::vec3(writtenVal, transform->scale.y, transform->scale.z));
                                break;
                            case AnimatableProperty::ScaleY:
                                if (transform) transform->setScale(
                                    glm::vec3(transform->scale.x, writtenVal, transform->scale.z));
                                break;
                            case AnimatableProperty::ScaleZ:
                                if (transform) transform->setScale(
                                    glm::vec3(transform->scale.x, transform->scale.y, writtenVal));
                                break;
                            case AnimatableProperty::Opacity:
                                if (ml) ml->opacity = writtenVal;
                                break;
                            default: break;
                        }

                        // Properties this drag actually wrote. Normally just the
                        // dragged row — but a scale edit under the uniform lock
                        // drags the other two axes along, and each of those needs
                        // its own keyframe upsert.
                        std::vector<std::pair<AnimatableProperty, float>> written;
                        written.emplace_back(row.prop, writtenVal);

                        // Uniform-scale lock. The Properties panel has always
                        // honored this; the twirl-down used to write the dragged
                        // axis alone, silently breaking the lock.
                        if (transform && isScaleProp(row.prop) && prevAxis > 0.0001f &&
                            isScaleLocked(registry, clipEntity))
                        {
                            const float ratio = writtenVal / prevAxis;
                            glm::vec3 s = transform->scale;  // dragged axis already written
                            if (row.prop != AnimatableProperty::ScaleX) s.x *= ratio;
                            if (row.prop != AnimatableProperty::ScaleY) s.y *= ratio;
                            if (row.prop != AnimatableProperty::ScaleZ) s.z *= ratio;
                            transform->setScale(s);

                            if (row.prop != AnimatableProperty::ScaleX)
                                written.emplace_back(AnimatableProperty::ScaleX, s.x);
                            if (row.prop != AnimatableProperty::ScaleY)
                                written.emplace_back(AnimatableProperty::ScaleY, s.y);
                            if (row.prop != AnimatableProperty::ScaleZ)
                                written.emplace_back(AnimatableProperty::ScaleZ, s.z);
                        }

                        // Keyframe upsert during drag (animated non-OA channels).
                        // No interp arg: a value drag must not reset the keyframe's
                        // easing (it upserts onto an existing keyframe whenever the
                        // playhead sits on one). Only animated axes get a keyframe —
                        // a static Scale Y stays static even when the lock scales it.
                        if (localFrame >= 0 && animProps) {
                            for (const auto& [prop, val] : written) {
                                const KeyframeTrack* t = animProps->getTrack(prop);
                                if (t && t->hasKeyframes()) {
                                    animProps->addKeyframe(prop, localFrame, val);
                                }
                            }
                        }
                    }
                } else { // EffectParam
                    // Live-write the static slot while dragging.
                    if (registry.valid(row.effectEntity) && m_effectKindRegistry) {
                        auto* fx = registry.try_get<Effect>(row.effectEntity);
                        auto* ep = registry.try_get<EffectParameters>(row.effectEntity);
                        if (fx && ep) {
                            const effects::EffectKind* kind =
                                m_effectKindRegistry->find(fx->kindId);
                            if (kind) {
                                for (std::size_t pi = 0; pi < kind->params.size(); ++pi) {
                                    if (effects::fnv1a32(kind->params[pi].name) == row.paramHash
                                        && pi < ep->values.size()
                                        && ep->values[pi].type == ParamValue::Type::Float)
                                    {
                                        ep->values[pi].f4[0] = writtenVal;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // Keyframe upsert during drag (animated effect params).
                    if (hasKeyframes && localFrame >= 0 &&
                        registry.valid(row.effectEntity))
                    {
                        if (auto* eap =
                                registry.try_get<EffectAnimatedParameters>(row.effectEntity))
                        {
                            for (auto& nt : eap->tracks) {
                                if (nt.paramKeyHash == row.paramHash) {
                                    bool found = false;
                                    for (auto& kf : nt.keyframes) {
                                        if (kf.frame == localFrame) {
                                            kf.value = writtenVal; found = true; break;
                                        }
                                    }
                                    if (!found) {
                                        Keyframe kf;
                                        kf.frame = localFrame;
                                        kf.value = writtenVal;
                                        kf.interpolation = InterpolationType::Linear;
                                        nt.keyframes.push_back(kf);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // --- IsItemDeactivatedAfterEdit: enqueue undoable command ---
            if (ImGui::IsItemDeactivatedAfterEdit() && m_commandDispatcher &&
                m_twirldownPreEdit.entity == clipEntity)
            {
                const float committedVal = isOpacity ? dragVal / 100.0f : dragVal;
                auto idx = renderFindClipIndices(m_timeline, clipEntity);
                // Post-drag transform — the linked scale axes commit the values the
                // live-write already put here.
                const auto* transformForCommit = registry.try_get<Transform>(clipEntity);

                if (row.source == TimelinePropertyDef::Source::Transform) {
                    if (isOA) {
                        // OA: always commit as UpsertKeyframeCommand — there is no
                        // static value, every edit is a keyframe write. Mirrors
                        // renderOAValueRow (PropertyWindow.cpp:1493-1500). The
                        // nullopt keyframeValue signals undo to remove the kf that
                        // the drag created (rather than restoring a bogus prior value).
                        if (idx) {
                            auto cmd = std::make_unique<UpsertKeyframeCommand>(
                                idx->first, idx->second, row.prop,
                                m_twirldownPreEdit.keyframeFrame, committedVal);
                            cmd->setPreviousValue(m_twirldownPreEdit.keyframeValue);
                            m_commandDispatcher->enqueue(std::move(cmd));
                        }
                    } else if (m_twirldownPreEdit.wasKeyframed && idx) {
                        auto cmd = std::make_unique<UpsertKeyframeCommand>(
                            idx->first, idx->second, row.prop,
                            m_twirldownPreEdit.keyframeFrame, committedVal);
                        cmd->setPreviousValue(m_twirldownPreEdit.keyframeValue);
                        m_commandDispatcher->enqueue(std::move(cmd));
                    } else if (idx) {
                        // Scalar commands only exist for Opacity and Rotation.
                        // Position/Scale use live-write-only (matches PropertyWindow).
                        switch (row.prop) {
                            case AnimatableProperty::Opacity: {
                                auto cmd = std::make_unique<SetClipOpacityCommand>(
                                    idx->first, idx->second, committedVal);
                                cmd->setPreviousOpacity(m_twirldownPreEdit.scalarValue);
                                m_commandDispatcher->enqueue(std::move(cmd));
                                break;
                            }
                            case AnimatableProperty::Rotation: {
                                auto* transform = registry.try_get<Transform>(clipEntity);
                                if (transform) {
                                    auto cmd = std::make_unique<SetClipRotationCommand>(
                                        idx->first, idx->second,
                                        transform->rotation.x,
                                        transform->rotation.y,
                                        transform->rotation.z);
                                    cmd->setPreviousRotation(
                                        transform->rotation.x,
                                        transform->rotation.y,
                                        m_twirldownPreEdit.scalarValue);
                                    m_commandDispatcher->enqueue(std::move(cmd));
                                }
                                break;
                            }
                            default:
                                // PositionX/Y/Z, ScaleX/Y/Z:
                                // live-write-only (no scalar command exists, matches PropertyWindow).
                                break;
                        }
                    }

                    // Axes the uniform-scale lock dragged along. Emitted outside the
                    // chain above because the dragged axis itself may be static while
                    // a linked one is animated — that case takes the scalar branch,
                    // which has no command for scale, yet the linked keyframe was
                    // still written live and needs to be undoable.
                    if (idx && transformForCommit) {
                        for (const auto& linked : m_twirldownPreEdit.linkedScaleAxes) {
                            auto cmd = std::make_unique<UpsertKeyframeCommand>(
                                idx->first, idx->second, linked.prop,
                                m_twirldownPreEdit.keyframeFrame,
                                scaleAxis(*transformForCommit, linked.prop));
                            cmd->setPreviousValue(linked.keyframeValue);
                            m_commandDispatcher->enqueue(std::move(cmd));
                        }
                    }
                } else { // EffectParam
                    if (m_twirldownPreEdit.wasKeyframed && registry.valid(row.effectEntity)) {
                        auto cmd = std::make_unique<UpsertEffectKeyframeCommand>(
                            row.effectEntity, row.paramName,
                            m_twirldownPreEdit.keyframeFrame, committedVal);
                        cmd->setPreviousValue(m_twirldownPreEdit.keyframeValue);
                        m_commandDispatcher->enqueue(std::move(cmd));
                    } else if (registry.valid(row.effectEntity)) {
                        auto cmd = std::make_unique<SetEffectFloatParamCommand>(
                            row.effectEntity, row.paramName, committedVal);
                        cmd->setPreviousValue(m_twirldownPreEdit.scalarValue);
                        m_commandDispatcher->enqueue(std::move(cmd));
                    }
                }
                m_twirldownPreEdit.entity = entt::null;
            }

            ImGui::PopID();
            ImGui::PopID();

            // === HANDLE CLICKS ===
            if (ImGui::IsMouseClicked(0)) {
                const ImVec2 mp = ImGui::GetMousePos();

                // Stopwatch click — toggles animation on the whole property.
                // OFF (currently animated): delete every keyframe, hold the value
                // the property is showing right now. Destructive by design, AE-style;
                // one undo brings the whole track back.
                // ON: start animating by dropping a keyframe at the playhead.
                if (m_commandDispatcher && localFrame >= 0 &&
                    mp.x >= stopwatchX - stopwatchR - 4 && mp.x <= stopwatchX + stopwatchR + 4 &&
                    mp.y >= centerY - stopwatchR - 4 && mp.y <= centerY + stopwatchR + 4)
                {
                    if (row.source == TimelinePropertyDef::Source::Transform) {
                        if (auto clipIdx = renderFindClipIndices(m_timeline, clipEntity)) {
                            if (hasKeyframes) {
                                m_commandDispatcher->enqueue(
                                    std::make_unique<ClearPropertyKeyframesCommand>(
                                        clipIdx->first, clipIdx->second, row.prop,
                                        currentValue));
                            } else {
                                m_commandDispatcher->enqueue(
                                    std::make_unique<UpsertKeyframeCommand>(
                                        clipIdx->first, clipIdx->second, row.prop,
                                        localFrame, currentValue));
                            }
                        }
                    } else if (registry.valid(row.effectEntity)) {  // EffectParam
                        if (hasKeyframes) {
                            m_commandDispatcher->enqueue(
                                std::make_unique<ClearEffectParamKeyframesCommand>(
                                    row.effectEntity, row.paramName, currentValue));
                        } else {
                            m_commandDispatcher->enqueue(
                                std::make_unique<UpsertEffectKeyframeCommand>(
                                    row.effectEntity, row.paramName,
                                    localFrame, currentValue));
                        }
                    }
                }

                // Left arrow click (prev keyframe)
                if (hasKeyframes &&
                    mp.x >= leftArrowX - arrowSize - 4 && mp.x <= leftArrowX + arrowSize + 4 &&
                    mp.y >= centerY - arrowSize - 4 && mp.y <= centerY + arrowSize + 4)
                {
                    FrameNumber prevFrame = -1;
                    for (const auto& kf : kfSnapshot) {
                        if (kf.frame < localFrame && kf.frame > prevFrame) prevFrame = kf.frame;
                    }
                    if (prevFrame >= 0) m_timeline->seekToFrame(prevFrame + layerStart);
                }

                // Diamond click (add/remove keyframe). Transform path
                // uses direct AnimatedProperties writes (matches the
                // existing un-commanded UI shape). EffectParam path
                // dispatches the new undoable commands.
                if (mp.x >= diamondX - diamondSize - 4 && mp.x <= diamondX + diamondSize + 4 &&
                    mp.y >= centerY - diamondSize - 4 && mp.y <= centerY + diamondSize + 4)
                {
                    if (row.source == TimelinePropertyDef::Source::Transform) {
                        if (atKeyframe && animProps) {
                            if (auto* mt = animProps->getTrack(row.prop)) {
                                mt->removeKeyframe(localFrame);
                            }
                        } else {
                            auto& props = registry.get_or_emplace<AnimatedProperties>(clipEntity);
                            props.addKeyframe(row.prop, localFrame, currentValue,
                                              InterpolationType::Linear);
                        }
                    } else { // EffectParam
                        if (m_commandDispatcher && localFrame >= 0 &&
                            registry.valid(row.effectEntity))
                        {
                            if (atKeyframe) {
                                m_commandDispatcher->enqueue(
                                    std::make_unique<RemoveEffectKeyframeCommand>(
                                        row.effectEntity, row.paramName, localFrame));
                            } else {
                                m_commandDispatcher->enqueue(
                                    std::make_unique<UpsertEffectKeyframeCommand>(
                                        row.effectEntity, row.paramName,
                                        localFrame, currentValue));
                            }
                        }
                    }
                }

                // Right arrow click (next keyframe)
                if (hasKeyframes &&
                    mp.x >= rightArrowX - arrowSize - 4 && mp.x <= rightArrowX + arrowSize + 4 &&
                    mp.y >= centerY - arrowSize - 4 && mp.y <= centerY + arrowSize + 4)
                {
                    FrameNumber nextFrame = std::numeric_limits<FrameNumber>::max();
                    for (const auto& kf : kfSnapshot) {
                        if (kf.frame > localFrame && kf.frame < nextFrame) nextFrame = kf.frame;
                    }
                    if (nextFrame != std::numeric_limits<FrameNumber>::max()) {
                        m_timeline->seekToFrame(nextFrame + layerStart);
                    }
                }
            }

            propY += PROPERTY_ROW_HEIGHT;
        }
    }

    return expandedPropertyRowCount(clipEntity) * PROPERTY_ROW_HEIGHT;
}

// ============================================================================
// Cue lane rendering (Phase A + Phase 3)
//
// Cues live in a dedicated band ABOVE the ruler with row stacking so labels
// don't overlap. The band height is computed at the top of render() via
// computeCueLaneLayout(); this function does the actual draw using the same
// stacking pass for visual placement.
// ============================================================================

void TimelineWidget::renderCueLane(ImVec2 laneOriginPos, float laneHeight, float rulerTopY) {
    if (!m_timeline) return;
    const auto& cues = m_timeline->getCueTags();
    if (cues.empty()) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Lane background — slightly darker than the ruler so it reads as a
    // separate band, matching the ruler width via the parent child window's
    // content width (we draw beyond if needed; ImGui clip rect handles it).
    const float laneRight = laneOriginPos.x + ImGui::GetContentRegionAvail().x +
                            ImGui::GetScrollX() + ImGui::GetScrollMaxX();
    drawList->AddRectFilled(
        ImVec2(laneOriginPos.x, laneOriginPos.y),
        ImVec2(laneRight, laneOriginPos.y + laneHeight),
        IM_COL32(30, 30, 32, 255));

    constexpr float kFlagPadX = 4.0f;
    const ImU32 cueFill    = IM_COL32(240, 200,  80, 230);
    const ImU32 cueOutline = IM_COL32(255, 240, 140, 255);
    const ImU32 cueText    = IM_COL32( 30,  30,  30, 255);
    const ImU32 selFill    = IM_COL32(255, 230, 120, 255);
    const ImU32 ptrColor   = IM_COL32(200, 170,  60, 200);

    auto selectedCueOpt = m_timeline->getSelectedCueNumber();

    std::vector<CueLaneSlot> slots;
    int rowsUsed = 0;
    computeCueLaneLayout(slots, rowsUsed);

    for (const CueLaneSlot& slot : slots) {
        const CueTag& cue = cues[slot.cueIndex];

        // While dragging, render the dragged cue at its drag-current
        // position instead of its stored frame. The original slot is
        // skipped so it doesn't ghost-render in two places.
        Timecode renderT = m_timeline->frameToTime(cue.frame);
        if (m_isDraggingCue && cue.number == m_draggedCueNumber) {
            renderT = m_dragCurrentCueTime;
        }

        const float x = laneOriginPos.x + timeToPixel(renderT);
        const float flagTop = laneOriginPos.y + slot.row * kCueRowH;
        const float flagBot = flagTop + kCueRowH - 1.0f;

        char buf[320];
        if (slot.longLabel && !cue.label.empty()) {
            std::snprintf(buf, sizeof(buf), "%.2f [%s]", cue.number, cue.label.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%.2f", cue.number);
        }

        const ImU32 fill = (selectedCueOpt.has_value() && *selectedCueOpt == cue.number)
                           ? selFill : cueFill;

        // Flag body hugging the marker line on its right side.
        drawList->AddRectFilled(ImVec2(x, flagTop),
                                ImVec2(x + slot.labelW, flagBot), fill);
        drawList->AddRect(ImVec2(x, flagTop),
                          ImVec2(x + slot.labelW, flagBot), cueOutline);
        drawList->AddText(ImVec2(x + kFlagPadX, flagTop + 1.0f), cueText, buf);

        // Pointer line dropping from the cue's row down to the ruler tick.
        // Length depends on which row it landed in (deeper rows draw a
        // longer line). A small filled arrow at the bottom keeps the
        // tick origin obvious.
        drawList->AddLine(ImVec2(x, flagBot),
                          ImVec2(x, rulerTopY),
                          ptrColor, 1.0f);
        ImVec2 tri[3] = {
            ImVec2(x - 3.0f, rulerTopY - 4.0f),
            ImVec2(x + 3.0f, rulerTopY - 4.0f),
            ImVec2(x,        rulerTopY)
        };
        drawList->AddTriangleFilled(tri[0], tri[1], tri[2], fill);
        drawList->AddTriangle(tri[0], tri[1], tri[2], cueOutline);
    }
}

void TimelineWidget::renderCueModal() {
    if (!m_timeline) return;
    if (m_cueModalOpenRequested) {
        ImGui::OpenPopup("CueRulerModal");
        m_cueModalOpenRequested = false;
    }
    if (!ImGui::BeginPopupModal("CueRulerModal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text(m_cueModalMode == CueModalMode::Edit ? "Edit Cue" : "Add Cue");
    ImGui::Separator();

    entity::ui::InputDouble("Number", &m_cueModalNumber, 0.1, 1.0, "%.2f");
    ImGui::InputText("Label", m_cueModalLabelBuf, sizeof(m_cueModalLabelBuf));

    // m_cueModalFrame is an integer timeline frame — bind directly.
    long long frameLL = static_cast<long long>(m_cueModalFrame);
    if (entity::ui::InputScalar("Frame", ImGuiDataType_S64, &frameLL)) {
        if (frameLL < 0) frameLL = 0;
        m_cueModalFrame = static_cast<FrameNumber>(frameLL);
    }

    ImGui::Separator();

    if (ImGui::Button("OK", ImVec2(120, 0))) {
        if (m_commandDispatcher) {
            if (m_cueModalMode == CueModalMode::Edit) {
                auto cmd = std::make_unique<EditCueCommand>(
                    m_cueModalOldNumber, m_cueModalNumber, m_cueModalFrame,
                    std::string(m_cueModalLabelBuf));
                if (const CueTag* live = m_timeline->findCueTag(m_cueModalOldNumber)) {
                    cmd->setPreviousState(*live);
                }
                m_commandDispatcher->enqueue(std::move(cmd));
            } else {
                // "Add Section + Cue": emit section break first so undo pops
                // cue first then section (most intuitive order).
                if (m_cueModalAlsoAddSection) {
                    const std::size_t sectionCount =
                        m_timeline ? m_timeline->getSections().size() : 0;
                    const uint32_t color = sectionPalette::pickColor(sectionCount + 1);
                    m_commandDispatcher->enqueue(std::make_unique<AddSectionBreakCommand>(
                        m_cueModalFrame, color, 0.0));
                }
                m_commandDispatcher->enqueue(std::make_unique<AddCueAtCommand>(
                    m_cueModalNumber, m_cueModalFrame, std::string(m_cueModalLabelBuf)));
            }
        }
        m_cueModalAlsoAddSection = false;
        m_cueModalMode = CueModalMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        m_cueModalAlsoAddSection = false;
        m_cueModalMode = CueModalMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ============================================================================
// Section break modal (Phase B)
// ============================================================================

void TimelineWidget::renderSectionBreakModal() {
    if (!m_timeline) return;
    if (m_sectionBreakModalOpenRequested) {
        ImGui::OpenPopup("SectionBreakRulerModal");
        m_sectionBreakModalOpenRequested = false;
    }
    if (!ImGui::BeginPopupModal("SectionBreakRulerModal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const bool isEdit = (m_sectionBreakModalMode == SectionBreakModalMode::Edit);
    ImGui::Text(isEdit ? "Edit Section Break" : "Add Section Break");
    ImGui::Separator();

    // m_sectionBreakModalFrame is an integer timeline frame — bind directly.
    long long frameLL = static_cast<long long>(m_sectionBreakModalFrame);
    if (entity::ui::InputScalar("Frame", ImGuiDataType_S64, &frameLL)) {
        if (frameLL < 0) frameLL = 0;
        m_sectionBreakModalFrame = static_cast<FrameNumber>(frameLL);
    }

    // Color picker. Stored as ABGR ImU32; ImGui::ColorEdit4 wants float[4] RGBA.
    {
        ImU32 c = m_sectionBreakModalColor;
        float rgba[4] = {
            ((c >>  0) & 0xFFu) / 255.0f,
            ((c >>  8) & 0xFFu) / 255.0f,
            ((c >> 16) & 0xFFu) / 255.0f,
            ((c >> 24) & 0xFFu) / 255.0f,
        };
        if (ImGui::ColorEdit4("Color", rgba, ImGuiColorEditFlags_NoInputs)) {
            ImU32 nr = static_cast<ImU32>(rgba[0] * 255.0f) & 0xFFu;
            ImU32 ng = static_cast<ImU32>(rgba[1] * 255.0f) & 0xFFu;
            ImU32 nb = static_cast<ImU32>(rgba[2] * 255.0f) & 0xFFu;
            ImU32 na = static_cast<ImU32>(rgba[3] * 255.0f) & 0xFFu;
            m_sectionBreakModalColor = (na << 24) | (nb << 16) | (ng << 8) | nr;
        }
    }

    entity::ui::InputDouble("Fade (seconds)", &m_sectionBreakModalFadeSeconds, 0.05, 0.5, "%.2f");
    if (m_sectionBreakModalFadeSeconds < 0.0) m_sectionBreakModalFadeSeconds = 0.0;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Auto-fade duration applied to clips that start or end at this break.");
    }

    ImGui::Separator();

    if (ImGui::Button("OK", ImVec2(120, 0))) {
        if (m_commandDispatcher) {
            if (isEdit) {
                auto cmd = std::make_unique<EditSectionBreakCommand>(
                    m_sectionBreakModalOldFrame, m_sectionBreakModalFrame,
                    m_sectionBreakModalColor, m_sectionBreakModalFadeSeconds);
                if (const Timeline::Section* live =
                        m_timeline->findSectionBreakNear(m_sectionBreakModalOldFrame, 0)) {
                    cmd->setPreviousState(*live);
                }
                m_commandDispatcher->enqueue(std::move(cmd));
            } else {
                m_commandDispatcher->enqueue(std::make_unique<AddSectionBreakCommand>(
                    m_sectionBreakModalFrame,
                    m_sectionBreakModalColor, m_sectionBreakModalFadeSeconds));
            }
        }
        m_sectionBreakModalMode = SectionBreakModalMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        m_sectionBreakModalMode = SectionBreakModalMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ============================================================================
// Ripple time modal — "Insert Time Here..." / "Delete Time Here..."
// ============================================================================

void TimelineWidget::renderRippleTimeModal() {
    if (!m_timeline) return;
    if (!ImGui::BeginPopupModal("RippleTimeModal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const bool isInsert = (m_rippleTimeModalMode == RippleTimeModalMode::Insert);
    ImGui::Text(isInsert ? "Insert Time Here" : "Delete Time Here");
    ImGui::Separator();

    // Anchor frame — where the operation begins.
    long long frameLL = static_cast<long long>(m_rippleTimeModalFrame);
    if (entity::ui::InputScalar("Frame", ImGuiDataType_S64, &frameLL)) {
        if (frameLL < 0) frameLL = 0;
        m_rippleTimeModalFrame = static_cast<FrameNumber>(frameLL);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(isInsert
            ? "Timeline frame at which time is inserted."
            : "First frame of the time span to delete.");
    }

    // Duration field.
    long long durLL = static_cast<long long>(m_rippleTimeModalDuration);
    if (entity::ui::InputScalar("Duration (frames)", ImGuiDataType_S64, &durLL)) {
        if (durLL < 1) durLL = 1;
        m_rippleTimeModalDuration = static_cast<FrameNumber>(durLL);
    }

    ImGui::Separator();

    if (ImGui::Button("OK", ImVec2(120, 0))) {
        if (m_commandDispatcher && m_rippleTimeModalDuration > 0) {
            if (isInsert) {
                m_commandDispatcher->enqueue(std::make_unique<RippleInsertTimeCommand>(
                    m_rippleTimeModalFrame, m_rippleTimeModalDuration));
            } else {
                const FrameNumber rangeEnd =
                    m_rippleTimeModalFrame + m_rippleTimeModalDuration;
                m_commandDispatcher->enqueue(std::make_unique<RippleDeleteTimeCommand>(
                    m_rippleTimeModalFrame, rangeEnd));
            }
        }
        m_rippleTimeModalMode = RippleTimeModalMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        m_rippleTimeModalMode = RippleTimeModalMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace entity
