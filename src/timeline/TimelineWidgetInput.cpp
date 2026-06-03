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
#include "entity/components/SignalLayer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/Screen.hpp"
#include "entity/core/Engine.hpp"
#include "entity/project/ProjectManager.hpp"
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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
                registry.all_of<GenerativeLayer>(e)       ||
                registry.all_of<SignalLayer>(e))) {
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
                registry.all_of<GenerativeLayer>(e)       ||
                registry.all_of<SignalLayer>(e))) {
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
                    // Round the microsecond drag position to the nearest frame.
                    clip->startFrame = m_timeline->timeToFrame(newStartTime);
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
                                   registry.all_of<GenerativeLayer>(clipEntity)       ||
                                   registry.all_of<SignalLayer>(clipEntity))) {
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

TimelineWidget::KeyframeHit
TimelineWidget::findKeyframeAtPosition(ImVec2 mousePos, ImVec2 windowPos) const {
    KeyframeHit hit;
    if (!m_timeline) return hit;

    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();
    const double fps = m_timeline->getFrameRate();
    if (fps <= 0.0) return hit;

    // Hit zone = glyph half-size + a few px slack, matching the keyframe
    // size used in renderPropertyTracks.
    constexpr float kKfSize = 5.0f;
    constexpr float kHitPad = 3.0f;

    // Walk tracks with the same cumulative-Y layout as findClipAtPosition.
    float cumulativeY = 0.0f;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const entt::entity trackEntity = tracks[i];
        const float trackY = windowPos.y + cumulativeY;

        float trackHeight = TRACK_HEIGHT;
        const bool expanded =
            m_expandedTracks.count(static_cast<uint32_t>(trackEntity)) > 0 ||
            m_timeline->isTrackExpanded(trackEntity);
        entt::entity clipAtPlayhead = entt::null;
        if (expanded) {
            clipAtPlayhead = findClipAtPlayhead(trackEntity);
            if (clipAtPlayhead != entt::null) {
                trackHeight = TRACK_HEIGHT +
                    expandedPropertyRowCount(clipAtPlayhead) * PROPERTY_ROW_HEIGHT;
            }
        }

        if (clipAtPlayhead != entt::null) {
            const TimelinePlacement place = readPlacement(registry, clipAtPlayhead);
            const auto* animProps = registry.try_get<AnimatedProperties>(clipAtPlayhead);
            if (place.valid && animProps) {
                // Keyframe screen math is byte-identical to renderPropertyTracks.
                const float startSeconds =
                    static_cast<float>(place.startFrame) / static_cast<float>(fps);
                const float clipX = windowPos.x +
                    timeToPixel(static_cast<Timecode>(startSeconds * 1000000.0f));
                const float clipWidth = timeToPixel(static_cast<Timecode>(
                    (place.duration / static_cast<float>(fps)) * 1000000.0f));
                const float propY0 = trackY + TRACK_HEIGHT;

                const auto properties = propertyListForEntity(clipAtPlayhead);
                for (size_t p = 0; p < properties.size(); ++p) {
                    // Effect-keyframe drag-to-move + right-click is a
                    // Phase 4 follow-up. For now, skip non-Transform
                    // rows so the right-pane hit-test stays consistent
                    // with what renderPropertyTracks draws ring-able.
                    if (properties[p].source !=
                        TimelinePropertyDef::Source::Transform) continue;
                    const KeyframeTrack* kfTrack =
                        animProps->getTrack(properties[p].prop);
                    if (!kfTrack || !kfTrack->hasKeyframes()) continue;
                    const float rowY =
                        propY0 + static_cast<float>(p) * PROPERTY_ROW_HEIGHT;
                    const float keyframeY = rowY + PROPERTY_ROW_HEIGHT / 2.0f;
                    if (mousePos.y < keyframeY - kKfSize - kHitPad ||
                        mousePos.y > keyframeY + kKfSize + kHitPad) continue;
                    for (const auto& kf : kfTrack->keyframes) {
                        const float kfSeconds =
                            static_cast<float>(kf.frame) / static_cast<float>(fps);
                        const float kfX = clipX + kfSeconds * m_pixelsPerSecond;
                        if (kfX < clipX || kfX > clipX + clipWidth) continue;
                        if (mousePos.x >= kfX - kKfSize - kHitPad &&
                            mousePos.x <= kfX + kKfSize + kHitPad) {
                            hit.valid    = true;
                            hit.clip     = clipAtPlayhead;
                            hit.property = properties[p].prop;
                            hit.frame    = kf.frame;
                            return hit;
                        }
                    }
                }
            }
        }

        cumulativeY += trackHeight + TRACK_PADDING;
    }
    return hit;
}

bool TimelineWidget::isInPropertyRowBand(ImVec2 mousePos, ImVec2 windowPos) const {
    if (!m_timeline) return false;
    const auto& tracks = m_timeline->getTracks();

    float cumulativeY = 0.0f;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const entt::entity trackEntity = tracks[i];
        const float trackY = windowPos.y + cumulativeY;

        float trackHeight = TRACK_HEIGHT;
        float propPanelH = 0.0f;
        const bool expanded =
            m_expandedTracks.count(static_cast<uint32_t>(trackEntity)) > 0 ||
            m_timeline->isTrackExpanded(trackEntity);
        if (expanded) {
            entt::entity atPlayhead = findClipAtPlayhead(trackEntity);
            if (atPlayhead != entt::null) {
                propPanelH =
                    expandedPropertyRowCount(atPlayhead) * PROPERTY_ROW_HEIGHT;
                trackHeight += propPanelH;
            }
        }

        if (propPanelH > 0.0f) {
            const float propY0 = trackY + TRACK_HEIGHT;
            if (mousePos.y >= propY0 && mousePos.y <= propY0 + propPanelH) {
                return true;
            }
        }

        cumulativeY += trackHeight + TRACK_PADDING;
    }
    return false;
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
    ImVec2 mousePos = ImGui::GetMousePos();
    ImGuiIO& io = ImGui::GetIO();

    // Right edge of the interactive ruler. ImGui::GetContentRegionAvail() is
    // unreliable for this — ImGui caps the reported content region (the same
    // cap renderTimeRuler() works around for the ruler background), so past
    // ~65k content px the hover-highlight and right-click hit-test silently
    // died. Derive the extent from the timeline content width instead, with a
    // visible-width floor so a short timeline's ruler stays interactive.
    const float rulerRightEdge = windowPos.x +
        std::max(timeToPixel(m_timeline->getDuration()), m_lastVisibleWidth);

    // Cue lane sits ABOVE the ruler. windowPos.y == ruler top, so the
    // lane occupies [windowPos.y - m_cueLaneHeight, windowPos.y).
    const float laneTopY = windowPos.y - m_cueLaneHeight;
    bool overCueLane = (m_cueLaneHeight > 0.0f &&
                        mousePos.x >= windowPos.x &&
                        mousePos.x <= rulerRightEdge &&
                        mousePos.y >= laneTopY &&
                        mousePos.y <  windowPos.y);

    // Check if mouse is over the ruler window
    bool overRuler = (mousePos.x >= windowPos.x &&
                      mousePos.x <= rulerRightEdge &&
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
        const float relativeX = mousePos.x - windowPos.x;
        const FrameNumber currentFrame = m_timeline->timeToFrame(pixelToTime(relativeX));
        const FrameNumber tickEvery = static_cast<FrameNumber>(framesPerTick());
        m_hoverDivisionIndex = (tickEvery > 0 && currentFrame >= 0)
            ? static_cast<int>(currentFrame / tickEvery)
            : -1;
    } else {
        m_hoverDivisionIndex = -1;
    }

    // ── Cue lane gestures (issue 2026-05-25):
    //   single-click on label/triangle → seek playhead to cue.frame
    //   drag past io.MouseDragThreshold → move cue (EditCueCommand)
    //   double-click → open the Edit Cue modal
    // The full label rect is the gesture-initiation zone for all three.
    // Drag-from-the-label is intentional, so we can't just narrow the
    // hit-rect to "fix" the bare-click-moves-cue bug — we disambiguate
    // click from drag with a candidate-then-upgrade state machine on
    // m_cueClickCandidate.
    auto hitTestCueAt = [&](ImVec2 pos, double& outNumber,
                            FrameNumber& outFrame,
                            std::string& outLabel) -> bool {
        std::vector<CueLaneSlot> slots;
        int rowsUsed = 0;
        computeCueLaneLayout(slots, rowsUsed);
        const auto& cues = m_timeline->getCueTags();
        for (const CueLaneSlot& slot : slots) {
            const CueTag& cue = cues[slot.cueIndex];
            const float cx = windowPos.x + frameToPixel(cue.frame);
            const float rowTop = laneTopY + slot.row * kCueRowH;
            const float rowBot = rowTop + kCueRowH;
            if (pos.x >= cx && pos.x <= cx + slot.labelW &&
                pos.y >= rowTop && pos.y <= rowBot) {
                outNumber = cue.number;
                outFrame  = cue.frame;
                outLabel  = cue.label;
                return true;
            }
        }
        return false;
    };

    // (1) Mouse-down inside any cue's label rect → record a click
    // candidate. Do NOT set m_isDraggingCue yet; that flips only when
    // the gesture crosses io.MouseDragThreshold below.
    if (overCueLane && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !m_isDraggingCue && !m_cueClickCandidate.has_value()) {
        double      cueNum   = 0.0;
        FrameNumber cueFrame = 0;
        std::string cueLabel;
        if (hitTestCueAt(mousePos, cueNum, cueFrame, cueLabel)) {
            m_cueClickCandidate   = cueNum;
            m_dragOriginalCueTime = m_timeline->frameToTime(cueFrame);
            m_dragCurrentCueTime  = m_dragOriginalCueTime;
        }
    }

    // (2) Double-click on a cue label/triangle → open the edit modal.
    // ImGui fires this on the SECOND click of the pair; the first
    // click already ran (3)'s single-click seek-to-cue-frame branch on
    // its release. Clearing m_cueClickCandidate prevents the second
    // release from firing a redundant seek.
    if (overCueLane && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        double      cueNum   = 0.0;
        FrameNumber cueFrame = 0;
        std::string cueLabel;
        if (hitTestCueAt(mousePos, cueNum, cueFrame, cueLabel)) {
            m_cueModalMode      = CueModalMode::Edit;
            m_cueModalOldNumber = cueNum;
            m_cueModalNumber    = cueNum;
            m_cueModalFrame     = cueFrame;
            std::snprintf(m_cueModalLabelBuf, sizeof(m_cueModalLabelBuf),
                          "%s", cueLabel.c_str());
            m_cueModalOpenRequested = true;
            m_cueClickCandidate.reset();
            return;
        }
    }

    // (3) Click-candidate state machine. While held without crossing
    // the drag threshold we sit here. On release we resolve as a
    // single-click seek; on threshold-cross we upgrade to
    // m_isDraggingCue and the existing drag-update block (4) takes
    // over (same frame, via fall-through).
    if (m_cueClickCandidate.has_value() && !m_isDraggingCue) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // -1.0f → use io.MouseDragThreshold (default 6 px), the
            // natural "slight click vs real drag" boundary.
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, -1.0f)) {
                m_isDraggingCue    = true;
                m_draggedCueNumber = *m_cueClickCandidate;
                m_cueClickCandidate.reset();
                // Fall through to block (4) so the snap-update path
                // also runs on the upgrade frame.
            } else {
                // Still candidate — no drag yet, no seek yet. Suppress
                // ruler scrub for this frame and wait.
                return;
            }
        } else {
            // Released without ever crossing the drag threshold →
            // seek playhead to the cue's frame. Cue itself does NOT
            // move; no EditCueCommand.
            if (const CueTag* live = m_timeline->findCueTag(*m_cueClickCandidate)) {
                m_timeline->seekToFrame(live->frame);
            }
            m_cueClickCandidate.reset();
            m_dragOriginalCueTime = 0;
            m_dragCurrentCueTime  = 0;
            return;
        }
    }

    // (4) Active-drag update + release. Reached only after block (3)
    // promoted to m_isDraggingCue. While dragging we DON'T enqueue any
    // commands; renderCueLane() reads m_dragCurrentCueTime to draw the
    // cue at its visual position. Release commits one EditCueCommand.
    if (m_isDraggingCue) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float relativeX = mousePos.x - windowPos.x;
            Timecode t = pixelToTime(relativeX);
            if (t < 0) t = 0;

            // Snap pipeline mirroring clip drag — playhead + grid + other
            // cues + section breaks at SNAP_THRESHOLD_PIXELS tolerance.
            // Deliberately omits clip-edge snapping (cues are timeline-wide
            // markers; clip-edge snap would be noisy at high track counts).
            // Shift modifier bypasses grid snap only — matches clip drag
            // floorToGrid pattern.
            const bool gridSnap = !ImGui::GetIO().KeyShift;
            const Timecode tol = pixelToTime(SNAP_THRESHOLD_PIXELS);

            Timecode chosen = t;
            Timecode bestDist = tol + 1;
            auto consider = [&](Timecode candidate) {
                // Helpers return `t` unchanged when nothing snaps — skip to
                // avoid poisoning the ranker with distance-0 self-matches.
                if (candidate == t) return;
                const Timecode d = std::llabs(static_cast<long long>(candidate - t));
                if (d <= tol && d < bestDist) {
                    bestDist = d;
                    chosen = candidate;
                }
            };

            consider(m_timeline->getCurrentTime());     // playhead
            consider(snapTimeToCues(t, tol));           // self-skip via m_draggedCueNumber
            consider(snapTimeToSections(t, tol));
            if (gridSnap) consider(snapTimeToTickGrid(t));

            m_dragCurrentCueTime = chosen;
            m_isSnapping = (chosen != t);
            m_snapTargetTime = chosen;
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
                    newF, existingLabel);
                if (const CueTag* live = m_timeline->findCueTag(m_draggedCueNumber)) {
                    cmd->setPreviousState(*live);
                }
                m_commandDispatcher->enqueue(std::move(cmd));
            }
            m_isDraggingCue = false;
            m_draggedCueNumber = 0.0;
            m_dragOriginalCueTime = 0;
            m_dragCurrentCueTime = 0;
            m_isSnapping = false;
            m_snapTargetTime = 0;
        }
        return;  // Suppress ruler scrub / range while a cue drag is active.
    }

    // Shift+click on the ruler starts a range selection. Plain click keeps
    // the existing scrub gesture and clears any prior range so the user can
    // get back to scrubbing without hunting for a clear command.
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float relativeX = mousePos.x - windowPos.x;
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
            // Plain ruler click also clears any selection — established
            // editing tools behave this way so the user isn't left wondering why the
            // ripple-delete shortcut is greyed out.
            m_range = {};
        }
    }

    if (m_isCreatingRange) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float relativeX = mousePos.x - windowPos.x;
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
            float relativeX = mousePos.x - windowPos.x;
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
        float relativeX = mousePos.x - windowPos.x;
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
            const float cx = windowPos.x + frameToPixel(cue.frame);
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
        float relativeX = mousePos.x - windowPos.x;
        Timecode clickTime = pixelToTime(relativeX);
        if (clickTime < 0) clickTime = 0;
        m_rulerRightClickTime = clickTime;

        // Section break hit-test (Phase B). Hit-tests against the rendered
        // break line at ±6px. Selects the captured break frame for the
        // popup; vector indices shift on edit, so we key by frame.
        m_rightClickedSectionBreakFrame.reset();
        const float kBreakHitPx = 6.0f;
        const auto& sections = m_timeline->getSections();
        for (const auto& sec : sections) {
            const float bx = windowPos.x + frameToPixel(sec.breakFrame);
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
                    m_timeline->timeToFrame(m_range.start), 0xFF6090C8u, 0.0));
                m_commandDispatcher->enqueue(std::make_unique<AddSectionBreakCommand>(
                    m_timeline->timeToFrame(m_range.end), 0xFF6090C8u, 0.0));
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
            const FrameNumber brkT = *m_rightClickedSectionBreakFrame;
            const Timeline::Section* live = m_timeline->findSectionBreakNear(brkT, 0);
            if (live) {
                char hdr[64];
                std::snprintf(hdr, sizeof(hdr), "Break @ F%lld",
                              static_cast<long long>(live->breakFrame));
                ImGui::TextDisabled("%s", hdr);
                ImGui::Separator();
                if (ImGui::MenuItem("Jump to Break") && m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<SeekCommand>(
                        m_timeline->frameToTime(brkT)));
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
                m_cueModalFrame = cue.frame;
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
            // Snap to the highlighted tick-grid cell so the break lands on the
            // front line of the division the hover band is showing.
            // snapTimeToBest is avoided here: it would prefer an existing
            // nearby section break, producing a near-duplicate.
            const FrameNumber at =
                m_timeline->timeToFrame(snapTimeToTickGrid(m_rulerRightClickTime));
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
            m_cueModalFrame = m_timeline->timeToFrame(snapTimeToTickGrid(m_rulerRightClickTime));
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
            m_cueModalFrame = m_timeline->timeToFrame(snapTimeToTickGrid(m_rulerRightClickTime));
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
            float relativeX = mousePos.x - windowPos.x;
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
                    // Round the microsecond mouse position to the nearest frame.
                    FrameNumber mouseFrame = m_timeline->timeToFrame(mouseTime);

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

                            // Pure timeline resize: do NOT slip mediaStartFrame.
                            // Source in/out is the PropertyWindow's domain (slip edits
                            // route through SetClipMediaStartFrameCommand). Timeline-edge
                            // drag only changes the clip's timeline footprint
                            // (startFrame + duration) — the clip's source-window
                            // anchor (mediaStartFrame) is left intact so the same
                            // source frame still plays at frame 0 of the clip.
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

    // Active keyframe drag — owns all track interaction until release. The
    // keyframe data is not mutated mid-drag; renderPropertyTracks draws a live
    // preview from m_dragKeyframeCurrentFrame and a single undoable
    // MoveKeyframeCommand is committed on mouse-up.
    if (m_isDraggingKeyframe) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (registry.valid(m_dragKeyframeClip)) {
                const double fps = m_timeline->getFrameRate();
                const TimelinePlacement place =
                    readPlacement(registry, m_dragKeyframeClip);
                const float startSeconds =
                    static_cast<float>(place.startFrame) / static_cast<float>(fps);
                const float clipX = windowPos.x +
                    timeToPixel(static_cast<Timecode>(startSeconds * 1000000.0f));
                const float pxPerFrame = (fps > 0.0)
                    ? m_pixelsPerSecond / static_cast<float>(fps) : 1.0f;

                FrameNumber newFrame = m_dragKeyframeOriginalFrame;
                if (pxPerFrame > 0.0f) {
                    newFrame = static_cast<FrameNumber>(
                        std::lround((mousePos.x - clipX) / pxPerFrame));
                }

                // Clamp to the clip's frame range and to the gap between the
                // adjacent keyframes on the same track — a drag can't cross
                // or collide with another keyframe.
                FrameNumber lo = 0;
                FrameNumber hi = (place.duration > 0) ? place.duration - 1 : 0;
                if (const auto* animProps =
                        registry.try_get<AnimatedProperties>(m_dragKeyframeClip)) {
                    if (const KeyframeTrack* kt =
                            animProps->getTrack(m_dragKeyframeProperty)) {
                        for (const auto& kf : kt->keyframes) {
                            if (kf.frame < m_dragKeyframeOriginalFrame)
                                lo = std::max(lo, kf.frame + 1);
                            else if (kf.frame > m_dragKeyframeOriginalFrame)
                                hi = std::min(hi, kf.frame - 1);
                        }
                    }
                }
                if (hi < lo) hi = lo;
                m_dragKeyframeCurrentFrame = std::clamp(newFrame, lo, hi);
            }
        } else {
            // Release — commit a single undoable MoveKeyframeCommand.
            if (m_dragKeyframeCurrentFrame != m_dragKeyframeOriginalFrame &&
                m_commandDispatcher) {
                if (auto idx = findClipIndices(m_timeline, m_dragKeyframeClip)) {
                    m_commandDispatcher->enqueue(std::make_unique<MoveKeyframeCommand>(
                        idx->first, idx->second, m_dragKeyframeProperty,
                        m_dragKeyframeOriginalFrame, m_dragKeyframeCurrentFrame));
                    m_selectedKeyframeClip     = m_dragKeyframeClip;
                    m_selectedKeyframeProperty = m_dragKeyframeProperty;
                    m_selectedKeyframeFrame    = m_dragKeyframeCurrentFrame;
                }
            }
            m_isDraggingKeyframe = false;
            m_dragKeyframeClip   = entt::null;
        }
        return;
    }

    // Handle clip dragging (continue even if not hovered, to allow drag outside window)
    if (m_isDraggingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate desired new clip position based on mouse
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX - m_dragOffsetX;
            Timecode desiredStartTime = pixelToTime(relativeX);

            // Clamp to valid range (>= 0)
            if (desiredStartTime < 0) desiredStartTime = 0;

            // Live target track — what track the cursor is currently over.
            // Collision check + same-track snap candidates now key off this
            // (not m_selectedClipTrackIndex, the source) so cross-track
            // drags can't land overlapping a clip on the destination track.
            // Fallback to source track if cursor is outside the lane region.
            int liveTargetTrack = findTrackAtY(mousePos.y, windowPos.y);
            if (liveTargetTrack < 0) liveTargetTrack = m_selectedClipTrackIndex;

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

                    // Other clips on the LIVE TARGET track — their edges,
                    // both matched against our start and our end. Both
                    // Clip-backed and Layer-only (OA / Generative) entities
                    // contribute snap targets, read uniformly via
                    // readPlacement(). Keyed off liveTargetTrack so during
                    // cross-track drag we snap to the destination's edges,
                    // not the source's.
                    if (liveTargetTrack >= 0) {
                        const auto& tracks = m_timeline->getTracks();
                        if (liveTargetTrack < static_cast<int>(tracks.size())) {
                            const auto* track = registry.try_get<TimelineTrack>(tracks[liveTargetTrack]);
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

            // Check for collisions and snap to valid position. Use the live
            // target track so a cross-track drag commits to a position that
            // doesn't overlap on the destination — no more "auto-correct
            // on next click" after release.
            Timecode newStartTime = checkClipCollision(m_selectedClip, desiredStartTime, liveTargetTrack);

            // Update placement (start frame). Routes to Clip or Layer-only
            // storage depending on the entity's archetype.
            if (registry.valid(m_selectedClip)) {
                // Round the microsecond drag position to the nearest frame so a
                // clip snapped to a section break / cue / grid line lands exactly
                // on it (truncation would land it one frame early).
                FrameNumber newStartFrame = m_timeline->timeToFrame(newStartTime);
                writeStartFrame(registry, m_selectedClip, newStartFrame);
            }

            // Ghost preview — only show on cross-track drag (the actual
            // clip is still rendered on its source track, so the ghost
            // gives the user a preview of the destination landing site).
            if (liveTargetTrack >= 0 && liveTargetTrack != m_selectedClipTrackIndex
                && movingPlace.valid) {
                const double fps = m_timeline->getFrameRate();
                const float durSec = movingPlace.duration / static_cast<float>(fps);
                const Timecode durTime = static_cast<Timecode>(durSec * 1000000.0f);
                m_ghost.active     = true;
                m_ghost.trackIndex = liveTargetTrack;
                m_ghost.startTime  = newStartTime;
                m_ghost.duration   = durTime;
                m_ghost.valid      = !wouldOverlapAnyClip(liveTargetTrack, newStartTime,
                                                         durTime, m_selectedClip);
                m_ghost.label.clear();   // ghost IS the clip, no extra label
            } else {
                m_ghost.active = false;
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
            m_ghost.active = false;  // Clear drop ghost
            m_timeline->setScrubbing(false);  // Exit scrubbing mode - triggers final seek
        }
    } else if (!m_isDraggingRuler && isWindowHovered) {
        // Keyframe select + start drag. Checked before clip-edge / clip
        // selection so a click on a keyframe diamond in an expanded property
        // row never starts a clip drag (the property panel is part of the
        // clip's hit zone). An empty click inside a property row is a no-op
        // for the same reason — it must not fall through to clip drag.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            KeyframeHit kfHit = findKeyframeAtPosition(mousePos, windowPos);
            if (kfHit.valid) {
                m_selectedKeyframeClip      = kfHit.clip;
                m_selectedKeyframeProperty  = kfHit.property;
                m_selectedKeyframeFrame     = kfHit.frame;
                m_isDraggingKeyframe        = true;
                m_dragKeyframeClip          = kfHit.clip;
                m_dragKeyframeProperty      = kfHit.property;
                m_dragKeyframeOriginalFrame = kfHit.frame;
                m_dragKeyframeCurrentFrame  = kfHit.frame;
                return;
            }
            if (isInPropertyRowBand(mousePos, windowPos)) {
                m_selectedKeyframeClip = entt::null;
                return;
            }
        }

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
                m_selectedKeyframeClip = entt::null;  // selecting a clip clears keyframe selection
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
                m_selectedKeyframeClip = entt::null;

                // Sync deselection to Timeline
                m_timeline->setSelectedClip(entt::null);
            }
        }

        // Handle right-click for context menus (only if window is hovered).
        // We DEFER the actual ImGui::OpenPopup call to handleContextMenus()
        // because popup IDs are scoped to the current ImGui window context.
        // We're inside the TimelineTracks BeginChild here, but
        // BeginPopup() runs in the parent (TimelineWindow) context — they
        // wouldn't match if we opened here. Setting the m_show* flag is
        // safe across the EndChild boundary.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // Right-click on a keyframe diamond → interpolation context menu,
            // routed through the same hit-test as left-click select/drag.
            KeyframeHit kfHit = findKeyframeAtPosition(mousePos, windowPos);
            if (kfHit.valid) {
                m_keyframeEditClip     = kfHit.clip;
                m_keyframeEditProperty = kfHit.property;
                m_keyframeEditFrame    = kfHit.frame;
                m_showKeyframeContextMenu = true;
                m_showClipContextMenu  = false;
                m_showTrackContextMenu = false;
            } else {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                m_rightClickedClip = clipUnderMouse;
                m_rightClickedTrackIndex = trackIndex;
                m_showClipContextMenu = true;
                m_showTrackContextMenu = false;
            } else {
                int trackAtY = findTrackAtY(mousePos.y, windowPos.y);
                if (trackAtY >= 0) {
                    m_rightClickedTrackIndex = trackAtY;
                    m_rightClickedClip = entt::null;
                    m_showTrackContextMenu = true;
                    m_showClipContextMenu = false;

                    // Capture click frame on the timeline for the
                    // Paste-here menu item. Floor-snap to the current
                    // tick grid so the inserted clip's start aligns
                    // with the visible tick at-or-before the cursor.
                    const float relativeX =
                        mousePos.x - windowPos.x;
                    Timecode clickTime = pixelToTime(relativeX);
                    if (clickTime < 0) clickTime = 0;
                    clickTime = snapTimeToTickGrid(clickTime);
                    m_rightClickedTrackFrame = m_timeline->timeToFrame(clickTime);
                }
            }
            }  // end else (right-click did not land on a keyframe)
        }
    }
}

void TimelineWidget::handleContextMenus() {
    if (!m_timeline) return;

    // Open the popups deferred from handleTracksInteraction. Doing the
    // ImGui::OpenPopup here (parent window context) matches the BeginPopup
    // call below — popup IDs are scoped to the current ImGui window, so
    // opening + beginning in the same window context is required.
    if (m_showClipContextMenu) {
        ImGui::OpenPopup("ClipContextMenu");
        m_showClipContextMenu = false;
    }
    if (m_showTrackContextMenu) {
        ImGui::OpenPopup("TrackContextMenu");
        m_showTrackContextMenu = false;
    }

    // Clip context menu — Cut / Copy / Paste / Duplicate / Delete plus
    // Change Media... and a Route to... submenu. ASCII-only labels per
    // the ImGui default-font constraint.
    if (ImGui::BeginPopup("ClipContextMenu")) {
        if (m_rightClickedClip != entt::null) {
            auto& registry = m_timeline->getRegistry();
            const auto* clip = registry.try_get<Clip>(m_rightClickedClip);
            const auto idx = findClipIndices(m_timeline, m_rightClickedClip);
            const bool hasIdx = idx.has_value();
            const bool isClipKind = (clip != nullptr);
            // Any layer with a Layer component (Clip or Generative) is copyable.
            // OA layers are excluded: their targetScreen ref isn't meaningful
            // on paste to an unrelated session state.
            const bool isCopyable = hasIdx &&
                registry.all_of<Layer>(m_rightClickedClip) &&
                !registry.all_of<ObjectAnimationLayer>(m_rightClickedClip);
            const bool clipboardFull =
                m_engine && m_engine->clipClipboard().has_value() &&
                m_engine->clipClipboard()->valid;

            if (ImGui::MenuItem("Cut", "Ctrl+X", false, isCopyable)) {
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<CutClipCommand>(
                        static_cast<uint32_t>(m_rightClickedClip)));
                }
            }
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, isCopyable)) {
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<CopyClipCommand>(
                        static_cast<uint32_t>(m_rightClickedClip)));
                }
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, clipboardFull)) {
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<PasteClipCommand>());
                }
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, isClipKind && hasIdx)) {
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<DuplicateClipCommand>(
                        static_cast<uint32_t>(m_rightClickedClip)));
                }
            }
            if (ImGui::MenuItem("Delete", "Del", false, hasIdx)) {
                if (m_selectedClip == m_rightClickedClip) {
                    m_selectedClip = entt::null;
                    m_selectedClipTrackIndex = -1;
                }
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<DeleteClipCommand>(
                        static_cast<uint32_t>(m_rightClickedClip)));
                } else {
                    m_timeline->deleteClip(m_rightClickedClip);
                }
            }

            ImGui::Separator();

            // Change Media... — only for clip-kind layers (not OA/Generative).
            if (ImGui::MenuItem("Change Media...", nullptr, false, isClipKind && hasIdx)) {
                m_pendingChangeMediaClip = m_rightClickedClip;
                m_openChangeMediaPopup = true;
                m_changeMediaFilterBuf[0] = '\0';
            }

            // Route to... submenu — all routing kinds (Direct / Tiled /
            // FeedMap). Auto-direct entries first (one per Screen), then
            // user-created. ASCII kind badges per ImGui font constraint.
            const bool canRoute = isClipKind && hasIdx;
            if (canRoute && ImGui::BeginMenu("Route to...")) {
                const auto* currentRef = registry.try_get<ContentRoutingRef>(m_rightClickedClip);
                const entt::entity currentAsset = currentRef ? currentRef->asset : entt::null;

                // Default (All visible) — clears the routing ref.
                if (ImGui::MenuItem("[*] Default (All visible)", nullptr, currentAsset == entt::null)) {
                    if (m_commandDispatcher) {
                        m_commandDispatcher->enqueue(std::make_unique<SetClipTargetScreenCommand>(
                            idx->first, idx->second, std::string("All Screens")));
                    }
                }

                struct AssetPick {
                    entt::entity asset{entt::null};
                    std::string  display;
                    RouteMode    kind{RouteMode::Direct};
                    bool         isAuto{false};
                };
                std::vector<AssetPick> autoDirect, userCreated;
                for (auto [e, a] : registry.view<ContentRoutingAsset>().each()) {
                    AssetPick p;
                    p.asset   = e;
                    p.display = a.name;
                    p.kind    = a.kind;
                    p.isAuto  = (a.autoBoundScreen != entt::null);
                    (p.isAuto ? autoDirect : userCreated).push_back(std::move(p));
                }
                auto byName = [](const AssetPick& a, const AssetPick& b) { return a.display < b.display; };
                std::sort(autoDirect.begin(), autoDirect.end(), byName);
                std::sort(userCreated.begin(), userCreated.end(), byName);

                auto kindTag = [](RouteMode m) -> const char* {
                    switch (m) {
                        case RouteMode::Direct:  return "[D] ";
                        case RouteMode::Tiled:   return "[T] ";
                        case RouteMode::FeedMap: return "[F] ";
                    }
                    return "";
                };

                auto applyPick = [&](const AssetPick& p) {
                    if (!m_commandDispatcher) return;
                    const auto* asset = registry.try_get<ContentRoutingAsset>(p.asset);
                    // Direct with single target → SetClipTargetScreenCommand (matches
                    // existing PropertyWindow dispatch + lets undo capture the
                    // previous screen name as a string).
                    if (asset && asset->kind == RouteMode::Direct &&
                        asset->targets.size() == 1) {
                        std::string screenName = "All Screens";
                        if (auto* sc = registry.try_get<Screen>(asset->targets.front().screen)) {
                            screenName = sc->name;
                        }
                        m_commandDispatcher->enqueue(std::make_unique<SetClipTargetScreenCommand>(
                            idx->first, idx->second, screenName));
                        return;
                    }
                    // Tiled / FeedMap / multi-target Direct → SetContentRoutingCommand
                    // with the asset's full spec.
                    if (asset) {
                        const char* modeStr = "Direct";
                        switch (asset->kind) {
                            case RouteMode::Direct:  modeStr = "Direct";  break;
                            case RouteMode::Tiled:   modeStr = "Tiled";   break;
                            case RouteMode::FeedMap: modeStr = "FeedMap"; break;
                        }
                        std::vector<SetContentRoutingCommand::TargetSpec> specs;
                        specs.reserve(asset->targets.size());
                        for (const auto& t : asset->targets) {
                            SetContentRoutingCommand::TargetSpec s;
                            s.uvRect = t.uvRect;
                            if (t.screen != entt::null) {
                                if (auto* sc = registry.try_get<Screen>(t.screen)) {
                                    s.screenName = sc->name;
                                }
                            }
                            specs.push_back(std::move(s));
                        }
                        m_commandDispatcher->enqueue(std::make_unique<SetContentRoutingCommand>(
                            idx->first, idx->second, std::string(modeStr), std::move(specs)));
                    }
                };

                if (!autoDirect.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Auto (per Screen)");
                    for (const auto& p : autoDirect) {
                        std::string label = kindTag(p.kind);
                        label += p.display;
                        if (ImGui::MenuItem(label.c_str(), nullptr, p.asset == currentAsset)) {
                            applyPick(p);
                        }
                    }
                }
                if (!userCreated.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("User-created");
                    for (const auto& p : userCreated) {
                        std::string label = kindTag(p.kind);
                        label += p.display;
                        if (ImGui::MenuItem(label.c_str(), nullptr, p.asset == currentAsset)) {
                            applyPick(p);
                        }
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // Bottom info block — Clip kind only; OA/Generative have their
            // own readouts in PropertyWindow.
            if (clip) {
                ImGui::TextDisabled("Duration: %d frames", clip->duration);
                ImGui::TextDisabled("Start: frame %d", clip->startFrame);
            }
        }
        ImGui::EndPopup();
    }

    // Modal popup (separate so it survives across frames even after the
    // context popup closes).
    renderChangeMediaPopup();

    // Track context menu
    if (ImGui::BeginPopup("TrackContextMenu")) {
        if (m_rightClickedTrackIndex >= 0) {
            const bool clipboardFull =
                m_engine && m_engine->clipClipboard().has_value() &&
                m_engine->clipClipboard()->valid;

            // Paste here — uses the click-frame captured during right-
            // click (floor-snapped to tick grid).
            {
                std::ostringstream label;
                label << "Paste at frame " << m_rightClickedTrackFrame;
                if (ImGui::MenuItem(label.str().c_str(), "Ctrl+V", false, clipboardFull)) {
                    if (m_commandDispatcher) {
                        m_commandDispatcher->enqueue(std::make_unique<PasteClipCommand>(
                            m_rightClickedTrackFrame, m_rightClickedTrackIndex));
                    }
                }
            }

            ImGui::Separator();

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
                            // Undoable via RemoveKeyframeCommand; direct-write
                            // fallback for the no-dispatcher (script / test)
                            // embedding, mirroring setInterp above.
                            if (m_commandDispatcher && idx) {
                                m_commandDispatcher->enqueue(
                                    std::make_unique<RemoveKeyframeCommand>(
                                        idx->first, idx->second,
                                        m_keyframeEditProperty, m_keyframeEditFrame));
                            } else {
                                track->removeKeyframe(m_keyframeEditFrame);
                            }
                            // Drop the timeline keyframe selection if it
                            // pointed at the keyframe just deleted.
                            if (m_selectedKeyframeClip == m_keyframeEditClip &&
                                m_selectedKeyframeProperty == m_keyframeEditProperty &&
                                m_selectedKeyframeFrame == m_keyframeEditFrame) {
                                m_selectedKeyframeClip = entt::null;
                            }
                        }
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
}

void TimelineWidget::renderChangeMediaPopup() {
    if (m_openChangeMediaPopup) {
        ImGui::OpenPopup("Change Media");
        m_openChangeMediaPopup = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Change Media", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // Bail out early if we lost the prerequisites between open + render.
    if (m_pendingChangeMediaClip == entt::null || !m_engine || !m_engine->getProjectManager()) {
        ImGui::TextDisabled("No clip selected.");
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            m_pendingChangeMediaClip = entt::null;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    // Show what we're retargeting for context.
    auto& registry = m_timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(m_pendingChangeMediaClip);
    if (clip) {
        ImGui::TextDisabled("Current:");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(clip->filepath.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##change_media_filter", "Search media...",
                              m_changeMediaFilterBuf, sizeof(m_changeMediaFilterBuf));

    ImGui::Separator();

    // Case-insensitive substring match on the filename leaf.
    auto icontains = [](std::string_view hay, std::string_view needle) {
        if (needle.empty()) return true;
        if (needle.size() > hay.size()) return false;
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < needle.size(); ++j) {
                if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                    std::tolower(static_cast<unsigned char>(needle[j]))) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    };

    const std::string_view filter{m_changeMediaFilterBuf};
    const auto& mediaFiles = m_engine->getLoadedMediaFiles();

    bool chose = false;
    std::string chosenPath;

    if (ImGui::BeginChild("##change_media_list", ImVec2(420, 280), true)) {
        for (const auto& entry : mediaFiles) {
            std::filesystem::path p{entry.originalPath};
            const std::string leaf = p.filename().string();
            if (!filter.empty() && !icontains(leaf, filter)) continue;
            const bool isCurrent = (clip && entry.originalPath == clip->filepath);
            ImGui::PushID(entry.originalPath.c_str());
            if (ImGui::Selectable(leaf.c_str(), isCurrent,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (!isCurrent) {
                    chose = true;
                    chosenPath = entry.originalPath;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", entry.originalPath.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        m_pendingChangeMediaClip = entt::null;
        ImGui::CloseCurrentPopup();
    }

    if (chose) {
        if (auto idx = findClipIndices(m_timeline, m_pendingChangeMediaClip);
            idx.has_value() && m_commandDispatcher) {
            m_commandDispatcher->enqueue(std::make_unique<SetClipMediaCommand>(
                idx->first, idx->second, chosenPath));
        }
        m_pendingChangeMediaClip = entt::null;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace entity
