/**
 * TimelineWidget input / interaction code.
 *
 * Split from TimelineWidget.cpp (Phase B #16). All handle*() and findClip*()
 * hit-testing live here; drawing lives in TimelineWidgetRender.cpp; core
 * dispatch + geometry helpers stay in TimelineWidget.cpp.
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/timeline/CueTag.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <sstream>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>

namespace entity {

namespace {

// Locate (trackIndex, clipIndex) for an entity that lives in some
// TimelineTrack::layers vector. Mirrors the helper in PropertyWindow.cpp
// / ContentRoutingWindow.cpp — kept local because the consolidation
// is a separate cleanup.
std::optional<std::pair<int, int>>
findClipIndices(Timeline* timeline, entt::entity entity) {
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

// Read placement (start frame + duration in timeline frames) from a
// timeline-resident entity. Clip-backed entities still source truth from
// the Clip component (Layer mirror is synced via syncLayerFromClip).
// Layer-only entities (ObjectAnimation / Generative) read directly from
// Layer. Returns valid=false for entities that aren't on a track.
struct TimelinePlacement {
    FrameNumber startFrame{0};
    FrameNumber duration{0};
    bool valid{false};
};

TimelinePlacement readPlacement(entt::registry& registry, entt::entity e) {
    if (const auto* clip = registry.try_get<Clip>(e)) {
        return {clip->startFrame, clip->duration, true};
    }
    if (const auto* lay = registry.try_get<Layer>(e);
        lay && (registry.all_of<ObjectAnimationLayer>(e) ||
                registry.all_of<GenerativeLayer>(e))) {
        return {lay->startFrame, lay->duration, true};
    }
    return {};
}

// Write a new start frame to a timeline-resident entity. Clip-backed
// entities write to Clip and sync the Layer mirror; Layer-only entities
// write Layer::startFrame directly.
void writeStartFrame(entt::registry& registry, entt::entity e, FrameNumber f) {
    if (auto* clip = registry.try_get<Clip>(e)) {
        clip->startFrame = f;
        Timeline::syncLayerFromClip(registry, e);
        return;
    }
    if (auto* lay = registry.try_get<Layer>(e);
        lay && (registry.all_of<ObjectAnimationLayer>(e) ||
                registry.all_of<GenerativeLayer>(e))) {
        lay->startFrame = f;
    }
}

} // namespace

void TimelineWidget::handleInteraction() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImVec2 mousePos = ImGui::GetMousePos();
    ImGuiIO& io = ImGui::GetIO();

    // Check if mouse is over the timeline window
    bool overTimeline = (mousePos.x >= windowPos.x &&
                         mousePos.x <= windowPos.x + windowSize.x &&
                         mousePos.y >= windowPos.y);

    // Alt+scroll steps the discrete zoom ladder. Wheel up = zoom in =
    // smaller frames per major tick = lower index.
    if (overTimeline && io.MouseWheel != 0.0f && io.KeyAlt) {
        setZoomIndex(m_zoomIndex + (io.MouseWheel > 0.0f ? -1 : 1));
    }

    // Check if mouse is over the time ruler
    bool overRuler = (mousePos.x >= windowPos.x &&
                      mousePos.x <= windowPos.x + windowSize.x &&
                      mousePos.y >= windowPos.y &&
                      mousePos.y <= windowPos.y + RULER_HEIGHT);

    // Handle ruler click and drag for seeking/scrubbing
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_isDraggingRuler = true;
        float relativeX = mousePos.x - windowPos.x;
        Timecode newTime = pixelToTime(relativeX);
        m_timeline->seek(newTime);
    }

    if (m_isDraggingRuler) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Continue dragging - update time as mouse moves
            float relativeX = mousePos.x - windowPos.x;
            Timecode newTime = pixelToTime(relativeX);
            m_timeline->seek(newTime);
        } else {
            // Mouse released - stop dragging
            m_isDraggingRuler = false;
        }
    }

    // Handle clip dragging
    if (m_isDraggingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate new clip position based on mouse
            float relativeX = mousePos.x - windowPos.x - m_dragOffsetX;
            Timecode newStartTime = pixelToTime(relativeX);

            // Clamp to valid range (>= 0)
            if (newStartTime < 0) newStartTime = 0;

            // Update clip's start frame
            auto& registry = m_timeline->getRegistry();
            if (registry.valid(m_selectedClip)) {
                auto* clip = registry.try_get<Clip>(m_selectedClip);
                if (clip) {
                    float newStartSeconds = newStartTime / 1000000.0f;
                    double timelineFrameRate = m_timeline->getFrameRate();
                    clip->startFrame = static_cast<FrameNumber>(newStartSeconds * timelineFrameRate);
                }
            }
        } else {
            // Mouse released - stop dragging
            m_isDraggingClip = false;
        }
    } else if (!m_isDraggingRuler) {
        // Only check for clip selection if not dragging ruler or clip
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Start dragging this clip
                m_selectedClip = clipUnderMouse;
                m_isDraggingClip = true;

                // Calculate drag offset (where in the clip the user clicked)
                auto& registry = m_timeline->getRegistry();
                if (registry.valid(clipUnderMouse)) {
                    auto* clip = registry.try_get<Clip>(clipUnderMouse);
                    if (clip) {
                        double timelineFrameRate = m_timeline->getFrameRate();
                        float startSeconds = clip->startFrame / static_cast<float>(timelineFrameRate);
                        Timecode clipStartTime = static_cast<Timecode>(startSeconds * 1000000.0f);
                        float clipX = windowPos.x + timeToPixel(clipStartTime);
                        m_dragOffsetX = mousePos.x - clipX;
                        m_clipDragStartTime = clipStartTime;
                    }
                }
            } else {
                // Clicked on empty space - deselect
                m_selectedClip = entt::null;
            }
        }
    }
}

entt::entity TimelineWidget::findClipAtPosition(ImVec2 mousePos, ImVec2 windowPos, int& outTrackIndex) {
    if (!m_timeline) return entt::null;

    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();

    // Calculate cumulative Y position to account for expanded clips
    float cumulativeY = 0.0f;

    // Check each track
    for (size_t i = 0; i < tracks.size(); ++i) {
        entt::entity trackEntity = tracks[i];
        const auto* track = registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;

        // Calculate track Y position including offset from previous expanded tracks
        float trackY = windowPos.y + cumulativeY;

        // Calculate this track's height. Track expansion now adds the
        // property panel under the track (driven by the playhead clip), not
        // a per-clip panel — see findClipAtPlayhead().
        float trackHeight = TRACK_HEIGHT;
        bool trackExpanded = m_expandedTracks.count(static_cast<uint32_t>(trackEntity)) > 0 ||
                             m_timeline->isTrackExpanded(trackEntity);
        if (trackExpanded) {
            entt::entity atPlayhead = findClipAtPlayhead(trackEntity);
            if (atPlayhead != entt::null) {
                trackHeight = TRACK_HEIGHT +
                    expandedPropertyRowCount(atPlayhead) * PROPERTY_ROW_HEIGHT;
            }
        }

        // Check if mouse Y is within this track (including expanded area)
        if (mousePos.y >= trackY && mousePos.y <= trackY + trackHeight) {
            // Check each clip in this track. Both Clip-backed entities and
            // layer-only entities (OA / Generative) are hit-tested using
            // (startFrame, duration) in timeline frames — for layer-only
            // entities those come from the Layer component. Drag/trim code
            // downstream gates on `Clip` so layer-only entities are
            // selection-only.
            for (entt::entity clipEntity : track->layers) {
                FrameNumber startFrame = 0;
                FrameNumber durationFrames = 0;
                if (const auto* clip = registry.try_get<Clip>(clipEntity)) {
                    startFrame = clip->startFrame;
                    durationFrames = clip->duration;
                } else if (const auto* lay = registry.try_get<Layer>(clipEntity);
                           lay && (registry.all_of<ObjectAnimationLayer>(clipEntity) ||
                                   registry.all_of<GenerativeLayer>(clipEntity))) {
                    startFrame = lay->startFrame;
                    durationFrames = lay->duration;
                } else {
                    continue;
                }

                // Calculate clip bounds (use timeline frame rate, not clip source rate)
                double timelineFrameRate = m_timeline->getFrameRate();
                float startSeconds = startFrame / static_cast<float>(timelineFrameRate);
                float durationSeconds = durationFrames / static_cast<float>(timelineFrameRate);
                Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
                Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

                float clipX = windowPos.x + timeToPixel(startTime);
                float clipWidth = timeToPixel(endTime - startTime);

                // Clips report their full height (the track's expanded
                // property panel area is shared, not per-clip).
                float clipHeight = trackHeight;

                // Check if mouse is within clip bounds (including expanded area)
                if (mousePos.x >= clipX && mousePos.x <= clipX + clipWidth &&
                    mousePos.y >= trackY && mousePos.y <= trackY + clipHeight) {
                    outTrackIndex = static_cast<int>(i);
                    return clipEntity;
                }
            }
        }

        cumulativeY += trackHeight + TRACK_PADDING;
    }

    outTrackIndex = -1;
    return entt::null;
}

ClipEdge TimelineWidget::findClipEdgeAtPosition(ImVec2 mousePos, ImVec2 windowPos, entt::entity& outClip, int& outTrackIndex) {
    if (!m_timeline) {
        outClip = entt::null;
        outTrackIndex = -1;
        return ClipEdge::None;
    }

    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();

    // Check each track
    for (size_t i = 0; i < tracks.size(); ++i) {
        entt::entity trackEntity = tracks[i];
        const auto* track = registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;

        // Calculate track Y bounds
        float trackY = windowPos.y + i * (TRACK_HEIGHT + TRACK_PADDING);

        // Check if mouse Y is within this track
        if (mousePos.y < trackY || mousePos.y > trackY + TRACK_HEIGHT) {
            continue;
        }

        // Check each clip in this track
        for (entt::entity clipEntity : track->layers) {
            const auto* clip = registry.try_get<Clip>(clipEntity);
            if (!clip) continue;

            // Calculate clip bounds (use timeline frame rate, not clip source rate)
            double timelineFrameRate = m_timeline->getFrameRate();
            float startSeconds = clip->startFrame / static_cast<float>(timelineFrameRate);
            float durationSeconds = clip->duration / static_cast<float>(timelineFrameRate);
            Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
            Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

            // windowPos == m_tracksScreenPos == GetCursorScreenPos() inside the
            // scrolled child window, which ImGui already adjusts for scroll.
            // findClipAtPosition() and renderClip() both compute their X as
            // `windowPos.x + timeToPixel(startTime)` — this hit-test must
            // match. The previous `- m_syncScrollX` subtraction double-counted
            // scroll, which is what made trim handles visually drift away
            // from the clip body the further the user scrolled (worst at
            // 1f/2f zoom because that's where horizontal scroll matters
            // most). Round-3 Phase 3A fix.
            float clipX = windowPos.x + timeToPixel(startTime);
            float clipWidth = timeToPixel(endTime - startTime);
            float clipEndX = clipX + clipWidth;

            // Edge hit-test half-width: scales with clip pixel width so a
            // narrow clip still distinguishes left vs. right. Floor 3px so
            // there's always something to grab; cap at 40% of clipWidth so
            // the left-half hit zone never overlaps the right-half hit
            // zone (above 15px wide that puts each half at the original
            // 4px). Wider clips keep the original 4px half so hover
            // sensitivity matches what users learned at the default zoom.
            const float halfDefault = TRIM_EDGE_WIDTH * 0.5f;
            const float halfFloor = 3.0f;
            const float halfCap = clipWidth * 0.4f;
            float halfHit = halfDefault;
            if (halfCap < halfHit) halfHit = halfCap;
            if (halfHit < halfFloor) halfHit = halfFloor;

            // Check if mouse is near the left edge
            if (mousePos.x >= clipX - halfHit && mousePos.x <= clipX + halfHit) {
                outClip = clipEntity;
                outTrackIndex = static_cast<int>(i);
                return ClipEdge::Left;
            }

            // Check if mouse is near the right edge
            if (mousePos.x >= clipEndX - halfHit && mousePos.x <= clipEndX + halfHit) {
                outClip = clipEntity;
                outTrackIndex = static_cast<int>(i);
                return ClipEdge::Right;
            }
        }
    }

    outClip = entt::null;
    outTrackIndex = -1;
    return ClipEdge::None;
}

void TimelineWidget::handleRulerInteraction() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImVec2 mousePos = ImGui::GetMousePos();
    ImGuiIO& io = ImGui::GetIO();

    // Cue lane sits ABOVE the ruler. windowPos.y == ruler top, so the
    // lane occupies [windowPos.y - m_cueLaneHeight, windowPos.y).
    const float laneTopY = windowPos.y - m_cueLaneHeight;
    bool overCueLane = (m_cueLaneHeight > 0.0f &&
                        mousePos.x >= windowPos.x &&
                        mousePos.x <= windowPos.x + windowSize.x &&
                        mousePos.y >= laneTopY &&
                        mousePos.y <  windowPos.y);

    // Check if mouse is over the ruler window
    bool overRuler = (mousePos.x >= windowPos.x &&
                      mousePos.x <= windowPos.x + windowSize.x &&
                      mousePos.y >= windowPos.y &&
                      mousePos.y <= windowPos.y + RULER_HEIGHT);

    // Alt+scroll over the ruler (or cue lane) also steps the discrete zoom
    // ladder. Same as in handleInteraction().
    if ((overRuler || overCueLane) && io.MouseWheel != 0.0f && io.KeyAlt) {
        setZoomIndex(m_zoomIndex + (io.MouseWheel > 0.0f ? -1 : 1));
    }

    // Track the tick-division currently under the mouse on the ruler band
    // (NOT the cue lane) — renderTimeRuler() reads this to draw the
    // hover highlight.
    if (overRuler) {
        const float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
        const FrameNumber currentFrame = m_timeline->timeToFrame(pixelToTime(relativeX));
        const FrameNumber tickEvery = static_cast<FrameNumber>(framesPerTick());
        m_hoverDivisionIndex = (tickEvery > 0 && currentFrame >= 0)
            ? static_cast<int>(currentFrame / tickEvery)
            : -1;
    } else {
        m_hoverDivisionIndex = -1;
    }

    // ── Cue lane left-click: hit-test cue flags for drag-to-move. Done
    // before ruler scrub so cue drag gets first shot at the click.
    if (overCueLane && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_isDraggingCue) {
        std::vector<CueLaneSlot> slots;
        int rowsUsed = 0;
        computeCueLaneLayout(slots, rowsUsed);
        const auto& cues = m_timeline->getCueTags();
        for (const CueLaneSlot& slot : slots) {
            const CueTag& cue = cues[slot.cueIndex];
            const float cx = windowPos.x + timeToPixel(cue.timestamp);
            const float rowTop = laneTopY + slot.row * kCueRowH;
            const float rowBot = rowTop + kCueRowH;
            if (mousePos.x >= cx && mousePos.x <= cx + slot.labelW &&
                mousePos.y >= rowTop && mousePos.y <= rowBot) {
                m_isDraggingCue = true;
                m_draggedCueNumber = cue.number;
                m_dragOriginalCueTime = cue.timestamp;
                m_dragCurrentCueTime = cue.timestamp;
                break;
            }
        }
    }

    // ── Cue drag updates / release. While dragging we DON'T enqueue any
    // commands; renderCueLane() reads m_dragCurrentCueTime to draw the
    // cue at its visual position. Release commits one EditCueCommand.
    if (m_isDraggingCue) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode t = pixelToTime(relativeX);
            if (t < 0) t = 0;
            m_dragCurrentCueTime = snapTimeToBest(t);
        } else {
            // Release. Commit only if the frame actually moved.
            const FrameNumber origF = m_timeline->timeToFrame(m_dragOriginalCueTime);
            const FrameNumber newF  = m_timeline->timeToFrame(m_dragCurrentCueTime);
            if (std::llabs(static_cast<long long>(newF - origF)) > 1 && m_commandDispatcher) {
                std::string existingLabel;
                if (const CueTag* live = m_timeline->findCueTag(m_draggedCueNumber)) {
                    existingLabel = live->label;
                }
                auto cmd = std::make_unique<EditCueCommand>(
                    m_draggedCueNumber, m_draggedCueNumber,
                    m_dragCurrentCueTime, existingLabel);
                if (const CueTag* live = m_timeline->findCueTag(m_draggedCueNumber)) {
                    cmd->setPreviousState(*live);
                }
                m_commandDispatcher->enqueue(std::move(cmd));
            }
            m_isDraggingCue = false;
            m_draggedCueNumber = 0.0;
            m_dragOriginalCueTime = 0;
            m_dragCurrentCueTime = 0;
        }
        return;  // Suppress ruler scrub / range while a cue drag is active.
    }

    // Shift+click on the ruler starts a range selection. Plain click keeps
    // the existing scrub gesture and clears any prior range so the user can
    // get back to scrubbing without hunting for a clear command.
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
        Timecode clickTime = pixelToTime(relativeX);
        if (clickTime < 0) clickTime = 0;

        if (io.KeyShift) {
            m_isCreatingRange = true;
            m_rangeAnchorTime = snapTimeToTickGrid(clickTime);
            m_range.start = m_rangeAnchorTime;
            m_range.end = m_rangeAnchorTime;
            m_range.active = true;
        } else {
            m_isDraggingRuler = true;
            m_timeline->setScrubbing(true);
            // User-driven scrub snaps to the minor-tick grid at the current
            // zoom level. Sub-frame playhead positions are visually meaningless
            // and let the user accidentally land between integer frames.
            // Playback (Timeline::update) and programmatic seeks are NOT
            // snapped — only mouse-driven seeks pass through this branch.
            Timecode snapped = snapTimeToTickGrid(clickTime);
            if (snapped != m_lastSeekTime) {
                m_lastSeekTime = snapped;
                m_timeline->seek(snapped);
            }
            // Plain ruler click also clears any selection — Disguise/Resolve
            // both behave this way so the user isn't left wondering why the
            // ripple-delete shortcut is greyed out.
            m_range = {};
        }
    }

    if (m_isCreatingRange) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode mouseTime = pixelToTime(relativeX);
            if (mouseTime < 0) mouseTime = 0;
            mouseTime = snapTimeToTickGrid(mouseTime);
            m_range.start = std::min(m_rangeAnchorTime, mouseTime);
            m_range.end   = std::max(m_rangeAnchorTime, mouseTime);
        } else {
            m_isCreatingRange = false;
            // Zero-length drag = treat as just clearing the range.
            if (m_range.end <= m_range.start) m_range = {};
        }
    } else if (m_isDraggingRuler) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode snapped = snapTimeToTickGrid(pixelToTime(relativeX));
            if (snapped != m_lastSeekTime) {
                m_lastSeekTime = snapped;
                m_timeline->seek(snapped);
            }
        } else {
            m_isDraggingRuler = false;
            m_timeline->setScrubbing(false);
            m_lastSeekTime = -1;
        }
    }

    // Right-click is now disambiguated by Y-band:
    //   - cue lane (above ruler) -> cue ops (edit/delete an existing flag,
    //                                        or "Add Cue Here..." on empty lane)
    //   - ruler (below cue lane) -> section break ops + range ops
    // This kills the prior cue-vs-break right-click ambiguity that lived
    // when both shared the ruler band.
    if (overCueLane && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
        Timecode clickTime = pixelToTime(relativeX);
        if (clickTime < 0) clickTime = 0;
        m_rulerRightClickTime = clickTime;

        // Lane-row hit test against rendered cue label rects.
        m_rightClickedCueIndex = -1;
        std::vector<CueLaneSlot> slots;
        int rowsUsed = 0;
        computeCueLaneLayout(slots, rowsUsed);
        const auto& cues = m_timeline->getCueTags();
        for (const CueLaneSlot& slot : slots) {
            const CueTag& cue = cues[slot.cueIndex];
            const float cx = windowPos.x + timeToPixel(cue.timestamp);
            const float rowTop = laneTopY + slot.row * kCueRowH;
            const float rowBot = rowTop + kCueRowH;
            if (mousePos.x >= cx && mousePos.x <= cx + slot.labelW &&
                mousePos.y >= rowTop && mousePos.y <= rowBot) {
                m_rightClickedCueIndex = static_cast<int>(slot.cueIndex);
                break;
            }
        }

        if (m_rightClickedCueIndex >= 0) {
            ImGui::OpenPopup("CueContextMenu");
        } else {
            // Empty lane area -> directly offer Add Cue Here.
            ImGui::OpenPopup("CueLaneContextMenu");
        }
    } else if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
        Timecode clickTime = pixelToTime(relativeX);
        if (clickTime < 0) clickTime = 0;
        m_rulerRightClickTime = clickTime;

        // Section break hit-test (Phase B). Hit-tests against the rendered
        // break line at ±6px. Selects the captured break frame for the
        // popup; vector indices shift on edit, so we key by Timecode.
        m_rightClickedSectionBreakFrame.reset();
        const float kBreakHitPx = 6.0f;
        const auto& sections = m_timeline->getSections();
        for (const auto& sec : sections) {
            const float bx = windowPos.x + timeToPixel(sec.breakFrame);
            if (std::abs(mousePos.x - bx) <= kBreakHitPx) {
                m_rightClickedSectionBreakFrame = sec.breakFrame;
                break;
            }
        }

        if (m_rightClickedSectionBreakFrame.has_value()) {
            ImGui::OpenPopup("SectionBreakContextMenu");
        } else if (m_range.active && m_range.end > m_range.start) {
            // Range active -> offer endpoint break creation.
            m_rangeContextMenuRequested = true;
            ImGui::OpenPopup("RangeContextMenu");
        } else {
            // Empty ruler -> Add Section Break Here (cue ops moved to the
            // cue lane band above).
            ImGui::OpenPopup("RulerContextMenu");
        }
    }

    // "Create Section Breaks from Range" popup. Emits two breaks (start + end)
    // — the closest analogue of the old region-style "Create Section from
    // Selection" under the break-point model.
    if (ImGui::BeginPopup("RangeContextMenu")) {
        if (m_range.active && m_range.end > m_range.start) {
            if (ImGui::MenuItem("Create Section Breaks at Range Endpoints") && m_commandDispatcher) {
                m_commandDispatcher->enqueue(std::make_unique<AddSectionBreakCommand>(
                    m_range.start, 0xFF6090C8u, 0.0));
                m_commandDispatcher->enqueue(std::make_unique<AddSectionBreakCommand>(
                    m_range.end, 0xFF6090C8u, 0.0));
            }
            ImGui::Separator();
            FrameNumber inF = m_timeline->timeToFrame(m_range.start);
            FrameNumber outF = m_timeline->timeToFrame(m_range.end);
            ImGui::TextDisabled("Range: F%lld - F%lld (%lldf)",
                static_cast<long long>(inF),
                static_cast<long long>(outF),
                static_cast<long long>(outF - inF));
        }
        ImGui::EndPopup();
    }

    // Section break right-click: edit / delete / jump.
    if (ImGui::BeginPopup("SectionBreakContextMenu")) {
        if (m_rightClickedSectionBreakFrame.has_value()) {
            const Timecode brkT = *m_rightClickedSectionBreakFrame;
            const Timeline::Section* live = m_timeline->findSectionBreakNear(brkT, 0);
            if (live) {
                char hdr[64];
                std::snprintf(hdr, sizeof(hdr), "Break @ F%lld",
                              static_cast<long long>(m_timeline->timeToFrame(live->breakFrame)));
                ImGui::TextDisabled("%s", hdr);
                ImGui::Separator();
                if (ImGui::MenuItem("Jump to Break") && m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<SeekCommand>(brkT));
                }
                if (ImGui::MenuItem("Edit Break...")) {
                    m_sectionBreakModalMode = SectionBreakModalMode::Edit;
                    m_sectionBreakModalOldFrame = brkT;
                    m_sectionBreakModalFrame = brkT;
                    m_sectionBreakModalColor = live->color;
                    m_sectionBreakModalFadeSeconds = live->fadeSeconds;
                    m_sectionBreakModalOpenRequested = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Break") && m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<RemoveSectionBreakCommand>(brkT));
                    m_rightClickedSectionBreakFrame.reset();
                }
            }
        }
        ImGui::EndPopup();
    }

    // Cue flag right-click: fire / edit / delete. Index captured at the
    // click site stays valid for one popup lifetime (no concurrent mutation).
    if (ImGui::BeginPopup("CueContextMenu")) {
        const auto& cues = m_timeline->getCueTags();
        if (m_rightClickedCueIndex >= 0 &&
            m_rightClickedCueIndex < static_cast<int>(cues.size())) {
            const auto& cue = cues[m_rightClickedCueIndex];
            char hdr[48];
            std::snprintf(hdr, sizeof(hdr), "Cue %.2f", cue.number);
            ImGui::TextDisabled("%s", hdr);
            if (!cue.label.empty()) {
                ImGui::TextDisabled("%s", cue.label.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Fire Cue") && m_commandDispatcher) {
                m_commandDispatcher->enqueue(std::make_unique<FireCueCommand>(cue.number));
            }
            if (ImGui::MenuItem("Edit Cue...")) {
                m_cueModalMode = CueModalMode::Edit;
                m_cueModalOldNumber = cue.number;
                m_cueModalNumber = cue.number;
                m_cueModalTimestamp = cue.timestamp;
                std::strncpy(m_cueModalLabelBuf, cue.label.c_str(),
                             sizeof(m_cueModalLabelBuf) - 1);
                m_cueModalLabelBuf[sizeof(m_cueModalLabelBuf) - 1] = '\0';
                m_cueModalOpenRequested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Cue") && m_commandDispatcher) {
                m_commandDispatcher->enqueue(std::make_unique<RemoveCueCommand>(cue.number));
                m_rightClickedCueIndex = -1;
            }
        }
        ImGui::EndPopup();
    }

    // Empty-ruler right-click: section break ops + cue add (Round-3 Phase 2 #2).
    // The ADD path is now dialog-less — auto-color from the palette, fade=0;
    // EDIT (right-click an existing break line) still opens the modal so the
    // user can override color + fade.
    if (ImGui::BeginPopup("RulerContextMenu")) {
        if (ImGui::MenuItem("Add Section Break Here") && m_commandDispatcher) {
            // Grid-only snap: snapTimeToBest would prefer an existing nearby
            // section break, producing a near-duplicate.
            const Timecode at = snapTimeToTickGrid(m_rulerRightClickTime);
            // Auto-color: index = current section count + 1, so the first
            // user-added break gets palette[1] (palette[0] is reserved for
            // the implicit first segment).
            const std::size_t sectionCount =
                m_timeline ? m_timeline->getSections().size() : 0;
            const uint32_t color = sectionPalette::pickColor(sectionCount + 1);
            m_commandDispatcher->enqueue(
                std::make_unique<AddSectionBreakCommand>(at, color, 0.0));
        }
        if (ImGui::MenuItem("Add Cue Here...")) {
            // Cue ADD keeps its modal — the user still needs to set the
            // cue number + label. Mirrors the CueLaneContextMenu shortcut
            // so the operator doesn't have to chase the cue-lane band.
            m_cueModalMode = CueModalMode::Add;
            const auto& cues = m_timeline->getCueTags();
            m_cueModalOldNumber = 0.0;
            m_cueModalNumber = cues.empty() ? 1.0 : (cues.back().number + 1.0);
            m_cueModalTimestamp = snapTimeToTickGrid(m_rulerRightClickTime);
            m_cueModalLabelBuf[0] = '\0';
            m_cueModalOpenRequested = true;
        }
        ImGui::EndPopup();
    }

    // Empty cue-lane right-click: offer Add Cue Here. Direct shortcut since
    // the lane has no other actions on empty space.
    if (ImGui::BeginPopup("CueLaneContextMenu")) {
        if (ImGui::MenuItem("Add Cue Here...")) {
            m_cueModalMode = CueModalMode::Add;
            const auto& cues = m_timeline->getCueTags();
            m_cueModalOldNumber = 0.0;
            m_cueModalNumber = cues.empty() ? 1.0 : (cues.back().number + 1.0);
            // Grid-only snap: snapTimeToBest would prefer an existing nearby
            // cue, producing a near-duplicate.
            m_cueModalTimestamp = snapTimeToTickGrid(m_rulerRightClickTime);
            m_cueModalLabelBuf[0] = '\0';
            m_cueModalOpenRequested = true;
        }
        ImGui::EndPopup();
    }

    // Render the cue + section-break modals (no-op when not open).
    renderCueModal();
    renderSectionBreakModal();
}

void TimelineWidget::handleTracksInteraction() {
    if (!m_timeline) return;

    // Use cached tracks position (saved in render() before cursor is moved by widgets)
    ImVec2 windowPos = m_tracksScreenPos;
    ImVec2 mousePos = ImGui::GetMousePos();

    // Only handle new interactions if this child window is hovered
    bool isWindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    auto& registry = m_timeline->getRegistry();

    // Handle clip trimming (priority over dragging)
    if (m_isTrimmingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate mouse position in timeline time
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode mouseTime = pixelToTime(relativeX);
            if (mouseTime < 0) mouseTime = 0;
            // Round-3 Phase 3B — shift held bypasses grid+cue+section snap so
            // the user can land a frame-precise trim. Default behavior keeps
            // snapTimeToBest (grid + nearest cue + nearest section break).
            if (!ImGui::GetIO().KeyShift) {
                mouseTime = snapTimeToBest(mouseTime);
            }

            // Get the clip being trimmed
            if (registry.valid(m_trimClip)) {
                auto* clip = registry.try_get<Clip>(m_trimClip);
                if (clip) {
                    double timelineFrameRate = m_timeline->getFrameRate();
                    FrameNumber mouseFrame = static_cast<FrameNumber>(mouseTime / 1000000.0f * timelineFrameRate);

                    // Find the track this clip is on for collision detection
                    int trimTrackIndex = -1;
                    const auto& tracks = m_timeline->getTracks();
                    for (size_t i = 0; i < tracks.size(); ++i) {
                        const auto* track = registry.try_get<TimelineTrack>(tracks[i]);
                        if (track) {
                            for (entt::entity clipEnt : track->layers) {
                                if (clipEnt == m_trimClip) {
                                    trimTrackIndex = static_cast<int>(i);
                                    break;
                                }
                            }
                        }
                        if (trimTrackIndex >= 0) break;
                    }

                    if (m_trimEdge == ClipEdge::Left) {
                        // Trim left edge - adjust start and duration
                        // Don't allow trimming past original end
                        FrameNumber originalEnd = m_trimOriginalStart + m_trimOriginalDuration;
                        if (mouseFrame < originalEnd - 1) {  // Keep at least 1 frame
                            // Don't allow trimming before 0
                            if (mouseFrame < 0) mouseFrame = 0;

                            // Check for collision with previous clip
                            FrameNumber minStartFrame = 0;
                            if (trimTrackIndex >= 0 && trimTrackIndex < static_cast<int>(tracks.size())) {
                                const auto* track = registry.try_get<TimelineTrack>(tracks[trimTrackIndex]);
                                if (track) {
                                    for (entt::entity otherClip : track->layers) {
                                        if (otherClip == m_trimClip) continue;
                                        const auto* other = registry.try_get<Clip>(otherClip);
                                        if (other) {
                                            FrameNumber otherEnd = other->startFrame + other->duration;
                                            // If other clip ends before our original start, it could limit us
                                            if (otherEnd <= m_trimOriginalStart && otherEnd > minStartFrame) {
                                                minStartFrame = otherEnd;
                                            }
                                        }
                                    }
                                }
                            }

                            // Clamp to minimum (collision boundary)
                            if (mouseFrame < minStartFrame) {
                                mouseFrame = minStartFrame;
                            }

                            FrameNumber framesDelta = mouseFrame - m_trimOriginalStart;
                            clip->startFrame = mouseFrame;
                            clip->duration = m_trimOriginalDuration - framesDelta;
                            Timeline::syncLayerFromClip(registry, m_trimClip);

                            // Adjust media start to keep sync
                            clip->mediaStartFrame = m_trimOriginalMediaStart + framesDelta;
                        }
                    } else if (m_trimEdge == ClipEdge::Right) {
                        // Trim right edge - adjust duration only
                        FrameNumber newDuration = mouseFrame - clip->startFrame;
                        if (newDuration >= 1) {  // Keep at least 1 frame
                            // Check for collision with next clip
                            FrameNumber maxEndFrame = std::numeric_limits<FrameNumber>::max();
                            if (trimTrackIndex >= 0 && trimTrackIndex < static_cast<int>(tracks.size())) {
                                const auto* track = registry.try_get<TimelineTrack>(tracks[trimTrackIndex]);
                                if (track) {
                                    for (entt::entity otherClip : track->layers) {
                                        if (otherClip == m_trimClip) continue;
                                        const auto* other = registry.try_get<Clip>(otherClip);
                                        if (other) {
                                            // If other clip starts after our start, it could limit us
                                            if (other->startFrame > clip->startFrame && other->startFrame < maxEndFrame) {
                                                maxEndFrame = other->startFrame;
                                            }
                                        }
                                    }
                                }
                            }

                            // Clamp duration to not exceed collision boundary
                            FrameNumber maxDuration = maxEndFrame - clip->startFrame;
                            if (newDuration > maxDuration) {
                                newDuration = maxDuration;
                            }

                            clip->duration = newDuration;
                            Timeline::syncLayerFromClip(registry, m_trimClip);
                        }
                    }
                }
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else {
            // Mouse released - stop trimming
            m_isTrimmingClip = false;
            m_trimEdge = ClipEdge::None;
            m_trimClip = entt::null;
            m_timeline->setScrubbing(false);  // Exit scrubbing mode - triggers final seek
        }
        return;  // Don't process other interactions while trimming
    }

    // Handle clip dragging (continue even if not hovered, to allow drag outside window)
    if (m_isDraggingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate desired new clip position based on mouse
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX - m_dragOffsetX;
            Timecode desiredStartTime = pixelToTime(relativeX);

            // Clamp to valid range (>= 0)
            if (desiredStartTime < 0) desiredStartTime = 0;

            // Snap to playhead and other clips if enabled
            m_isSnapping = false;  // Reset snap state
            TimelinePlacement movingPlace = registry.valid(m_selectedClip)
                ? readPlacement(registry, m_selectedClip)
                : TimelinePlacement{};
            if (m_snappingEnabled && movingPlace.valid) {
                {
                    Timecode playheadTime = m_timeline->getCurrentTime();
                    // Use timeline frame rate since duration is in timeline frames
                    double timelineFrameRate = m_timeline->getFrameRate();
                    float durationSeconds = movingPlace.duration / static_cast<float>(timelineFrameRate);
                    Timecode clipDuration = static_cast<Timecode>(durationSeconds * 1000000.0f);
                    Timecode desiredEndTime = desiredStartTime + clipDuration;

                    // Convert snap threshold from pixels to time
                    Timecode snapThresholdTime = pixelToTime(SNAP_THRESHOLD_PIXELS);

                    // Round-3 Phase 3B — shift modifier bypasses tick-grid
                    // snap only. Without it, a raw playhead / clip-edge
                    // candidate sitting closer than the nearest tick wins
                    // and the user observes 1-frame snaps even at 5f/10f
                    // tick zoom. With grid-snap on, every candidate is
                    // pre-floored to the tick grid so they compete at
                    // grid resolution. Playhead + clip-edge snap still
                    // apply with shift held — only the implicit grid
                    // floor is dropped.
                    const bool gridSnap = !ImGui::GetIO().KeyShift;
                    auto floorToGrid = [&](Timecode raw) -> Timecode {
                        return gridSnap ? snapTimeToTickGrid(raw) : raw;
                    };

                    // Track best snap candidate
                    Timecode bestSnapTime = 0;
                    Timecode bestSnapDistance = snapThresholdTime + 1;  // Start with invalid distance
                    bool snapToStart = true;  // Whether to snap clip start (true) or end (false)

                    // Unified candidate ranker. `targetForStart` chooses
                    // whether the candidate is being matched against the
                    // dragged clip's start edge (true) or end edge (false).
                    // `candidate` is the snap target time; the lambda
                    // grid-floors it and computes the cursor->candidate
                    // distance against the appropriate edge.
                    auto considerCandidate = [&](Timecode raw, bool targetForStart) {
                        const Timecode candidate = floorToGrid(raw);
                        const Timecode desired = targetForStart ? desiredStartTime : desiredEndTime;
                        const Timecode dist = std::abs(desired - candidate);
                        if (dist < bestSnapDistance) {
                            bestSnapDistance = dist;
                            bestSnapTime = candidate;
                            snapToStart = targetForStart;
                        }
                    };

                    // Playhead — both edges.
                    considerCandidate(playheadTime, true);
                    considerCandidate(playheadTime, false);

                    // Grid + cues + sections (snapTimeToBest already returns
                    // a snapped value; floorToGrid is a no-op for the grid
                    // case but rounds cue/section snaps to the tick grid
                    // when grid-snap is on, otherwise leaves them at raw).
                    considerCandidate(snapTimeToBest(desiredStartTime), true);
                    considerCandidate(snapTimeToBest(desiredEndTime),   false);

                    // Other clips on the same track — their edges, both
                    // matched against our start and our end. Both Clip-backed
                    // and Layer-only (OA / Generative) entities contribute
                    // snap targets, read uniformly via readPlacement().
                    if (m_selectedClipTrackIndex >= 0) {
                        const auto& tracks = m_timeline->getTracks();
                        if (m_selectedClipTrackIndex < static_cast<int>(tracks.size())) {
                            const auto* track = registry.try_get<TimelineTrack>(tracks[m_selectedClipTrackIndex]);
                            if (track) {
                                for (entt::entity otherClipEntity : track->layers) {
                                    if (otherClipEntity == m_selectedClip) continue;
                                    TimelinePlacement otherPlace = readPlacement(registry, otherClipEntity);
                                    if (!otherPlace.valid) continue;

                                    float otherStartSec = otherPlace.startFrame / static_cast<float>(timelineFrameRate);
                                    float otherDurSec   = otherPlace.duration   / static_cast<float>(timelineFrameRate);
                                    Timecode otherStartTime = static_cast<Timecode>(otherStartSec * 1000000.0f);
                                    Timecode otherEndTime = static_cast<Timecode>((otherStartSec + otherDurSec) * 1000000.0f);

                                    considerCandidate(otherStartTime, true);   // our start ↔ other's start
                                    considerCandidate(otherEndTime,   true);   // our start ↔ other's end
                                    considerCandidate(otherStartTime, false);  // our end   ↔ other's start
                                    considerCandidate(otherEndTime,   false);  // our end   ↔ other's end
                                }
                            }
                        }
                    }

                    // Apply best snap if found
                    if (bestSnapDistance < snapThresholdTime) {
                        if (snapToStart) {
                            desiredStartTime = bestSnapTime;
                        } else {
                            desiredStartTime = bestSnapTime - clipDuration;
                        }
                        if (desiredStartTime < 0) desiredStartTime = 0;
                        m_isSnapping = true;
                        m_snapTargetTime = bestSnapTime;
                    }
                }
            }

            // Check for collisions and snap to valid position
            Timecode newStartTime = checkClipCollision(m_selectedClip, desiredStartTime, m_selectedClipTrackIndex);

            // Update placement (start frame). Routes to Clip or Layer-only
            // storage depending on the entity's archetype.
            if (registry.valid(m_selectedClip)) {
                float newStartSeconds = newStartTime / 1000000.0f;
                double timelineFrameRate = m_timeline->getFrameRate();
                FrameNumber newStartFrame = static_cast<FrameNumber>(newStartSeconds * timelineFrameRate);
                writeStartFrame(registry, m_selectedClip, newStartFrame);
            }
        } else {
            // Mouse released - stop dragging
            // Check if we need to move the clip to a different track
            int finalTrackIndex = findTrackAtY(mousePos.y, windowPos.y);
            if (finalTrackIndex >= 0 && finalTrackIndex != m_selectedClipTrackIndex) {
                // Move clip to new track
                m_timeline->moveClipToTrack(m_selectedClip, finalTrackIndex);
                m_selectedClipTrackIndex = finalTrackIndex;
            }

            m_isDraggingClip = false;
            m_selectedClipTrackIndex = -1;
            m_isSnapping = false;  // Clear snap indicator
            m_timeline->setScrubbing(false);  // Exit scrubbing mode - triggers final seek
        }
    } else if (!m_isDraggingRuler && isWindowHovered) {
        // Check for clip edge hover (for trimming cursor)
        entt::entity edgeClip = entt::null;
        int edgeTrackIndex = -1;
        ClipEdge edge = findClipEdgeAtPosition(mousePos, windowPos, edgeClip, edgeTrackIndex);

        if (edge != ClipEdge::None) {
            // Show resize cursor when hovering over clip edge
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            // Start trimming on click
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                m_isTrimmingClip = true;
                m_trimEdge = edge;
                m_trimClip = edgeClip;
                m_selectedClip = edgeClip;
                m_timeline->setSelectedClip(edgeClip);
                m_timeline->setSelectedScreen(entt::null);  // Deselect screen when selecting clip
                m_timeline->setScrubbing(true);  // Enter scrubbing mode - prevents decoder seeks

                // Store original clip state for trim calculations
                auto* clip = registry.try_get<Clip>(edgeClip);
                if (clip) {
                    m_trimOriginalStart = clip->startFrame;
                    m_trimOriginalDuration = clip->duration;
                    m_trimOriginalMediaStart = clip->mediaStartFrame;
                }
                return;
            }
        }

        // Only check for clip selection if not hovering edge
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Start dragging this clip
                m_selectedClip = clipUnderMouse;
                m_isDraggingClip = true;
                m_selectedClipTrackIndex = trackIndex;
                m_timeline->setScrubbing(true);  // Enter scrubbing mode - prevents decoder seeks

                // Sync selection to Timeline for PropertyWindow
                m_timeline->setSelectedClip(clipUnderMouse);
                m_timeline->setSelectedScreen(entt::null);  // Deselect screen when selecting clip

                // Calculate drag offset (where in the clip the user clicked).
                // Reads placement uniformly so OA/Generative Layer-only
                // entities pick up a correct offset (without this, the offset
                // stays at its previous value and the layer doesn't track).
                if (registry.valid(clipUnderMouse)) {
                    TimelinePlacement place = readPlacement(registry, clipUnderMouse);
                    if (place.valid) {
                        double timelineFrameRate = m_timeline->getFrameRate();
                        float startSeconds = place.startFrame / static_cast<float>(timelineFrameRate);
                        Timecode clipStartTime = static_cast<Timecode>(startSeconds * 1000000.0f);
                        float clipX = windowPos.x + timeToPixel(clipStartTime) - m_syncScrollX;
                        m_dragOffsetX = mousePos.x - clipX;
                        m_clipDragStartTime = clipStartTime;
                    }
                }
            } else {
                // Clicked on empty space within timeline - deselect
                m_selectedClip = entt::null;
                m_selectedClipTrackIndex = -1;

                // Sync deselection to Timeline
                m_timeline->setSelectedClip(entt::null);
            }
        }

        // Handle right-click for context menus (only if window is hovered)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Right-clicked on a clip
                m_rightClickedClip = clipUnderMouse;
                m_rightClickedTrackIndex = trackIndex;
                m_showClipContextMenu = true;
                m_showTrackContextMenu = false;
                ImGui::OpenPopup("ClipContextMenu");
            } else {
                // Check if right-clicked on a track (empty area)
                int trackAtY = findTrackAtY(mousePos.y, windowPos.y);
                if (trackAtY >= 0) {
                    m_rightClickedTrackIndex = trackAtY;
                    m_rightClickedClip = entt::null;
                    m_showTrackContextMenu = true;
                    m_showClipContextMenu = false;
                    ImGui::OpenPopup("TrackContextMenu");
                }
            }
        }
    }
}

void TimelineWidget::handleContextMenus() {
    if (!m_timeline) return;

    // Clip context menu
    if (ImGui::BeginPopup("ClipContextMenu")) {
        if (m_rightClickedClip != entt::null) {
            if (ImGui::MenuItem("Delete Clip")) {
                // Clear selection if we're deleting the selected clip
                if (m_selectedClip == m_rightClickedClip) {
                    m_selectedClip = entt::null;
                    m_selectedClipTrackIndex = -1;
                }
                // Route through the command dispatcher so the delete is
                // undoable (Ctrl+Z restores). Falls back to the direct path
                // only if no dispatcher is wired (shouldn't happen at runtime
                // but keeps unit tests that use TimelineWidget standalone OK).
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<DeleteClipCommand>(
                        static_cast<uint32_t>(m_rightClickedClip)));
                } else {
                    m_timeline->deleteClip(m_rightClickedClip);
                }
                m_rightClickedClip = entt::null;
            }
            ImGui::Separator();

            // Get clip info for display
            auto& registry = m_timeline->getRegistry();
            const auto* clip = registry.try_get<Clip>(m_rightClickedClip);
            if (clip) {
                ImGui::TextDisabled("Duration: %d frames", clip->duration);
                ImGui::TextDisabled("Start: frame %d", clip->startFrame);
            }
        }
        ImGui::EndPopup();
    }

    // Track context menu
    if (ImGui::BeginPopup("TrackContextMenu")) {
        if (m_rightClickedTrackIndex >= 0) {
            std::ostringstream label;
            label << "Delete Track " << (m_rightClickedTrackIndex + 1);
            if (ImGui::MenuItem(label.str().c_str())) {
                const auto& tracks = m_timeline->getTracks();
                if (m_rightClickedTrackIndex < static_cast<int>(tracks.size())) {
                    entt::entity trackToDelete = tracks[m_rightClickedTrackIndex];
                    m_timeline->deleteTrack(trackToDelete);
                }
                m_rightClickedTrackIndex = -1;
            }

            ImGui::Separator();

            // Option to add a new track
            if (ImGui::MenuItem("Add Track Above")) {
                // For now, just add at the end (track ordering would need more work)
                std::ostringstream trackName;
                trackName << "Video Track " << (m_timeline->getTrackCount() + 1);
                m_timeline->createTrack(trackName.str());
            }

            ImGui::Separator();
            ImGui::TextDisabled("Track %d", m_rightClickedTrackIndex + 1);
        }
        ImGui::EndPopup();
    }

    // Keyframe context menu (for easing options)
    if (m_showKeyframeContextMenu) {
        ImGui::OpenPopup("KeyframeContextMenu");
        m_showKeyframeContextMenu = false;
    }

    if (ImGui::BeginPopup("KeyframeContextMenu")) {
        if (m_keyframeEditClip != entt::null) {
            auto& registry = m_timeline->getRegistry();
            auto* animProps = registry.try_get<AnimatedProperties>(m_keyframeEditClip);

            if (animProps) {
                auto* track = animProps->getTrack(m_keyframeEditProperty);
                if (track) {
                    Keyframe* kf = track->getKeyframeAt(m_keyframeEditFrame);
                    if (kf) {
                        auto idx = findClipIndices(m_timeline, m_keyframeEditClip);

                        // Emit an undoable command for any interpolation
                        // change. The fallback direct-write path runs when
                        // there's no dispatcher (script-driven embedding,
                        // tests) or no clip index — preserves prior
                        // behavior so nothing regresses.
                        auto setInterp = [&](InterpolationType newInterp) {
                            if (m_commandDispatcher && idx) {
                                auto cmd = std::make_unique<SetKeyframeInterpolationCommand>(
                                    idx->first, idx->second,
                                    m_keyframeEditProperty, m_keyframeEditFrame,
                                    newInterp, kf->easeIn, kf->easeOut);
                                cmd->setPreviousState(kf->interpolation, kf->easeIn, kf->easeOut);
                                m_commandDispatcher->enqueue(std::move(cmd));
                            } else {
                                kf->interpolation = newInterp;
                            }
                        };

                        ImGui::TextDisabled("Interpolation:");

                        bool isLinear    = (kf->interpolation == InterpolationType::Linear);
                        bool isStep      = (kf->interpolation == InterpolationType::Step);
                        bool isEaseIn    = (kf->interpolation == InterpolationType::EaseIn);
                        bool isEaseOut   = (kf->interpolation == InterpolationType::EaseOut);
                        bool isEaseInOut = (kf->interpolation == InterpolationType::EaseInOut);

                        if (ImGui::MenuItem("Linear (Diamond)", nullptr, isLinear)) {
                            setInterp(InterpolationType::Linear);
                        }
                        if (ImGui::MenuItem("Hold (Square)", nullptr, isStep)) {
                            setInterp(InterpolationType::Step);
                        }
                        if (ImGui::MenuItem("Ease In", nullptr, isEaseIn)) {
                            setInterp(InterpolationType::EaseIn);
                        }
                        if (ImGui::MenuItem("Ease Out", nullptr, isEaseOut)) {
                            setInterp(InterpolationType::EaseOut);
                        }
                        if (ImGui::MenuItem("Ease In/Out (Hourglass)", nullptr, isEaseInOut)) {
                            setInterp(InterpolationType::EaseInOut);
                        }

                        // Bezier-tangent UI for the Ease In/Out type. Two
                        // DragFloats expose the cubic-bezier control points
                        // (P1.x = easeIn, P2.x = easeOut); presets cover
                        // the common cases. Drag-end commits one
                        // SetKeyframeInterpolationCommand so the whole
                        // gesture undoes as a single step.
                        if (isEaseInOut) {
                            ImGui::Separator();
                            ImGui::TextDisabled("Bezier tangents:");

                            float easeIn  = kf->easeIn;
                            float easeOut = kf->easeOut;

                            auto captureBezierPreEdit = [&]() {
                                if (ImGui::IsItemActivated() && !m_kfEasingPreEdit.valid) {
                                    m_kfEasingPreEdit.valid   = true;
                                    m_kfEasingPreEdit.interp  = kf->interpolation;
                                    m_kfEasingPreEdit.easeIn  = kf->easeIn;
                                    m_kfEasingPreEdit.easeOut = kf->easeOut;
                                }
                            };
                            auto commitBezierEdit = [&]() {
                                if (ImGui::IsItemDeactivatedAfterEdit() && m_kfEasingPreEdit.valid) {
                                    if (m_commandDispatcher && idx) {
                                        auto cmd = std::make_unique<SetKeyframeInterpolationCommand>(
                                            idx->first, idx->second,
                                            m_keyframeEditProperty, m_keyframeEditFrame,
                                            kf->interpolation, kf->easeIn, kf->easeOut);
                                        cmd->setPreviousState(
                                            m_kfEasingPreEdit.interp,
                                            m_kfEasingPreEdit.easeIn,
                                            m_kfEasingPreEdit.easeOut);
                                        m_commandDispatcher->enqueue(std::move(cmd));
                                    }
                                    m_kfEasingPreEdit.valid = false;
                                }
                            };

                            ImGui::SetNextItemWidth(140.0f);
                            if (ImGui::DragFloat("##kfEaseIn", &easeIn, 0.005f, 0.0f, 1.0f, "in %.2f")) {
                                kf->easeIn = easeIn;
                            }
                            captureBezierPreEdit();
                            commitBezierEdit();

                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(140.0f);
                            if (ImGui::DragFloat("##kfEaseOut", &easeOut, 0.005f, 0.0f, 1.0f, "out %.2f")) {
                                kf->easeOut = easeOut;
                            }
                            captureBezierPreEdit();
                            commitBezierEdit();

                            // Preset rows — drop a single command per click
                            // so each preset is its own undo step. Values
                            // mirror common easing curves: Smooth = default
                            // AE Easy Ease (~33%), Snappy = aggressive lead
                            // in / late out, Linear = no curve (matches
                            // Linear interp but keeps EaseInOut math active
                            // in case the user wants to nudge from there).
                            struct Preset { const char* name; float in; float out; };
                            const Preset presets[] = {
                                {"Smooth",  0.42f, 0.58f},
                                {"Snappy",  0.25f, 0.75f},
                                {"Linear",  0.50f, 0.50f},
                            };
                            for (const auto& p : presets) {
                                if (ImGui::SmallButton(p.name)) {
                                    if (m_commandDispatcher && idx) {
                                        auto cmd = std::make_unique<SetKeyframeInterpolationCommand>(
                                            idx->first, idx->second,
                                            m_keyframeEditProperty, m_keyframeEditFrame,
                                            kf->interpolation, p.in, p.out);
                                        cmd->setPreviousState(kf->interpolation, kf->easeIn, kf->easeOut);
                                        m_commandDispatcher->enqueue(std::move(cmd));
                                    } else {
                                        kf->easeIn  = p.in;
                                        kf->easeOut = p.out;
                                    }
                                }
                                ImGui::SameLine();
                            }
                            ImGui::NewLine();
                        }

                        ImGui::Separator();

                        if (ImGui::MenuItem("Delete Keyframe")) {
                            // Delete remains a direct write — undo for
                            // keyframe deletion is a separate follow-up.
                            track->removeKeyframe(m_keyframeEditFrame);
                        }
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace entity
